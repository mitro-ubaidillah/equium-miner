# Equium ($EQM) CPU Miner — Anti-MEV Edition

Bot mining untuk Equium ($EQM) di Solana mainnet dengan proteksi anti-MEV via Jito Bundle.

## Fitur

- ⛏ CPU mining Equihash (96, 5) — pure TypeScript
- 🛡 Anti-MEV via Jito Bundle (transaksi tidak terlihat di public mempool)
- 💰 Auto-claim reward ke wallet
- 📊 Real-time stats (hashrate, blocks mined, uptime)
- 🔄 Auto-retry pada stale challenge
- ⚡ Priority fee untuk landing cepat

## Cara Kerja Anti-MEV

Tanpa proteksi, validator bisa:
1. Melihat solusi kamu di mempool
2. Meng-copy dan submit solusi mereka sendiri (front-run)

Dengan Jito Bundle:
- Transaksi dikirim ke **private mempool** Jito
- Validator tidak bisa melihat/copy solusi sebelum dieksekusi
- Bundle diproses secara atomik (all-or-nothing)
- Tip ke Jito validator menjamin prioritas inklusi

## Setup

### 1. Install dependencies

```bash
npm install
```

### 2. Generate wallet baru

```bash
npm run setup
```

Atau import wallet existing — edit `.env` dan masukkan private key.

### 3. Konfigurasi

Copy `.env.example` ke `.env` dan isi:

```bash
cp .env.example .env
```

**Wajib:**
- `PRIVATE_KEY` — Base58 private key wallet kamu
- `RPC_URL` — Helius RPC recommended (gratis: https://dev.helius.xyz/)

**Optional:**
- `JITO_TIP_LAMPORTS` — Tip ke Jito (default: 100000 = 0.0001 SOL)
- `USE_JITO` — Set `false` untuk disable anti-MEV
- `MAX_ATTEMPTS` — Max nonce per cycle (default: 1000)

### 4. Fund wallet

Kirim minimal **0.01 SOL** ke wallet untuk biaya transaksi:
```
<your-wallet-pubkey>
```

### 5. Mulai mining

```bash
npm run mine
```

### 6. Cek status

```bash
npm run balance
```

## Biaya Operasional

Per blok yang berhasil ditambang:
- Transaction fee: ~0.000005 SOL
- Jito tip: 0.0001 SOL (configurable)
- **Total: ~0.0001 SOL per mine**

Reward: **25 EQM per blok** (saat ini)

## Catatan Keamanan

- Private key disimpan di `.env` (jangan commit ke git!)
- `.gitignore` sudah mengecualikan `.env`
- Bot tidak mengirim private key ke mana pun
- Jito bundle memastikan solusi tidak bisa di-front-run

## Troubleshooting

**"Insufficient SOL"** — Kirim lebih banyak SOL ke wallet.

**"Stale challenge"** — Round sudah dimenangkan orang lain. Normal, bot akan retry.

**"Jito bundle error"** — Jito sedang overload. Bot akan fallback ke direct submission.

**Rate limited** — Gunakan Helius RPC (gratis 100k req/hari) daripada public RPC.
