/**
 * Wallet Setup Utility
 * Generates a new Solana keypair for mining or imports an existing one.
 */

import { Keypair } from "@solana/web3.js";
import bs58 from "bs58";
import { writeFileSync, existsSync } from "fs";

console.log(`
╔══════════════════════════════════════════════════════╗
║          EQUIUM MINER — WALLET SETUP                ║
╚══════════════════════════════════════════════════════╝
`);

// Generate new keypair
const keypair = Keypair.generate();
const publicKey = keypair.publicKey.toBase58();
const privateKey = bs58.encode(keypair.secretKey);

console.log("✅ New wallet generated!\n");
console.log(`   Public Key:  ${publicKey}`);
console.log(`   Private Key: ${privateKey.substring(0, 10)}...${privateKey.substring(privateKey.length - 10)}`);
console.log("");
console.log("⚠️  IMPORTANT: Save your private key securely!");
console.log("   Anyone with this key can access your funds.\n");

// Write to .env if it doesn't exist
if (!existsSync(".env")) {
  const envContent = `# Equium Miner Configuration
# Generated: ${new Date().toISOString()}

# RPC endpoint (get free Helius key at https://dev.helius.xyz/)
RPC_URL=https://api.mainnet-beta.solana.com

# Wallet private key (base58)
PRIVATE_KEY=${privateKey}

# Jito Anti-MEV settings
USE_JITO=true
JITO_TIP_LAMPORTS=100000

# Mining settings
MAX_ATTEMPTS=1000
ROUND_DELAY_MS=500
`;

  writeFileSync(".env", envContent);
  console.log("📝 Created .env file with your wallet.\n");
} else {
  console.log("📝 .env already exists. Add this to your .env:");
  console.log(`   PRIVATE_KEY=${privateKey}\n`);
}

console.log("─────────────────────────────────────────────────────");
console.log("Next steps:");
console.log("  1. Send at least 0.01 SOL to your wallet for tx fees:");
console.log(`     ${publicKey}`);
console.log("  2. Get a free Helius RPC key: https://dev.helius.xyz/");
console.log("  3. Update RPC_URL in .env");
console.log("  4. Run: npm run mine");
console.log("─────────────────────────────────────────────────────\n");
