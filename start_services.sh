#!/bin/sh

# Copyright (c) 2025 Eclipse Foundation.
# 
# This program and the accompanying materials are made available under the
# terms of the MIT License which is available at
# https://opensource.org/licenses/MIT.
#
# SPDX-License-Identifier: MIT

# Resolve script directory for local (non-Docker) path fallbacks
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Detect host architecture for local binary selection
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64)  LOCAL_ARCH="amd64" ; LOCAL_NODE_SUFFIX="x64"  ;;
    aarch64) LOCAL_ARCH="arm64" ; LOCAL_NODE_SUFFIX="arm64" ;;
    *)       LOCAL_ARCH="amd64" ; LOCAL_NODE_SUFFIX="x64"  ;;
esac

# Detect if running inside a Docker container
_in_docker() {
    [ -f "/.dockerenv" ] || grep -qa 'docker\|container' /proc/1/cgroup 2>/dev/null
}

# Resolve binary paths: use Docker paths only inside a container, otherwise use local paths
_resolve() {
    docker_path="$1"
    local_path="$2"
    if _in_docker && [ -f "$docker_path" ] && [ -x "$docker_path" ]; then
        echo "$docker_path"
    elif [ -f "$local_path" ] && [ -x "$local_path" ]; then
        echo "$local_path"
    else
        echo ""
    fi
}

DATABROKER_BIN="$(_resolve /app/databroker "$SCRIPT_DIR/bin/$LOCAL_ARCH/databroker-$LOCAL_ARCH")"
NODE_KM_BIN="$(_resolve /home/dev/ws/kit-manager/node-km "$SCRIPT_DIR/bin/$LOCAL_ARCH/node-km-$LOCAL_NODE_SUFFIX")"
MOCK_PROVIDER_BIN="$(_resolve /home/dev/ws/kuksa-syncer-cpp/mock-provider "$SCRIPT_DIR/kuksa-syncer-cpp/build/mock-provider")"

# Select plain or TLS syncer binary based on URL scheme (https:// ? TLS binary)
case "${SYNCER_SERVER_URL:-}" in
    https://*)
        SYNCER_BIN="$(_resolve /home/dev/ws/kuksa-syncer-cpp/kuksa-syncer-tls "$SCRIPT_DIR/kuksa-syncer-cpp/build/kuksa-syncer-tls")"
        ;;
    *)
        SYNCER_BIN="$(_resolve /home/dev/ws/kuksa-syncer-cpp/kuksa-syncer "$SCRIPT_DIR/kuksa-syncer-cpp/build/kuksa-syncer")"
        ;;
esac

# Resolve data file paths
_resolve_file() {
    if [ -f "$1" ]; then echo "$1"; elif [ -f "$2" ]; then echo "$2"; else echo ""; fi
}
DEFAULT_MOCK_SIGNAL="$(_resolve_file /home/dev/ws/mock/signals.json "$SCRIPT_DIR/mock/signals.json")"
DEFAULT_VSS_JSON="$(_resolve_file /home/dev/ws/vss.json "$SCRIPT_DIR/data/vss-core/vss.json")"

DISABLE_DATABROKER=${DISABLE_DATABROKER:-""}
DATABROKER_ARGS=${DATABROKER_ARGS:-""}
SYNCER_SERVER_URL=${SYNCER_SERVER_URL:-"https://kit.digitalauto.tech"}
VSS_DATA=${VSS_DATA:-"$DEFAULT_VSS_JSON"}
RUNTIME_NAME=${RUNTIME_NAME:-"VSS4.0"} # Display name on the playground
MOCK_SIGNAL=${MOCK_SIGNAL:-"$DEFAULT_MOCK_SIGNAL"}
SITE_PACKAGES_DIR="${SITE_PACKAGES_DIR:-/home/dev/python-packages}"
GEN_MODEL_DIR="${GEN_MODEL_DIR:-/home/dev/python-packages/gen_model}"

# Append --vss flag to databroker args if a VSS file is available and not
# already specified in DATABROKER_ARGS.
if [ -n "$VSS_DATA" ] && [ -f "$VSS_DATA" ]; then
    case "$DATABROKER_ARGS" in
        *--vss*) ;;  # already provided
        *) DATABROKER_ARGS="$DATABROKER_ARGS --vss $VSS_DATA" ;;
    esac
    echo "VSS metadata: $VSS_DATA"
else
    echo "WARNING: No VSS metadata file found — databroker will start without signal type info"
fi

echo "Running as user: $(id -u -n)"
echo "Script directory: $SCRIPT_DIR"
echo "Architecture: $HOST_ARCH ($LOCAL_ARCH)"

if command -v mosquitto >/dev/null 2>&1; then
    mosquitto -d -c /etc/mosquitto/mosquitto-no-auth.conf
else
    echo "WARNING: mosquitto not found, skipping MQTT broker startup"
fi

if [ -z "$DISABLE_DATABROKER" ]; then
    if [ -z "$DATABROKER_BIN" ]; then
        echo "ERROR: databroker binary not found (checked /app/databroker and $SCRIPT_DIR/bin/$LOCAL_ARCH/databroker-$LOCAL_ARCH)"
        exit 1
    fi
    echo "Starting databroker: $DATABROKER_BIN"
    "$DATABROKER_BIN" $DATABROKER_ARGS &
fi

if [ -z "$NODE_KM_BIN" ]; then
    echo "WARNING: node-km binary not found, skipping kit-manager startup"
else
    echo "Starting kit-manager: $NODE_KM_BIN"
    "$NODE_KM_BIN" &
fi

sleep 4 # Ensure that the kuksa databroker and mosquitto start before the syncer

if [ -z "$SYNCER_BIN" ]; then
    echo "ERROR: kuksa-syncer binary not found"
    exit 1
fi
echo "Starting kuksa-syncer: $SYNCER_BIN"
"$SYNCER_BIN" &

if [ -n "$MOCK_SIGNAL" ]; then
    if [ -z "$MOCK_PROVIDER_BIN" ]; then
        echo "WARNING: mock-provider binary not found, skipping mock provider startup"
    else
        echo "Starting C++ mock provider: $MOCK_PROVIDER_BIN (signals: $MOCK_SIGNAL)"
        MOCK_SIGNAL="$MOCK_SIGNAL" VDB_ADDRESS="127.0.0.1:55555" \
            "$MOCK_PROVIDER_BIN" &
    fi
fi

tail -f /dev/null