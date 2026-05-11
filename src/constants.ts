import { PublicKey } from "@solana/web3.js";

// === Equium Program ===
export const EQUIUM_PROGRAM_ID = new PublicKey(
  "ZKGMUfxiRCXFPnqz9zgqAnuqJy15jk7fKbR4o6FuEQM"
);

export const EQM_MINT = new PublicKey(
  "1MhvZzEe8gQ8Rb9CrT3Dn26Gkn9QRErzLMGkkTwveqm"
);

// PDA Seeds
export const CONFIG_SEED = Buffer.from("equium-config");
export const VAULT_SEED = Buffer.from("equium-vault");

// Equihash parameters
export const EQUIHASH_N = 96;
export const EQUIHASH_K = 5;

// Personalization for I-block
export const PERSONALIZATION = Buffer.from("Equium-v1");

// I-block length: 9 + 32 + 32 + 8 = 81
export const I_LEN = 81;

// === Jito Tip Accounts ===
// These are the 8 static Jito tip accounts (from getTipAccounts RPC)
export const JITO_TIP_ACCOUNTS = [
  new PublicKey("3AVi9Tg9Uo68tJfuvoKvqKNWKkC5wPdSSdeBnizKZ6jT"),
  new PublicKey("HFqU5x63VTqvQss8hp11i4wVV8bD44PvwucfZ2bU7gRe"),
  new PublicKey("Cw8CFyM9FkoMi7K7Crf6HNQqf4uEMzpKw6QNghXLvLkY"),
  new PublicKey("DfXygSm4jCyNCybVYYK6DwvWqjKee8pbDmJGcLWNDXjh"),
  new PublicKey("96gYZGLnJYVFmbjzopPSU6QiEV5fGqZNyN9nmNhvrZU5"),
  new PublicKey("DttWaMuVvTiduZRnguLF7jNxTgiMBZ1hyAumKUiL2KRL"),
  new PublicKey("ADuUkR4vqLUMWXxW9gh6D6L8pMSawimctcNZ5pGwDcEt"),
  new PublicKey("ADaUMid9yfUytqMBgopwjb2DTLSokTSzL1zt6iGPaS49"),
];

// Jito Block Engine endpoints
export const JITO_ENDPOINTS = {
  mainnet: "https://mainnet.block-engine.jito.wtf/api/v1",
  amsterdam: "https://amsterdam.mainnet.block-engine.jito.wtf/api/v1",
  frankfurt: "https://frankfurt.mainnet.block-engine.jito.wtf/api/v1",
  ny: "https://ny.mainnet.block-engine.jito.wtf/api/v1",
  tokyo: "https://tokyo.mainnet.block-engine.jito.wtf/api/v1",
};

// SlotHashes sysvar
export const SLOT_HASHES_SYSVAR = new PublicKey(
  "SysvarS1otHashes111111111111111111111111111"
);
