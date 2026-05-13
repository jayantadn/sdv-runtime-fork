#!/bin/bash

# Copyright (c) 2025 Eclipse Foundation.
#
# This program and the accompanying materials are made available under the
# terms of the MIT License which is available at
# https://opensource.org/licenses/MIT.
#
# SPDX-License-Identifier: MIT

# run.sh — Start SDV-Runtime services
# Run setup.sh first if you haven't already.
#
# Usage: bash run.sh [RUNTIME_NAME]
#   RUNTIME_NAME  Display name for this runtime (default: LocalSDVRuntime)
#
# Examples:
#   bash run.sh
#   bash run.sh MyCarRuntime
#   SYNCER_SERVER_URL=http://localhost:3090 bash run.sh MyCarRuntime

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOCAL_PACKAGES_DIR="$SCRIPT_DIR/.local-packages"

# ─── Check setup was done ────────────────────────────────────────────────────
if [ ! -d "$LOCAL_PACKAGES_DIR" ]; then
    echo "ERROR: Local packages not found. Run setup.sh first:"
    echo "  bash $SCRIPT_DIR/setup.sh"
    exit 1
fi

# ─── Export environment for local paths ──────────────────────────────────────
export PYTHONPATH="$LOCAL_PACKAGES_DIR:${PYTHONPATH:-}"
export SITE_PACKAGES_DIR="$LOCAL_PACKAGES_DIR"
export GEN_MODEL_DIR="$LOCAL_PACKAGES_DIR/gen_model"

# Configurable defaults (override via environment or command-line argument)
# $1 takes priority, then $RUNTIME_NAME env var, then the built-in default.
export RUNTIME_NAME="${1:-${RUNTIME_NAME:-"LocalSDVRuntime"}}"
# Connects to the remote Kit Server by default.
# To use the bundled local kit-manager instead (no internet required), run:
#   SYNCER_SERVER_URL=http://localhost:3090 bash run.sh
export SYNCER_SERVER_URL="${SYNCER_SERVER_URL:-"https://kit.digitalauto.tech"}"
export MOCK_SIGNAL="${MOCK_SIGNAL:-"$SCRIPT_DIR/mock/signals.json"}"

echo "=== Starting SDV-Runtime (local mode) ==="
echo "  RUNTIME_NAME      : $RUNTIME_NAME"
echo "  SYNCER_SERVER_URL : $SYNCER_SERVER_URL"
echo "  MOCK_SIGNAL       : $MOCK_SIGNAL"
echo "  PYTHONPATH        : $PYTHONPATH"
echo ""

exec bash "$SCRIPT_DIR/start_services.sh"
