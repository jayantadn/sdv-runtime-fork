// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT
//
// mock_main.cpp — C++ equivalent of mockprovider.py
//
// Usage:
//   MOCK_SIGNAL=/path/to/signals.json  VDB_ADDRESS=127.0.0.1:55555 ./mock-provider
//
// Writes PID to /home/dev/mockprovider.pid (same path expected by syncer).

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "mock_provider.hpp"
#include "mock_service.hpp"

namespace {

std::atomic_bool g_shutdown{ false };

void signalHandler(int /*sig*/) {
    g_shutdown.store(true);
}

} // namespace

int main() {
    // -----------------------------------------------------------------------
    // Configuration from environment (same env vars as Python mockprovider.py)
    // -----------------------------------------------------------------------
    const char* mockSignalEnv = std::getenv("MOCK_SIGNAL");
    const char* vdbEnv        = std::getenv("VDB_ADDRESS");

    std::string signalFile = mockSignalEnv
        ? std::string(mockSignalEnv)
        : "/home/dev/ws/mock/signals.json";

    std::string vdbAddress = vdbEnv
        ? std::string(vdbEnv)
        : "127.0.0.1:55555";

    // Parse host:port from vdbAddress
    std::string brokerHost = "127.0.0.1";
    int         brokerPort  = 55555;
    auto sep = vdbAddress.rfind(':');
    if (sep != std::string::npos) {
        brokerHost = vdbAddress.substr(0, sep);
        try { brokerPort = std::stoi(vdbAddress.substr(sep + 1)); } catch (...) {}
    }

    // -----------------------------------------------------------------------
    // Write PID file (mirrors mockprovider.py behaviour; syncer reads this to
    // kill/restart the process)
    // -----------------------------------------------------------------------
    constexpr const char* PID_FILE = "/home/dev/mockprovider.pid";
    {
        std::ofstream pf(PID_FILE);
        if (pf) {
            pf << ::getpid() << std::endl;
            std::cout << "[mock-provider] PID " << ::getpid()
                      << " written to " << PID_FILE << std::endl;
        } else {
            std::cerr << "[mock-provider] WARNING: could not write PID file "
                      << PID_FILE << std::endl;
        }
    }

    // -----------------------------------------------------------------------
    // Signal handling
    // -----------------------------------------------------------------------
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT,  signalHandler);

    // -----------------------------------------------------------------------
    // Start full mock behavior engine (MockService)
    // Replaces the simpler MockProvider with the complete behavior engine
    // that mirrors mockservice.py / behaviorexecutor.py / mock.py
    // -----------------------------------------------------------------------
    std::cout << "[mock-provider] Starting MockService. signals=" << signalFile
              << "  broker=" << vdbAddress << std::endl;

    sdv::mock::MockService service(signalFile, brokerHost, brokerPort);
    service.start();

    // mainLoop() blocks until stop() is called
    // Run it in a separate thread so signal handling can interrupt it cleanly
    std::thread loopThread([&service] { service.mainLoop(); });

    // -----------------------------------------------------------------------
    // Block until SIGTERM / SIGINT
    // -----------------------------------------------------------------------
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "[mock-provider] Shutting down..." << std::endl;
    service.stop();
    loopThread.join();

    // Remove PID file on clean exit
    std::remove(PID_FILE);
    return 0;
}
