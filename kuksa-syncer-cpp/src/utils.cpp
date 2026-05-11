// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "utils.hpp"

#include <algorithm>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

namespace sdv {
namespace utils {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// isProcessRunning — inspects /proc/<pid>/comm on Linux
// ---------------------------------------------------------------------------
bool isProcessRunning(const std::string& processName) {
    DIR* proc = ::opendir("/proc");
    if (!proc) return false;

    bool found = false;
    struct dirent* entry;
    while ((entry = ::readdir(proc)) != nullptr) {
        // Only look at numeric directories
        std::string name = entry->d_name;
        bool isNumeric = !name.empty() &&
            std::all_of(name.begin(), name.end(), ::isdigit);
        if (!isNumeric) continue;

        std::string commPath = "/proc/" + name + "/comm";
        std::ifstream f(commPath);
        if (!f.is_open()) continue;
        std::string comm;
        std::getline(f, comm);
        if (comm == processName) { found = true; break; }
    }
    ::closedir(proc);
    return found;
}

// ---------------------------------------------------------------------------
// killProcessByName — kills all /proc/<pid>/cmdline entries matching needle
// ---------------------------------------------------------------------------
void killProcessByName(const std::string& needle) {
    DIR* proc = ::opendir("/proc");
    if (!proc) return;

    struct dirent* entry;
    while ((entry = ::readdir(proc)) != nullptr) {
        std::string name = entry->d_name;
        bool isNumeric = !name.empty() &&
            std::all_of(name.begin(), name.end(), ::isdigit);
        if (!isNumeric) continue;

        std::string cmdlinePath = "/proc/" + name + "/cmdline";
        std::ifstream f(cmdlinePath, std::ios::binary);
        if (!f.is_open()) continue;

        std::string cmdline((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        // Replace NUL separators with space for substring search
        for (auto& c : cmdline) if (c == '\0') c = ' ';

        if (cmdline.find(needle) != std::string::npos) {
            pid_t pid = static_cast<pid_t>(std::stoi(name));
            ::kill(pid, SIGKILL);
        }
    }
    ::closedir(proc);
}

// ---------------------------------------------------------------------------
// writeCodeToFile
// ---------------------------------------------------------------------------
void writeCodeToFile(const std::string& code, const std::string& filename) {
    std::ofstream f(filename, std::ios::out | std::ios::trunc);
    if (!f) {
        throw std::runtime_error("Cannot open file for writing: " + filename);
    }
    f << code;
}

// ---------------------------------------------------------------------------
// Mock provider management
// ---------------------------------------------------------------------------
void stopMockService(const std::string& pidFile) {
    if (!fs::exists(pidFile)) {
        std::cout << "mockprovider pid file at '" << pidFile
                  << "' does not exist." << std::endl;
        return;
    }

    std::ifstream f(pidFile);
    if (!f) return;

    pid_t pid = -1;
    f >> pid;
    if (pid <= 0) return;

    if (::kill(pid, SIGKILL) == 0) {
        std::cout << "mockprovider with PID " << pid << " has been killed."
                  << std::endl;
    } else {
        std::cout << "No process found with PID " << pid << std::endl;
    }
}

void startMockService(const std::string& mockScript) {
    std::cout << "Starting mock provider..." << std::endl;
    pid_t child = ::fork();
    if (child == 0) {
        // Detach from parent
        ::setsid();
        ::execl("/usr/bin/python3", "python3", mockScript.c_str(), nullptr);
        // fallback to python
        ::execl("/usr/bin/python", "python", mockScript.c_str(), nullptr);
        ::_exit(127);
    } else if (child > 0) {
        std::cout << "Mock provider started (PID " << child << ")." << std::endl;
    } else {
        std::cerr << "fork() failed starting mock provider: "
                  << ::strerror(errno) << std::endl;
    }
}

void restartMockProvider() {
    stopMockService();
    // Brief pause to allow sockets to be released
    ::usleep(500'000);
    startMockService();
}

// ---------------------------------------------------------------------------
// Signal file helpers
// ---------------------------------------------------------------------------
std::string readMockSignalFile(const std::string& path) {
    if (!fs::exists(path)) return "[]";
    std::ifstream f(path);
    if (!f) return "[]";
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

void writeMockSignalFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write mock signal file: " + path);
    f << content;
}

} // namespace utils
} // namespace sdv
