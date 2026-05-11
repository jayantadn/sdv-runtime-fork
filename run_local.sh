#!/bin/bash
# run_local.sh — Start SDV-Runtime services locally (without Docker)
# Run setup_local.sh first if you haven't already.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOCAL_PACKAGES_DIR="$SCRIPT_DIR/.local-packages"

# ─── Check setup was done ────────────────────────────────────────────────────
if [ ! -d "$LOCAL_PACKAGES_DIR" ]; then
    echo "ERROR: Local packages not found. Run setup_local.sh first:"
    echo "  bash $SCRIPT_DIR/setup_local.sh"
    exit 1
fi

# ─── Export environment for local paths ──────────────────────────────────────
export PYTHONPATH="$LOCAL_PACKAGES_DIR:${PYTHONPATH:-}"
export SITE_PACKAGES_DIR="$LOCAL_PACKAGES_DIR"
export GEN_MODEL_DIR="$LOCAL_PACKAGES_DIR/gen_model"

# Configurable defaults (override via environment or edit here)
export RUNTIME_NAME="${RUNTIME_NAME:-"LocalSDVRuntime"}"
# Uses the bundled local kit-manager by default (no internet required).
# The bundled node-km binary listens on port 3090.
# To connect to the remote server instead, run:
#   SYNCER_SERVER_URL=https://kit.digitalauto.tech bash run_local.sh
export SYNCER_SERVER_URL="${SYNCER_SERVER_URL:-"http://localhost:3090"}"
export MOCK_SIGNAL="${MOCK_SIGNAL:-"$SCRIPT_DIR/mock/signals.json"}"

echo "=== Starting SDV-Runtime (local mode) ==="
echo "  RUNTIME_NAME      : $RUNTIME_NAME"
echo "  SYNCER_SERVER_URL : $SYNCER_SERVER_URL"
echo "  MOCK_SIGNAL       : $MOCK_SIGNAL"
echo "  PYTHONPATH        : $PYTHONPATH"
echo ""

exec bash "$SCRIPT_DIR/start_services.sh"
