// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>

#include "syncer.hpp"

// Global syncer pointer used by the signal handler
static sdv::Syncer* g_syncer = nullptr;

static void handleSignal(int signum) {
    std::cout << "\nReceived signal " << signum
              << ", shutting down..." << std::endl;
    if (g_syncer) g_syncer->stop();
}

int main() {
    // Register POSIX signal handlers for graceful shutdown
    ::signal(SIGTERM, handleSignal);
    ::signal(SIGINT,  handleSignal);

    try {
        sdv::Syncer syncer;
        g_syncer = &syncer;
        syncer.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    g_syncer = nullptr;
    return EXIT_SUCCESS;
}
