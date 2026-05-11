#!/bin/bash
# ═══════════════════════════════════════════════════════
#  Equium ($EQM) Miner — VPS Setup Script
#  Target: Ubuntu 22.04+ (DigitalOcean 4 vCPU / 8GB RAM)
# ═══════════════════════════════════════════════════════

set -e

echo "╔══════════════════════════════════════════════════════╗"
echo "║   EQUIUM MINER — VPS SETUP (Ubuntu)                 ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

# ─── 1. System dependencies ───
echo "📦 Installing system dependencies..."
sudo apt-get update -qq
sudo apt-get install -y -qq build-essential gcc make nodejs npm git

# Check Node.js version (need 18+)
NODE_VERSION=$(node -v 2>/dev/null | cut -d'v' -f2 | cut -d'.' -f1)
if [ -z "$NODE_VERSION" ] || [ "$NODE_VERSION" -lt 18 ]; then
    echo "📦 Installing Node.js 20..."
    curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
    sudo apt-get install -y nodejs
fi

echo "  ✓ Node.js $(node -v)"
echo "  ✓ GCC $(gcc --version | head -1)"

# ─── 2. Build native solver ───
echo ""
echo "🔨 Building native Equihash solver..."
cd native

# Compile shared library
gcc -O3 -march=native -mtune=native -Wall -fPIC -shared \
    -o libequihash.so equihash_solver.c -lpthread
echo "  ✓ libequihash.so built"

# Compile worker binary
gcc -O3 -march=native -mtune=native -Wall \
    -o eqm_worker eqm_worker.c -lpthread
echo "  ✓ eqm_worker built"

# Quick test
echo ""
echo "🧪 Testing native solver..."
gcc -O3 -march=native -DSTANDALONE_TEST -o equihash_test equihash_solver.c -lpthread
./equihash_test
rm -f equihash_test

cd ..

# ─── 3. Install Node.js dependencies ───
echo ""
echo "📦 Installing Node.js dependencies..."
npm install --production 2>/dev/null
echo "  ✓ Dependencies installed"

# ─── 4. Check .env ───
echo ""
if [ ! -f .env ]; then
    cp .env.example .env
    echo "⚠️  Created .env from template. Please edit it:"
    echo "    nano .env"
    echo ""
    echo "    Required:"
    echo "    - PRIVATE_KEY (your Solana wallet base58 key)"
    echo "    - RPC_URL (Helius recommended: https://dev.helius.xyz/)"
else
    echo "✓ .env exists"
fi

# ─── 5. Create systemd service ───
echo ""
echo "📋 Creating systemd service..."
WORK_DIR=$(pwd)
cat > /tmp/eqm-miner.service << EOF
[Unit]
Description=Equium EQM CPU Miner
After=network.target

[Service]
Type=simple
User=$(whoami)
WorkingDirectory=${WORK_DIR}
ExecStart=$(which npx) tsx src/miner.ts
Restart=always
RestartSec=10
Environment=NODE_ENV=production

[Install]
WantedBy=multi-user.target
EOF

sudo mv /tmp/eqm-miner.service /etc/systemd/system/eqm-miner.service
sudo systemctl daemon-reload
echo "  ✓ Service created: eqm-miner"

# ─── Done ───
echo ""
echo "═══════════════════════════════════════════════════════"
echo "✅ Setup complete!"
echo ""
echo "Commands:"
echo "  npm run mine              # Run miner (foreground)"
echo "  npm run balance           # Check balance & network"
echo ""
echo "  sudo systemctl start eqm-miner    # Start as service"
echo "  sudo systemctl enable eqm-miner   # Auto-start on boot"
echo "  sudo systemctl status eqm-miner   # Check status"
echo "  journalctl -u eqm-miner -f        # View logs"
echo ""
echo "  sudo systemctl stop eqm-miner     # Stop"
echo "═══════════════════════════════════════════════════════"
