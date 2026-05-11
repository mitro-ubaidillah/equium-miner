/**
 * Jito Anti-MEV Module
 *
 * Sends mining transactions via Jito bundle to prevent:
 * 1. Front-running (validator sees your solution and submits their own)
 * 2. Sandwich attacks
 * 3. Transaction reordering by malicious validators
 *
 * CONDITIONAL TIP STRATEGY:
 * We send a 2-tx bundle:
 *   TX1: mine instruction (no tip)
 *   TX2: tip transfer to Jito
 *
 * Because Jito bundles are ATOMIC (all-or-nothing):
 * - If TX1 (mine) succeeds → TX2 (tip) executes → tip paid ✓
 * - If TX1 (mine) fails (stale challenge, etc) → entire bundle reverts → NO tip paid ✓
 *
 * This saves ~0.0001 SOL per failed attempt.
 */

import {
  Connection,
  Keypair,
  PublicKey,
  SystemProgram,
  Transaction,
  TransactionInstruction,
  ComputeBudgetProgram,
} from "@solana/web3.js";
import bs58 from "bs58";
import { JITO_TIP_ACCOUNTS, JITO_ENDPOINTS } from "./constants.js";

// Pick a random tip account to distribute load
function getRandomTipAccount(): PublicKey {
  const idx = Math.floor(Math.random() * JITO_TIP_ACCOUNTS.length);
  return JITO_TIP_ACCOUNTS[idx];
}

// Pick a random Jito endpoint for redundancy
function getJitoEndpoint(): string {
  const endpoints = Object.values(JITO_ENDPOINTS);
  const idx = Math.floor(Math.random() * endpoints.length);
  return endpoints[idx];
}

/**
 * Create a standalone tip transaction (TX2 in the bundle)
 */
function createTipTransaction(
  payer: PublicKey,
  tipLamports: number,
  blockhash: string
): Transaction {
  const tx = new Transaction();
  tx.add(
    SystemProgram.transfer({
      fromPubkey: payer,
      toPubkey: getRandomTipAccount(),
      lamports: tipLamports,
    })
  );
  tx.recentBlockhash = blockhash;
  tx.feePayer = payer;
  return tx;
}

/**
 * Send a transaction via Jito bundle with CONDITIONAL TIP.
 *
 * Bundle structure:
 *   [TX1: mine instruction] → [TX2: Jito tip]
 *
 * If TX1 fails on-chain, the entire bundle is rejected by Jito
 * and TX2 (tip) never executes. You only pay when you win.
 */
export async function sendWithJitoBundle(
  connection: Connection,
  transaction: Transaction,
  signer: Keypair,
  tipLamports: number = 100_000, // 0.0001 SOL default tip
  jitoEndpoint?: string
): Promise<string> {
  const endpoint = jitoEndpoint || getJitoEndpoint();

  // Get recent blockhash (shared by both transactions)
  const { blockhash, lastValidBlockHeight } =
    await connection.getLatestBlockhash("confirmed");

  // ─── TX1: Mine transaction (with compute budget, NO tip) ───
  const computeIx = ComputeBudgetProgram.setComputeUnitLimit({
    units: 400_000,
  });
  const priorityIx = ComputeBudgetProgram.setComputeUnitPrice({
    microLamports: 50_000,
  });

  transaction.instructions = [
    computeIx,
    priorityIx,
    ...transaction.instructions,
  ];

  transaction.recentBlockhash = blockhash;
  transaction.feePayer = signer.publicKey;
  transaction.lastValidBlockHeight = lastValidBlockHeight;
  transaction.sign(signer);

  const serializedTx1 = bs58.encode(
    transaction.serialize({ requireAllSignatures: true })
  );

  // ─── TX2: Tip transaction (only executes if TX1 succeeds) ───
  const tipTx = createTipTransaction(signer.publicKey, tipLamports, blockhash);
  tipTx.lastValidBlockHeight = lastValidBlockHeight;
  tipTx.sign(signer);

  const serializedTx2 = bs58.encode(
    tipTx.serialize({ requireAllSignatures: true })
  );

  // ─── Send as atomic bundle [TX1, TX2] ───
  const bundleId = await sendBundle(endpoint, [serializedTx1, serializedTx2]);

  if (!bundleId) {
    // Fallback: send via Jito sendTransaction (single tx with tip included)
    // In fallback mode, tip is NOT conditional (old behavior)
    console.log("  ⚠ Bundle failed, falling back to single-tx (tip not conditional)");
    return await sendFallbackWithTip(connection, transaction, signer, tipLamports, endpoint);
  }

  // Poll for bundle status
  const signature = await pollBundleStatus(endpoint, bundleId, connection);
  return signature;
}

/**
 * Fallback: send single transaction with tip embedded (non-conditional)
 * Used when bundle submission fails.
 */
async function sendFallbackWithTip(
  connection: Connection,
  transaction: Transaction,
  signer: Keypair,
  tipLamports: number,
  endpoint: string
): Promise<string> {
  // Re-build transaction with tip appended
  const tipIx = SystemProgram.transfer({
    fromPubkey: signer.publicKey,
    toPubkey: getRandomTipAccount(),
    lamports: tipLamports,
  });
  transaction.add(tipIx);

  const { blockhash, lastValidBlockHeight } =
    await connection.getLatestBlockhash("confirmed");
  transaction.recentBlockhash = blockhash;
  transaction.feePayer = signer.publicKey;
  transaction.lastValidBlockHeight = lastValidBlockHeight;

  // Need to re-sign since we modified the transaction
  transaction.signatures = [];
  transaction.sign(signer);

  const serializedTx = bs58.encode(
    transaction.serialize({ requireAllSignatures: true })
  );

  return await sendViaJitoTransaction(endpoint, serializedTx);
}

/**
 * Send bundle to Jito block engine
 */
async function sendBundle(
  endpoint: string,
  transactions: string[]
): Promise<string | null> {
  try {
    const response = await fetch(`${endpoint}/bundles`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        jsonrpc: "2.0",
        id: 1,
        method: "sendBundle",
        params: [transactions],
      }),
    });

    const data = (await response.json()) as any;

    if (data.error) {
      console.log(`  ⚠ Jito bundle error: ${data.error.message}`);
      return null;
    }

    return data.result as string;
  } catch (err: any) {
    console.log(`  ⚠ Jito bundle failed: ${err.message}`);
    return null;
  }
}

/**
 * Send via Jito's sendTransaction endpoint (private mempool, no bundle)
 */
async function sendViaJitoTransaction(
  endpoint: string,
  serializedTx: string
): Promise<string> {
  const response = await fetch(`${endpoint}/transactions`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      jsonrpc: "2.0",
      id: 1,
      method: "sendTransaction",
      params: [serializedTx, { encoding: "base58" }],
    }),
  });

  const data = (await response.json()) as any;

  if (data.error) {
    throw new Error(`Jito sendTransaction failed: ${data.error.message}`);
  }

  return data.result as string;
}

/**
 * Poll bundle status until landed or expired
 */
async function pollBundleStatus(
  endpoint: string,
  bundleId: string,
  connection: Connection,
  maxRetries: number = 30
): Promise<string> {
  for (let i = 0; i < maxRetries; i++) {
    await sleep(2000);

    try {
      const response = await fetch(`${endpoint}/bundles`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          jsonrpc: "2.0",
          id: 1,
          method: "getBundleStatuses",
          params: [[bundleId]],
        }),
      });

      const data = (await response.json()) as any;

      if (data.result?.value?.[0]) {
        const status = data.result.value[0];
        if (
          status.confirmation_status === "confirmed" ||
          status.confirmation_status === "finalized"
        ) {
          // Return the first transaction signature (mine tx)
          if (status.transactions?.length > 0) {
            return status.transactions[0];
          }
        }
        if (status.err) {
          throw new Error(`Bundle failed: ${JSON.stringify(status.err)}`);
        }
      }
    } catch (err: any) {
      if (err.message?.includes("Bundle failed")) throw err;
      // Continue polling on network errors
    }
  }

  throw new Error(`Bundle ${bundleId} did not land within timeout`);
}

/**
 * Fallback: send directly via RPC (no MEV protection, no Jito)
 * Used when Jito is completely unavailable.
 */
export async function sendDirectTransaction(
  connection: Connection,
  transaction: Transaction,
  signer: Keypair
): Promise<string> {
  const { blockhash, lastValidBlockHeight } =
    await connection.getLatestBlockhash("confirmed");
  transaction.recentBlockhash = blockhash;
  transaction.feePayer = signer.publicKey;
  transaction.lastValidBlockHeight = lastValidBlockHeight;

  // Add priority fee even without Jito
  const priorityIx = ComputeBudgetProgram.setComputeUnitPrice({
    microLamports: 100_000,
  });
  const computeIx = ComputeBudgetProgram.setComputeUnitLimit({
    units: 400_000,
  });
  transaction.instructions = [
    computeIx,
    priorityIx,
    ...transaction.instructions,
  ];

  transaction.sign(signer);

  const signature = await connection.sendRawTransaction(
    transaction.serialize(),
    {
      skipPreflight: false,
      preflightCommitment: "confirmed",
      maxRetries: 3,
    }
  );

  await connection.confirmTransaction(
    { signature, blockhash, lastValidBlockHeight },
    "confirmed"
  );

  return signature;
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
