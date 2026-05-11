# SDV-Runtime Architecture

## Component Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          HOST MACHINE (localhost)                           │
│                                                                             │
│  ┌──────────────────┐   gRPC/Protobuf    ┌─────────────────────────────┐   │
│  │  kuksa-syncer    │ ──────────────────▶│  KUKSA Databroker           │   │
│  │  (C++ binary)    │ ◀──────────────────│  bin/amd64/databroker-amd64 │   │
│  │                  │  127.0.0.1:55555   │  (Rust, v0.4.4)             │   │
│  └────────┬─────────┘                   └────────────┬────────────────┘   │
│           │                                          │                     │
│           │ Socket.IO                                │ gRPC/Protobuf       │
│           │ (WebSocket)                              │ 127.0.0.1:55555     │
│           │                                          │                     │
│  ┌────────▼─────────┐                   ┌────────────▼────────────────┐   │
│  │  Kit Manager     │                   │  mock-provider              │   │
│  │  node-km binary  │                   │  (C++ binary)               │   │
│  │  127.0.0.1:3090  │                   │  Subscribes actuator targets│   │
│  │  HTTP + WS       │                   │  Feeds initial signal values│   │
│  └────────▲─────────┘                   └─────────────────────────────┘   │
│           │                                                                 │
│           │ Socket.IO (WebSocket)                                           │
│           │ HTTP REST                                                       │
│           │                                                                 │
│  ┌────────┴─────────┐   MQTT (publish)   ┌─────────────────────────────┐   │
│  │  Browser /       │                   │  Mosquitto MQTT Broker      │   │
│  │  Playground UI   │                   │  127.0.0.1:1883             │   │
│  │  http://localhost│                   │  (no auth)                  │   │
│  │  :3090           │                   └─────────────────────────────┘   │
│  └──────────────────┘                                                      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                │
                │ (optional) Socket.IO over HTTPS/WSS
                ▼
     ┌─────────────────────┐
     │  kit.digitalauto.tech│
     │  Remote Kit Server  │
     │  https://...        │
     │  Port 443 (HTTPS)   │
     └─────────────────────┘
```

---

## Services & Ports

| Service | Binary / Process | Bind Address | Port | Protocol | Transport |
|---------|-----------------|--------------|------|----------|-----------|
| KUKSA Databroker | `bin/amd64/databroker-amd64` | `127.0.0.1` | **55555** | gRPC / Protobuf | TCP (plaintext, InsecureChannel) |
| Kit Manager (local) | `bin/amd64/node-km-x64` | `0.0.0.0` | **3090** | HTTP REST + Socket.IO | TCP (plain HTTP/WS) |
| Mosquitto MQTT | `mosquitto` | `0.0.0.0` | **1883** | MQTT v3.1.1 | TCP (no auth, no TLS) |
| Remote Kit Server | `kit.digitalauto.tech` | external | **443** | Socket.IO | TCP (HTTPS/WSS, TLS 1.2+) |

---

## Communication Paths

### 1. kuksa-syncer ↔ KUKSA Databroker

| Property | Value |
|----------|-------|
| Address | `127.0.0.1:55555` |
| Protocol | gRPC (HTTP/2) |
| Security | Plaintext (`InsecureChannelCredentials`) |
| IDL | Protobuf — `kuksa/val/v1/val.proto` + `types.proto` |
| RPCs used | `Get`, `Set`, `Subscribe`, `GetServerInfo` |
| Direction | Syncer → Broker (Get/Set); Broker → Syncer (Subscribe stream) |
| Keepalive | 20s ping / 10s timeout (subscription streams only) |

### 2. kuksa-syncer ↔ Kit Manager / Kit Server (Socket.IO)

| Property | Local Mode | Remote Mode |
|----------|-----------|-------------|
| URL | `http://localhost:3090` | `https://kit.digitalauto.tech` |
| Binary | `kuksa-syncer` (non-TLS) | `kuksa-syncer-tls` (TLS) |
| Protocol | Socket.IO v4 | Socket.IO v4 |
| Transport | WebSocket (plain WS) | WebSocket (WSS / TLS) |
| TLS Library | none | OpenSSL 3.0 via `sioclient_tls` |
| Port | **3090** | **443** |
| Direction | Bidirectional (full-duplex) |
| Env var | `SYNCER_SERVER_URL=http://localhost:3090` | `SYNCER_SERVER_URL=https://kit.digitalauto.tech` |

#### Socket.IO Events (Syncer → Server)

| Event | Payload | Purpose |
|-------|---------|---------|
| `register_kit` | `{kit_id, name}` | Register this runtime as an available kit |
| `messageToKit-kitReply` | `{kit_id, request_from, cmd, data, ...}` | Reply to commands from playground |
| `report-runtime-state` | `{kit_id, data: {noOfRunner, noSubscriber}}` | Heartbeat / status (every 5s) |

#### Socket.IO Events (Server → Syncer)

| Event | Payload | Purpose |
|-------|---------|---------|
| `messageToKit` | `{cmd, request_from, ...}` | Commands from the playground UI |

#### Commands received via `messageToKit`

| `cmd` value | Action |
|-------------|--------|
| `deploy_request` / `deploy-request` | Deploy + run Python app |
| `subscribe_apis` | Start VSS signal subscription |
| `unsubscribe_apis` | Stop VSS signal subscription |
| `list_mock_signal` | Return list of mock signals |
| `set_mock_signals` | Override mock signal values |
| `write_signals_value` | Write VSS signal to databroker |
| `reset_signals_value` | Reset VSS signals to defaults |
| `generate_vehicle_model` | Generate Python vehicle model from VSS JSON |
| `revert_vehicle_model` | Revert to previous vehicle model |
| `list_python_packages` | List installed Python packages |
| `install_python_packages` | Install Python packages via pip |

### 3. mock-provider ↔ KUKSA Databroker

| Property | Value |
|----------|-------|
| Address | `127.0.0.1:55555` (override: `VDB_ADDRESS` env var) |
| Protocol | gRPC (HTTP/2) |
| Security | Plaintext (`InsecureChannelCredentials`) |
| Operations | `Set` (write initial values) + `Subscribe` (actuator-target stream) |
| Signal file | `mock/signals.json` (override: `MOCK_SIGNAL` env var) |

### 4. Mosquitto MQTT Broker

| Property | Value |
|----------|-------|
| Address | `0.0.0.0:1883` |
| Protocol | MQTT v3.1.1 |
| Auth | None (`allow_anonymous true`) |
| Config | `mosquitto-no-auth.conf` |
| Usage | Available for vehicle app MQTT pub/sub (used by Velocitas apps) |

### 5. Kit Manager REST API (HTTP)

| Endpoint | Method | Response |
|----------|--------|---------|
| `GET /listAllKits` | GET | JSON list of registered runtime kits |
| `GET /listAllClient` | GET | JSON list of connected playground clients |
| `POST /convertCode` | POST | Converts playground Python code to Velocitas-compatible format |

---

## Environment Variables

| Variable | Default | Affects |
|----------|---------|---------|
| `SYNCER_SERVER_URL` | `http://localhost:3090` | Which Kit Server the syncer connects to; `http://` → `kuksa-syncer`, `https://` → `kuksa-syncer-tls` |
| `RUNTIME_NAME` | `LocalSDVRuntime` | Display name shown in the playground |
| `RUNTIME_PREFIX` | `Runtime-` | Prefix prepended to `RUNTIME_NAME` to form the kit ID |
| `MOCK_SIGNAL` | `mock/signals.json` | Signal definitions file for mock-provider |
| `VDB_ADDRESS` | `127.0.0.1:55555` | KUKSA Databroker address (mock-provider only) |
| `DISABLE_DATABROKER` | _(unset)_ | Set to any value to skip starting the databroker |
| `DATABROKER_ARGS` | _(empty)_ | Extra CLI args passed to the databroker binary |

---

## Binary Selection Logic

`start_services.sh` automatically picks the correct syncer binary based on the URL scheme:

```
SYNCER_SERVER_URL starts with https://  →  kuksa-syncer-tls  (OpenSSL, sioclient_tls)
SYNCER_SERVER_URL starts with http://   →  kuksa-syncer      (no TLS, sioclient)
```

---

## gRPC / Protobuf Details

| File | Purpose |
|------|---------|
| `proto/kuksa/val/v1/types.proto` | `Datapoint`, `Entry`, `Metadata`, `Field` enum, `DataType` enum |
| `proto/kuksa/val/v1/val.proto` | RPC service definition: `Get`, `Set`, `Subscribe`, `GetServerInfo`; `Error` message |
| Generated | `build/generated/kuksa/val/v1/*.pb.cc` / `*.grpc.pb.cc` |

`Error.reason` field is typed as `bytes` (not `string`) to avoid libprotobuf UTF-8 validation errors from databroker 0.4.4.

---

## Startup Sequence

```
1. mosquitto         starts  →  MQTT broker ready    (port 1883)
2. databroker        starts  →  gRPC server ready    (port 55555)
3. node-km (Kit Mgr) starts  →  HTTP/WS server ready (port 3090)
4. sleep 4s          (wait for databroker + kit-manager to be ready)
5. kuksa-syncer      starts  →  connects to databroker (55555) + Kit Server (3090 or 443)
6. mock-provider     starts  →  connects to databroker (55555), seeds signal values
```
