/**
 * Equihash utility functions shared between miner and solver.
 * Core solver logic is in solver-native.ts
 */

import { createHash } from "crypto";

/**
 * Build the Equihash I-block (input block) for Equium
 * I = "Equium-v1" || current_challenge || miner_pubkey || block_height_le
 *
 * Total length: 9 + 32 + 32 + 8 = 81 bytes
 */
export function buildInputBlock(
  currentChallenge: Buffer,
  minerPubkey: Buffer,
  blockHeight: bigint
): Buffer {
  const buf = Buffer.alloc(81);
  buf.write("Equium-v1", 0, 9, "ascii");
  currentChallenge.copy(buf, 9);
  minerPubkey.copy(buf, 41);
  buf.writeBigUInt64LE(blockHeight, 73);
  return buf;
}

/**
 * Compute solution hash: SHA256(soln_indices || input)
 * This is compared against the difficulty target.
 */
export function solutionHash(solutionIndices: Buffer, input: Buffer): Buffer {
  const h = createHash("sha256");
  h.update(solutionIndices);
  h.update(input);
  return h.digest();
}

/**
 * Check if hash is under target (big-endian 256-bit comparison)
 * Returns true if hash < target (solution is valid)
 */
export function hashUnderTarget(hash: Buffer, target: Buffer): boolean {
  for (let i = 0; i < 32; i++) {
    if (hash[i] < target[i]) return true;
    if (hash[i] > target[i]) return false;
  }
  return false; // equal = not under
}
