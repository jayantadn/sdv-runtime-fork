// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace sdv {
namespace utils {

// ── Process utilities ────────────────────────────────────────────────────────

// Returns true if a process whose executable name equals process_name is
// currently running (uses /proc on Linux).
bool isProcessRunning(const std::string& processName);

// Send SIGKILL to all processes whose /proc/<pid>/cmdline contains needle.
void killProcessByName(const std::string& needle);

// Write code string to file on disk.  Creates or truncates the file.
void writeCodeToFile(const std::string& code,
                     const std::string& filename = "main.py");

// ── Mock provider lifecycle ─────────────────────────────────────────────────

void stopMockService (const std::string& pidFile = "/home/dev/mockprovider.pid");
void startMockService(const std::string& mockScript = "/home/dev/ws/mock/mockprovider.py");
void restartMockProvider();

// ── Mock signal file helpers ─────────────────────────────────────────────────
// (These helpers are thin wrappers; heavier logic lives in Syncer)

// Read the full signal list from the signals JSON file.
// Returns the raw JSON string, or "{}" on error.
std::string readMockSignalFile(const std::string& path);

// Overwrite the signal file with the provided JSON string.
void writeMockSignalFile(const std::string& path, const std::string& jsonContent);

} // namespace utils
} // namespace sdv
