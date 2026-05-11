/**
 * Equium ($EQM) Mining Bot
 * Features:
 * - CPU mining with Equihash (96, 5)
 * - Anti-MEV via Jito bundle submission
 * - Auto-retry on stale challenges
 * - Real-time stats display
 */

import { Connection, Keypair, Transaction } from "@solana/web3.js";
import { getAssociatedTokenAddress } from "@solana/spl-token";
import bs58 from "bs58";
import { config } from "dotenv";
import { randomBytes, createHash } from "crypto";

import { buildInputBlock, hashUnderTarget, solutionHash } from "./equihash.js";
import { fetchConfig, buildMineInstruction, EquiumConfigState } from "./program.js";
import { sendWithJitoBundle, sendDirectTransaction } from "./jito.js";
import { EQM_MINT } from "./constants.js";
import { initSolver, solveEquihashMulti } from "./solver-native.js";

config(); // Load .env

// === Configuration ===
const RPC_URL = process.env.RPC_URL || "https://api.mainnet-beta.solana.com";
const PRIVATE_KEY = process.env.PRIVATE_KEY;
const JITO_TIP_LAMPORTS = parseInt(process.env.JITO_TIP_LAMPORTS || "100000");
const MAX_ATTEMPTS = parseInt(process.env.MAX_ATTEMPTS || "200");
const ROUND_DELAY_MS = parseInt(process.env.ROUND_DELAY_MS || "500");
const USE_JITO = process.env.USE_JITO !== "false"; // default true
const NUM_THREADS = parseInt(process.env.THREADS || "4");

// === Stats ===
let totalMined = 0;
let totalBlocks = 0;
let totalAttempts = 0;
let startTime = Date.now();

function formatDuration(ms: number): string {
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return `${h}:${String(m).padStart(2, "0")}:${String(sec).padStart(2, "0")}`;
}

function printBanner() {
  console.log(`
╔══════════════════════════════════════════════════════╗
║          EQUIUM ($EQM) CPU MINER v1.0               ║
║          Anti-MEV via Jito Bundle                    ║
╚══════════════════════════════════════════════════════╝
`);
}

function printStats() {
  const elapsed = Date.now() - startTime;
  const hashRate = totalAttempts / (elapsed / 1000);
  console.log(
    `\n📊 Stats: ${totalBlocks} blocks | ${totalMined / 1_000_000} EQM | ` +
      `${hashRate.toFixed(1)} H/s | uptime ${formatDuration(elapsed)}`
  );
}

/**
 * Solve Equihash using native C multi-threaded solver
 * Falls back to JS if native not available.
 */
async function solveBlock(
  currentChallenge: Buffer,
  minerPubkey: Buffer,
  blockHeight: bigint,
  target: Buffer,
  n: number,
  k: number
): Promise<{ nonce: Buffer; solutionIndices: Buffer } | null> {
  const input = buildInputBlock(currentChallenge, minerPubkey, blockHeight);

  const result = await solveEquihashMulti(input, target, NUM_THREADS, MAX_ATTEMPTS);

  if (result) {
    totalAttempts += result.attempts;
    return { nonce: result.nonce, solutionIndices: result.solution };
  }

  totalAttempts += MAX_ATTEMPTS;
  return null;
}

/**
 * Main mining loop
 */
async function mine() {
  printBanner();

  // Validate config
  if (!PRIVATE_KEY) {
    console.error("❌ PRIVATE_KEY not set in .env");
    console.error("   Run: npm run setup  to generate a wallet");
    process.exit(1);
  }

  // Setup connection and wallet
  const connection = new Connection(RPC_URL, {
    commitment: "confirmed",
    confirmTransactionInitialTimeout: 60_000,
  });

  let keypair: Keypair;
  try {
    // Try base58 first
    const decoded = bs58.decode(PRIVATE_KEY);
    keypair = Keypair.fromSecretKey(decoded);
  } catch {
    try {
      // Try JSON array format
      const arr = JSON.parse(PRIVATE_KEY);
      keypair = Keypair.fromSecretKey(Uint8Array.from(arr));
    } catch {
      console.error("❌ Invalid PRIVATE_KEY format. Use base58 or JSON array.");
      process.exit(1);
    }
  }

  const minerPubkey = keypair.publicKey;
  console.log(`⛏  Miner:    ${minerPubkey.toBase58()}`);
  console.log(`🔗 RPC:      ${RPC_URL.replace(/api-key=.*/, "api-key=***")}`);
  console.log(`🛡  Anti-MEV: ${USE_JITO ? "Jito Bundle ✓" : "Disabled"}`);
  console.log(`💰 Jito Tip: ${JITO_TIP_LAMPORTS / 1e9} SOL`);
  console.log("");

  // Initialize solver (native C or JS fallback)
  const solverInfo = initSolver();
  console.log(`🔧 Solver:   ${solverInfo.native ? "Native C (multi-thread)" : "Pure JS (fallback)"}`);
  console.log(`🧵 Threads:  ${NUM_THREADS}`);
  console.log("");

  // Check SOL balance
  const balance = await connection.getBalance(minerPubkey);
  console.log(`💳 SOL Balance: ${(balance / 1e9).toFixed(6)} SOL`);
  if (balance < 10_000_000) {
    // 0.01 SOL minimum
    console.error("❌ Insufficient SOL for transaction fees. Need at least 0.01 SOL.");
    process.exit(1);
  }

  // Check EQM balance
  try {
    const ata = await getAssociatedTokenAddress(EQM_MINT, minerPubkey);
    const tokenBalance = await connection.getTokenAccountBalance(ata);
    console.log(`🪙 EQM Balance: ${tokenBalance.value.uiAmountString} EQM`);
  } catch {
    console.log(`🪙 EQM Balance: 0 (ATA will be created on first mine)`);
  }

  console.log("\n─────────────────────────────────────────────────────");
  console.log("🚀 Starting mining loop...\n");

  startTime = Date.now();

  // Main loop
  while (true) {
    try {
      // Fetch current state
      const state = await fetchConfig(connection);

      if (!state.miningOpen) {
        console.log("⏳ Mining not yet open. Waiting...");
        await sleep(10_000);
        continue;
      }

      const reward = Number(state.currentEpochReward) / 1_000_000;
      console.log(
        `\n⛏  Round #${state.blockHeight}   reward ${reward} EQM   ` +
          `target 0x${state.currentTarget.subarray(0, 4).toString("hex")}…`
      );

      // Solve
      const solution = await solveBlock(
        state.currentChallenge,
        minerPubkey.toBuffer(),
        state.blockHeight,
        state.currentTarget,
        state.equihashN,
        state.equihashK
      );

      if (!solution) {
        console.log("  ✗ No solution found this cycle, refreshing challenge...");
        await sleep(ROUND_DELAY_MS);
        continue;
      }

      // Verify solution locally before submitting
      const input = buildInputBlock(
        state.currentChallenge,
        minerPubkey.toBuffer(),
        state.blockHeight
      );
      const solnHash = solutionHash(solution.solutionIndices, input);

      if (!hashUnderTarget(solnHash, state.currentTarget)) {
        console.log("  ✗ Solution above target, retrying...");
        continue;
      }

      console.log(`  ✓ Solution found! Hash: 0x${solnHash.subarray(0, 6).toString("hex")}…`);

      // Re-fetch state to check if round is still open (avoid stale submission)
      const freshState = await fetchConfig(connection);
      if (freshState.blockHeight !== state.blockHeight) {
        console.log("  ⚠ Round already advanced, someone else won. Moving on...");
        continue;
      }

      // Build mine instruction
      const mineIx = await buildMineInstruction(
        minerPubkey,
        solution.nonce,
        solution.solutionIndices
      );

      const tx = new Transaction().add(mineIx);

      // Submit via Jito (anti-MEV) or direct
      let signature: string;
      try {
        if (USE_JITO) {
          console.log("  📡 Submitting via Jito bundle (anti-MEV)...");
          signature = await sendWithJitoBundle(
            connection,
            tx,
            keypair,
            JITO_TIP_LAMPORTS
          );
        } else {
          console.log("  📡 Submitting directly...");
          signature = await sendDirectTransaction(connection, tx, keypair);
        }

        totalBlocks++;
        totalMined += Number(state.currentEpochReward);

        console.log(`  ✓ MINED! +${reward} EQM`);
        console.log(`    sig: ${signature}`);
        printStats();
      } catch (err: any) {
        if (err.message?.includes("StaleChallenge") || err.message?.includes("already been processed")) {
          console.log("  ⚠ Stale challenge — round already won by someone else.");
        } else if (err.message?.includes("AboveTarget")) {
          console.log("  ⚠ Solution rejected (above target). Difficulty may have changed.");
        } else {
          console.error(`  ❌ Transaction failed: ${err.message}`);
        }
      }

      await sleep(ROUND_DELAY_MS);
    } catch (err: any) {
      console.error(`\n❌ Error: ${err.message}`);
      console.log("   Retrying in 5s...");
      await sleep(5000);
    }
  }
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// Run
mine().catch((err) => {
  console.error("Fatal error:", err);
  process.exit(1);
});
