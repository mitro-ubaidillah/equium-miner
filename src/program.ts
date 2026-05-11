/**
 * Equium Program Interface
 * Builds the `mine` instruction matching the on-chain Anchor program.
 */

import {
  Connection,
  PublicKey,
  TransactionInstruction,
  SystemProgram,
  SYSVAR_RENT_PUBKEY,
} from "@solana/web3.js";
import { TOKEN_PROGRAM_ID, ASSOCIATED_TOKEN_PROGRAM_ID, getAssociatedTokenAddress } from "@solana/spl-token";
import { createHash } from "crypto";
import {
  EQUIUM_PROGRAM_ID,
  EQM_MINT,
  CONFIG_SEED,
  VAULT_SEED,
  SLOT_HASHES_SYSVAR,
} from "./constants.js";

// Anchor discriminator for `mine` instruction
// sha256("global:mine")[0..8]
const MINE_DISCRIMINATOR = Buffer.from(
  createHash("sha256").update("global:mine").digest().subarray(0, 8)
);

/**
 * Derive the config PDA
 */
export function getConfigPDA(): [PublicKey, number] {
  return PublicKey.findProgramAddressSync(
    [CONFIG_SEED],
    EQUIUM_PROGRAM_ID
  );
}

/**
 * Derive the vault PDA
 */
export function getVaultPDA(): [PublicKey, number] {
  return PublicKey.findProgramAddressSync(
    [VAULT_SEED],
    EQUIUM_PROGRAM_ID
  );
}

/**
 * Fetch the current on-chain config state
 */
export interface EquiumConfigState {
  mint: PublicKey;
  mineableVault: PublicKey;
  mineableVaultBump: number;
  configBump: number;
  genesisSlot: bigint;
  genesisUnixTs: bigint;
  equihashN: number;
  equihashK: number;
  currentTarget: Buffer;
  blockHeight: bigint;
  currentChallenge: Buffer;
  currentRoundOpenSlot: bigint;
  currentRoundOpenUnixTs: bigint;
  lastWinner: PublicKey;
  currentEpochReward: bigint;
  nextHalvingBlock: bigint;
  nextRetargetBlock: bigint;
  lastRetargetUnixTs: bigint;
  cumulativeMined: bigint;
  emptyRounds: bigint;
  miningOpen: boolean;
  admin: PublicKey;
  adminRenounced: boolean;
}

/**
 * Parse the EquiumConfig account data (Anchor layout)
 */
export function parseConfigAccount(data: Buffer): EquiumConfigState {
  // Skip 8-byte Anchor discriminator
  let offset = 8;

  const mint = new PublicKey(data.subarray(offset, offset + 32));
  offset += 32;

  const mineableVault = new PublicKey(data.subarray(offset, offset + 32));
  offset += 32;

  const mineableVaultBump = data.readUInt8(offset);
  offset += 1;

  const configBump = data.readUInt8(offset);
  offset += 1;

  const genesisSlot = data.readBigUInt64LE(offset);
  offset += 8;

  const genesisUnixTs = data.readBigInt64LE(offset);
  offset += 8;

  const equihashN = data.readUInt32LE(offset);
  offset += 4;

  const equihashK = data.readUInt32LE(offset);
  offset += 4;

  const currentTarget = Buffer.from(data.subarray(offset, offset + 32));
  offset += 32;

  const blockHeight = data.readBigUInt64LE(offset);
  offset += 8;

  const currentChallenge = Buffer.from(data.subarray(offset, offset + 32));
  offset += 32;

  const currentRoundOpenSlot = data.readBigUInt64LE(offset);
  offset += 8;

  const currentRoundOpenUnixTs = data.readBigInt64LE(offset);
  offset += 8;

  const lastWinner = new PublicKey(data.subarray(offset, offset + 32));
  offset += 32;

  const currentEpochReward = data.readBigUInt64LE(offset);
  offset += 8;

  const nextHalvingBlock = data.readBigUInt64LE(offset);
  offset += 8;

  const nextRetargetBlock = data.readBigUInt64LE(offset);
  offset += 8;

  const lastRetargetUnixTs = data.readBigInt64LE(offset);
  offset += 8;

  const cumulativeMined = data.readBigUInt64LE(offset);
  offset += 8;

  const emptyRounds = data.readBigUInt64LE(offset);
  offset += 8;

  const miningOpen = data.readUInt8(offset) === 1;
  offset += 1;

  const admin = new PublicKey(data.subarray(offset, offset + 32));
  offset += 32;

  const adminRenounced = data.readUInt8(offset) === 1;
  offset += 1;

  return {
    mint,
    mineableVault,
    mineableVaultBump,
    configBump,
    genesisSlot,
    genesisUnixTs,
    equihashN,
    equihashK,
    currentTarget,
    blockHeight,
    currentChallenge,
    currentRoundOpenSlot,
    currentRoundOpenUnixTs,
    lastWinner,
    currentEpochReward,
    nextHalvingBlock,
    nextRetargetBlock,
    lastRetargetUnixTs,
    cumulativeMined,
    emptyRounds,
    miningOpen,
    admin,
    adminRenounced,
  };
}

/**
 * Build the `mine` transaction instruction
 */
export async function buildMineInstruction(
  minerPubkey: PublicKey,
  nonce: Buffer,
  solutionIndices: Buffer
): Promise<TransactionInstruction> {
  const [configPDA] = getConfigPDA();
  const [vaultPDA] = getVaultPDA();

  // Get miner's ATA for EQM
  const minerAta = await getAssociatedTokenAddress(
    EQM_MINT,
    minerPubkey,
    false,
    TOKEN_PROGRAM_ID
  );

  // Serialize instruction data:
  // discriminator (8) + nonce (32) + soln_indices (4 byte len + bytes)
  const solnLen = Buffer.alloc(4);
  solnLen.writeUInt32LE(solutionIndices.length);

  const instructionData = Buffer.concat([
    MINE_DISCRIMINATOR,
    nonce,
    solnLen,
    solutionIndices,
  ]);

  // Account metas matching the Mine struct in lib.rs
  const keys = [
    { pubkey: minerPubkey, isSigner: true, isWritable: true },       // miner
    { pubkey: configPDA, isSigner: false, isWritable: true },        // config
    { pubkey: EQM_MINT, isSigner: false, isWritable: false },        // mint
    { pubkey: vaultPDA, isSigner: false, isWritable: true },         // mineable_vault
    { pubkey: minerAta, isSigner: false, isWritable: true },         // miner_ata
    { pubkey: TOKEN_PROGRAM_ID, isSigner: false, isWritable: false },// token_program
    { pubkey: ASSOCIATED_TOKEN_PROGRAM_ID, isSigner: false, isWritable: false }, // associated_token_program
    { pubkey: SystemProgram.programId, isSigner: false, isWritable: false },     // system_program
    { pubkey: SLOT_HASHES_SYSVAR, isSigner: false, isWritable: false },          // slot_hashes
  ];

  return new TransactionInstruction({
    programId: EQUIUM_PROGRAM_ID,
    keys,
    data: instructionData,
  });
}

/**
 * Fetch current config from chain
 */
export async function fetchConfig(
  connection: Connection
): Promise<EquiumConfigState> {
  const [configPDA] = getConfigPDA();
  const accountInfo = await connection.getAccountInfo(configPDA);

  if (!accountInfo) {
    throw new Error("Equium config account not found on-chain.");
  }

  // Check if account is owned by the program (initialized)
  if (accountInfo.owner.equals(new PublicKey("11111111111111111111111111111111"))) {
    throw new Error(
      "Equium config PDA exists but is not yet initialized (owned by System Program). " +
      "Mining has not started yet. The program needs to call `initialize` first."
    );
  }

  if (accountInfo.data.length < 8 + 32) {
    throw new Error(
      `Equium config account data too small (${accountInfo.data.length} bytes). ` +
      "Program may not be fully initialized."
    );
  }

  return parseConfigAccount(accountInfo.data as Buffer);
}
