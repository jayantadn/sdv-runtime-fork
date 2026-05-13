<!--
  Copyright (c) 2025 Eclipse Foundation.

  This program and the accompanying materials are made available under the
  terms of the MIT License which is available at
  https://opensource.org/licenses/MIT.

  SPDX-License-Identifier: MIT
-->

# kuksa-syncer-cpp

C++17 port of the Python `kuksa-syncer` runtime orchestrator component of the
SDV (Software Defined Vehicle) runtime.

## Overview

`kuksa-syncer` is the bridge between the **digitalauto Kit Server** (Socket.IO)
and the **KUKSA databroker** (gRPC / VSS signals). It:

| Responsibility | Python original | C++ equivalent |
|---|---|---|
| Socket.IO client | `python-socketio` | `socket.io-client-cpp` |
| KUKSA gRPC client | `kuksa-client 0.4.3` | custom `KuksaClient` wrapper (gRPC C++) |
| Subprocess launch | `subpiper` module | `SubPiper` class (POSIX fork/exec + pipes) |
| JSON handling | built-in `json` | `nlohmann/json` |
| Async tasks | `asyncio` | POSIX threads (`std::thread`) |

---

## Directory structure

```
kuksa-syncer-cpp/
├── CMakeLists.txt
├── README.md
├── proto/
│   └── kuksa/val/v1/
│       ├── types.proto        # KUKSA VAL v1 data types
│       └── val.proto          # KUKSA VAL v1 service definition
└── src/
    ├── main.cpp               # Entry point, signal handling
    ├── syncer.hpp / .cpp      # Main orchestrator (Socket.IO + tickers)
    ├── kuksa_client.hpp / .cpp # gRPC wrapper (get/set values, metadata)
    ├── subpiper.hpp / .cpp    # POSIX subprocess with piped I/O
    ├── utils.hpp / .cpp       # Process utils, file helpers, mock provider
    ├── project_utils.hpp/.cpp # Create project tree from JSON
    └── vehicle_model_manager.hpp/.cpp  # Vehicle model gen / revert
```

---

## Build prerequisites (Ubuntu 22.04 / 24.04)

```bash
# System packages
sudo apt-get install -y \
    build-essential cmake \
    libgrpc++-dev libprotobuf-dev \
    protobuf-compiler protobuf-compiler-grpc \
    libssl-dev

# nlohmann/json and socket.io-client-cpp are fetched automatically
# by CMake FetchContent during configure.
```

---

## Build

```bash
cd kuksa-syncer-cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary is produced at `build/kuksa-syncer`.

---

## Configuration (environment variables)

| Variable | Default | Description |
|---|---|---|
| `SYNCER_SERVER_URL` | `https://kit.digitalauto.tech` | Kit Server URL |
| `RUNTIME_NAME` | `MyRuntime` | Runtime display name |
| `RUNTIME_PREFIX` | `Runtime-` | Prefix prepended to runtime name |
| `DISABLE_DATABROKER` | _(unset)_ | Set to any value to skip databroker checks |

---

## Runtime architecture

```
main thread          — Socket.IO event loop (blocking in sio::client::sync_close)
tickerFast thread    — every 300 ms: poll subscribed VSS signals, push to clients
ticker thread        — every 1 s:    evict stale subscribers / runners
ticker5s thread      — every 5 s:    report runtime state to subscribers
```

Shared state (`lsOfRunner_`, `lsOfApiSubscriber_`) is protected by a single
`std::mutex`.  The KUKSA gRPC client is also mutex-guarded for thread safety.

---

## Ported commands

| Socket.IO command | Behaviour |
|---|---|
| `deploy_request` / `deploy-request` | Write code to file, simulate deploy steps |
| `subscribe_apis` | Add client to VSS signal poll list |
| `unsubscribe_apis` | Remove client from poll list |
| `list_mock_signal` | Return current mock signal JSON |
| `set_mock_signals` | Overwrite mock signals, restart mock provider |
| `write_signals_value` | Write sensor/actuator values to databroker |
| `reset_signals_value` | Reset signals from the signals.json file |
| `generate_vehicle_model` | Generate new VSS vehicle model (calls Python generator) |
| `revert_vehicle_model` | Restore std_vehicle model |
| `list_python_packages` | Run `pip freeze` and return output |
| `install_python_packages` | Run `pip install` for specified packages |
| `run_python_app` | Launch Python or binary app as sub-process |
| `run_bin_app` | Launch compiled binary from `/home/dev/output/` |
| `stop_python_app` | Kill running app for given client |
| `get-runtime-info` | Return runner/subscriber state |

---

## Notes on Python interop

Unit validation (`traverse_and_fix`) and all mock-provider lifecycle management
are now implemented in native C++.  The `vehicle_model_manager` still invokes
the Python `velocitas` model generator as a subprocess via `fork`+`execvp`
(no shell — no injection surface) because re-implementing the generator itself
in C++ would be a separate project.  All other hot-path logic runs as native C++.

---

## License

MIT — see [LICENSE](../LICENSE)
