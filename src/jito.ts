/**
 * Jito Anti-MEV Module
 *
 * Sends mining transactions via Jito bundle to prevent:
 * 1. Front-running (validator sees your solution and submits their own)
 * 2. Sandwich attacks
 * 3. Transaction reordering by malicious validators
 *
 * How it works:
 * - Transaction is sent as a Jito bundle (atomic execution)
 * - Includes a tip to Jito validator for priority inclusion
 * - Bundle is processed privately (not visible in public mempool)
 */

import {
  Connection,
  Keypair,
  PublicKey,
  SystemProgram,
  Transaction,
  TransactionInstruction,
  VersionedTransaction,
  TransactionMessage,
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
 * Create a tip instruction to Jito validator
 */
export function createJitoTipInstruction(
  payer: PublicKey,
  tipLamports: number
): TransactionInstruction {
  return SystemProgram.transfer({
    fromPubkey: payer,
    toPubkey: getRandomTipAccount(),
    lamports: tipLamports,
  });
}

/**
 * Send a transaction via Jito bundle for MEV protection
 */
export async function sendWithJitoBundle(
  connection: Connection,
  transaction: Transaction,
  signer: Keypair,
  tipLamports: number = 100_000, // 0.0001 SOL default tip
  jitoEndpoint?: string
): Promise<string> {
  const endpoint = jitoEndpoint || getJitoEndpoint();

  // Add compute budget for priority
  const computeIx = ComputeBudgetProgram.setComputeUnitLimit({
    units: 400_000,
  });
  const priorityIx = ComputeBudgetProgram.setComputeUnitPrice({
    microLamports: 50_000, // Priority fee
  });

  // Add Jito tip instruction
  const tipIx = createJitoTipInstruction(signer.publicKey, tipLamports);

  // Prepend compute budget and append tip
  transaction.instructions = [
    computeIx,
    priorityIx,
    ...transaction.instructions,
    tipIx,
  ];

  // Get recent blockhash
  const { blockhash, lastValidBlockHeight } =
    await connection.getLatestBlockhash("confirmed");
  transaction.recentBlockhash = blockhash;
  transaction.feePayer = signer.publicKey;
  transaction.lastValidBlockHeight = lastValidBlockHeight;

  // Sign
  transaction.sign(signer);

  // Serialize
  const serializedTx = bs58.encode(
    transaction.serialize({ requireAllSignatures: true })
  );

  // Send as bundle via Jito
  const bundleId = await sendBundle(endpoint, [serializedTx]);

  if (!bundleId) {
    // Fallback: send via Jito sendTransaction endpoint (still private mempool)
    return await sendViaJitoTransaction(endpoint, serializedTx);
  }

  // Poll for bundle status
  const signature = await pollBundleStatus(endpoint, bundleId, connection);
  return signature;
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
        if (status.confirmation_status === "confirmed" || status.confirmation_status === "finalized") {
          // Return the first transaction signature
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
 * Fallback: send directly via RPC (no MEV protection)
 * Used when Jito is unavailable
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
