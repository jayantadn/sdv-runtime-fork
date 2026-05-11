#!/bin/bash
# setup_local.sh — Prepare local (non-Docker) environment for SDV-Runtime
# Run this once before starting services with start_services.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOCAL_PACKAGES_DIR="$SCRIPT_DIR/.local-packages"

echo "=== SDV-Runtime Local Setup ==="
echo "Project root: $SCRIPT_DIR"

# ─── 1. System dependencies ──────────────────────────────────────────────────
echo ""
echo "[1/5] Checking system dependencies..."

MISSING_PKGS=""
for pkg in mosquitto python3 python3-pip git ca-certificates; do
    if ! command -v "$pkg" >/dev/null 2>&1 && ! dpkg -s "$pkg" >/dev/null 2>&1; then
        MISSING_PKGS="$MISSING_PKGS $pkg"
    fi
done

if [ -n "$MISSING_PKGS" ]; then
    echo "  Installing missing packages:$MISSING_PKGS"
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends $MISSING_PKGS
else
    echo "  All system dependencies present."
fi

# python3-dev and build-essential are needed for some pip packages
if ! dpkg -s python3-dev >/dev/null 2>&1 || ! dpkg -s build-essential >/dev/null 2>&1; then
    echo "  Installing build tools (python3-dev, build-essential)..."
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends python3-dev build-essential
fi

# ─── 2. Mosquitto config ─────────────────────────────────────────────────────
echo ""
echo "[2/5] Setting up Mosquitto config..."

MOSQUITTO_CONF_SRC="$SCRIPT_DIR/mosquitto-no-auth.conf"
MOSQUITTO_CONF_DST="/etc/mosquitto/mosquitto-no-auth.conf"

if [ ! -f "$MOSQUITTO_CONF_DST" ]; then
    echo "  Copying mosquitto-no-auth.conf to /etc/mosquitto/ (requires sudo)..."
    sudo cp "$MOSQUITTO_CONF_SRC" "$MOSQUITTO_CONF_DST"
    echo "  Done."
else
    echo "  Mosquitto config already in place: $MOSQUITTO_CONF_DST"
fi

# ─── 3. C++ binaries — rebuild if pre-built binary has symbol/version mismatch ──
echo ""
echo "[3/5] Checking C++ binaries..."

HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64)  ARCH="amd64"; NODE_SUFFIX="x64" ;;
    aarch64) ARCH="arm64"; NODE_SUFFIX="arm64" ;;
    *)       ARCH="amd64"; NODE_SUFFIX="x64" ;;
esac

DATABROKER="$SCRIPT_DIR/bin/$ARCH/databroker-$ARCH"
NODE_KM="$SCRIPT_DIR/bin/$ARCH/node-km-$NODE_SUFFIX"
SYNCER="$SCRIPT_DIR/kuksa-syncer-cpp/build/kuksa-syncer"
MOCK_PROVIDER="$SCRIPT_DIR/kuksa-syncer-cpp/build/mock-provider"

# Test whether the pre-built kuksa-syncer binary is compatible with this system's
# gRPC/protobuf. Uses ldd -r which performs full relocation checks and prints
# "undefined symbol" for ABI mismatches without executing the binary.
_needs_rebuild() {
    local bin="$1"
    [ ! -f "$bin" ] && return 0  # missing — must build
    local ldd_out
    ldd_out=$(ldd -r "$bin" 2>&1)
    # "not found"      — a required .so is absent
    # "undefined symbol" — .so exists but symbol is missing (protobuf/gRPC ABI mismatch)
    if echo "$ldd_out" | grep -qE "not found|undefined symbol"; then
        return 0
    fi
    return 1
}

if _needs_rebuild "$SYNCER"; then
    echo "  Pre-built kuksa-syncer is incompatible (gRPC/protobuf version mismatch)."
    echo "  Installing C++ build dependencies and rebuilding from source..."

    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends \
        cmake \
        build-essential \
        libgrpc++-dev \
        libprotobuf-dev \
        protobuf-compiler \
        protobuf-compiler-grpc \
        libabsl-dev \
        libssl-dev \
        pkg-config

    BUILD_DIR="$SCRIPT_DIR/kuksa-syncer-cpp/build"
    mkdir -p "$BUILD_DIR"
    cmake -S "$SCRIPT_DIR/kuksa-syncer-cpp" \
          -B "$BUILD_DIR" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          2>&1 | tail -5
    cmake --build "$BUILD_DIR" --parallel "$(nproc)" 2>&1 | tail -10
    echo "  Rebuild complete."
else
    echo "  Pre-built kuksa-syncer is compatible with this system."
fi

for bin in "$DATABROKER" "$NODE_KM" "$SYNCER" "$MOCK_PROVIDER"; do
    if [ -f "$bin" ]; then
        chmod +x "$bin"
        echo "  [ok] $bin"
    else
        echo "  [WARN] Not found: $bin"
    fi
done

# ─── 4. Python packages ───────────────────────────────────────────────────────
echo ""
echo "[4/5] Installing Python packages to $LOCAL_PACKAGES_DIR ..."

mkdir -p "$LOCAL_PACKAGES_DIR"

# Determine if pip needs --break-system-packages (PEP 668 / Ubuntu 23.04+)
PIP_BREAK_FLAG=""
if python3 -m pip install --help 2>&1 | grep -q -- '--break-system-packages'; then
    PIP_BREAK_FLAG="--break-system-packages"
fi

# Upgrade pip quietly
python3 -m pip install --quiet --upgrade pip $PIP_BREAK_FLAG

# Install all requirements into the local packages directory
python3 -m pip install --quiet \
    --target "$LOCAL_PACKAGES_DIR" \
    --no-cache-dir \
    $PIP_BREAK_FLAG \
    -r "$SCRIPT_DIR/requirements.txt"

echo "  Python packages installed."

# ─── 5. Verify VSS data ──────────────────────────────────────────────────────
echo ""
echo "[5/5] Checking VSS data..."

VSS_JSON="$SCRIPT_DIR/data/vss-core/vss.json"
if [ -f "$VSS_JSON" ]; then
    echo "  VSS data found: $VSS_JSON"
else
    echo "  [WARN] VSS data not found at $VSS_JSON"
    echo "         Run: git submodule update --init --recursive"
    echo "         Then re-run this script for full VSS generation."
fi

# ─── Summary ──────────────────────────────────────────────────────────────────
echo ""
echo "=== Setup Complete ==="
echo ""
echo "To start all services locally, run:"
echo ""
echo "  export PYTHONPATH=\"$LOCAL_PACKAGES_DIR:\$PYTHONPATH\""
echo "  export SITE_PACKAGES_DIR=\"$LOCAL_PACKAGES_DIR\""
echo "  export RUNTIME_NAME=\"MySDVRuntime\""
echo "  bash $SCRIPT_DIR/start_services.sh"
echo ""
echo "Or use the convenience wrapper:"
echo "  bash $SCRIPT_DIR/run_local.sh"
echo ""
