/**
 * Check wallet balance and mining stats
 */

import { Connection, Keypair, PublicKey } from "@solana/web3.js";
import { getAssociatedTokenAddress } from "@solana/spl-token";
import bs58 from "bs58";
import { config } from "dotenv";
import { fetchConfig } from "./program.js";
import { EQM_MINT } from "./constants.js";

config();

async function main() {
  const RPC_URL = process.env.RPC_URL || "https://api.mainnet-beta.solana.com";
  const PRIVATE_KEY = process.env.PRIVATE_KEY;

  if (!PRIVATE_KEY) {
    console.error("❌ PRIVATE_KEY not set in .env");
    process.exit(1);
  }

  const connection = new Connection(RPC_URL, "confirmed");

  let keypair: Keypair;
  try {
    keypair = Keypair.fromSecretKey(bs58.decode(PRIVATE_KEY));
  } catch {
    try {
      keypair = Keypair.fromSecretKey(Uint8Array.from(JSON.parse(PRIVATE_KEY)));
    } catch {
      console.error("❌ Invalid PRIVATE_KEY");
      process.exit(1);
    }
  }

  const pubkey = keypair.publicKey;

  console.log(`
╔══════════════════════════════════════════════════════╗
║          EQUIUM MINER — STATUS                      ║
╚══════════════════════════════════════════════════════╝
`);

  // Wallet info
  console.log(`👛 Wallet: ${pubkey.toBase58()}`);

  const solBalance = await connection.getBalance(pubkey);
  console.log(`💰 SOL:    ${(solBalance / 1e9).toFixed(6)} SOL`);

  try {
    const ata = await getAssociatedTokenAddress(EQM_MINT, pubkey);
    const tokenBalance = await connection.getTokenAccountBalance(ata);
    console.log(`🪙 EQM:    ${tokenBalance.value.uiAmountString} EQM`);
  } catch {
    console.log(`🪙 EQM:    0 (no token account yet)`);
  }

  // Network info
  console.log("\n─── Network Status ───");
  try {
    const state = await fetchConfig(connection);
    console.log(`📦 Block Height:    ${state.blockHeight}`);
    console.log(`🎯 Current Target:  0x${state.currentTarget.subarray(0, 4).toString("hex")}…`);
    console.log(`💎 Epoch Reward:    ${Number(state.currentEpochReward) / 1_000_000} EQM`);
    console.log(`⛏  Total Mined:    ${Number(state.cumulativeMined) / 1_000_000} EQM`);
    console.log(`📭 Empty Rounds:   ${state.emptyRounds}`);
    console.log(`🔓 Mining Open:    ${state.miningOpen ? "Yes ✓" : "No ✗"}`);
    console.log(`👑 Last Winner:    ${state.lastWinner.toBase58()}`);
    console.log(`🔑 Admin Renounced: ${state.adminRenounced ? "Yes ✓" : "No ⚠️"}`);
    console.log(`📐 Equihash:       (${state.equihashN}, ${state.equihashK})`);

    const remainingEQM = (18_900_000 - Number(state.cumulativeMined) / 1_000_000);
    console.log(`\n📊 Remaining Mineable: ${remainingEQM.toLocaleString()} EQM`);

    // Next halving
    const blocksToHalving = Number(state.nextHalvingBlock) - Number(state.blockHeight);
    const minutesToHalving = blocksToHalving; // ~1 block per minute
    const daysToHalving = minutesToHalving / 60 / 24;
    console.log(`⏰ Next Halving:   Block #${state.nextHalvingBlock} (~${daysToHalving.toFixed(0)} days)`);
  } catch (err: any) {
    console.error(`❌ Could not fetch network state: ${err.message}`);
  }

  console.log("");
}

main().catch(console.error);
