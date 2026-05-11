// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "vehicle_model_manager.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sdv {
namespace vehicle_model {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Run a shell command synchronously; throw on non-zero exit.
static void runCommand(const std::string& cmd) {
    int ret = ::system(cmd.c_str());
    if (ret != 0) {
        throw std::runtime_error("Command failed (exit " +
                                 std::to_string(WEXITSTATUS(ret)) +
                                 "): " + cmd);
    }
}

// Copy srcFile → dstFile, overwriting destination.
static void copyAndOverride(const fs::path& src, const fs::path& dst) {
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

// Extract the first class name from generated Python code.
static std::string extractFirstClassName(const std::string& pyCode) {
    static const std::regex kClassRe(R"(class\s+(\w+)\s*(\(.*?\))?:)");
    std::smatch m;
    if (std::regex_search(pyCode, m, kClassRe)) return m[1];
    return {};
}

// Fix the instantiation line in the generated __init__.py so it uses the
// correct class name (mirrors Python correct_parent_class_in_vehicle_model).
static void correctParentClass(const fs::path& filePath) {
    std::ifstream in(filePath);
    if (!in) return;
    std::string contents((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    in.close();

    std::string className = extractFirstClassName(contents);
    if (className.empty()) {
        std::cout << "No class definitions found in " << filePath << std::endl;
        return;
    }

    std::string oldLine = "vehicle = Vehicle(\"Vehicle\")";
    std::string newLine = "vehicle = " + className + "(\"" + className + "\")";

    auto pos = contents.find(oldLine);
    if (pos != std::string::npos) {
        contents.replace(pos, oldLine.size(), newLine);
        std::ofstream out(filePath, std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write: " + filePath.string());
        out << contents;
        std::cout << "Parent class corrected to '" << className
                  << "' in " << filePath << std::endl;
    }
}

// ---------------------------------------------------------------------------
// restartDatabroker
// ---------------------------------------------------------------------------
void restartDatabroker() {
    // Kill existing instances
    std::string killCmd = "pkill -f /app/databroker 2>/dev/null || true";
    ::system(killCmd.c_str());
    ::sleep(1);

    // Start fresh
    pid_t child = ::fork();
    if (child == 0) {
        ::setsid();
        ::execl("/app/databroker", "databroker", nullptr);
        ::_exit(127);
    } else if (child > 0) {
        std::cout << "Databroker restarted (PID " << child << ")." << std::endl;
    } else {
        std::cerr << "fork() failed restarting databroker: "
                  << ::strerror(errno) << std::endl;
    }
}

// ---------------------------------------------------------------------------
// generateVehicleModel
//
// The velocitas model generator is a Python tool; we invoke it as a
// subprocess rather than re-implementing it in C++.
// ---------------------------------------------------------------------------
void generateVehicleModel(const std::string& inputJson) {
    static const fs::path kVssPath     = "/home/dev/ws/vss.json";
    static const fs::path kGenModel    = "/home/dev/ws/gen_model";
    static const fs::path kPkgVehicle  = "/home/dev/python-packages/vehicle";
    static const fs::path kUnitFile    =
        "/home/dev/python-packages/vehicle_signal_specification/spec/units.yaml";
    static const fs::path kIncludeDir  =
        "/home/dev/python-packages/vehicle_signal_specification/spec";

    // 1. Write vss.json
    {
        std::ofstream f(kVssPath, std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot write vss.json");
        f << inputJson;
    }

    // 2. Remove old vehicle package
    if (fs::exists(kPkgVehicle)) fs::remove_all(kPkgVehicle);

    // 3. Invoke Python model generator
    std::ostringstream cmd;
    cmd << "python3 -c \""
        << "from velocitas.model_generator import generate_model; "
        << "generate_model("
        <<     "'" << kVssPath.string()          << "', "
        <<     "['" << kUnitFile.string()         << "'], "
        <<     "'python', "
        <<     "'" << kGenModel.string()          << "', "
        <<     "'vehicle', True, "
        <<     "'" << kIncludeDir.string()        << "'"
        << ")\"";
    runCommand(cmd.str());

    // 4. Correct parent class
    fs::path initFile = kGenModel / "vehicle" / "__init__.py";
    if (fs::exists(initFile)) correctParentClass(initFile);

    // 5. Move generated model
    if (fs::exists(kPkgVehicle)) fs::remove_all(kPkgVehicle);
    fs::rename(kGenModel / "vehicle", kPkgVehicle);

    // 6. Restart databroker (unless disabled)
    const char* disable = ::getenv("DISABLE_DATABROKER");
    if (!disable || std::string(disable).empty()) {
        restartDatabroker();
    }
}

// ---------------------------------------------------------------------------
// revertVehicleModel
// ---------------------------------------------------------------------------
void revertVehicleModel() {
    static const fs::path kPkgVehicle  = "/home/dev/python-packages/vehicle";
    static const fs::path kStdVehicle  = "/home/dev/python-packages/std_vehicle";
    static const fs::path kDefaultVss  = "/home/dev/ws/default_vss.json";
    static const fs::path kVssPath     = "/home/dev/ws/vss.json";

    if (fs::exists(kPkgVehicle)) fs::remove_all(kPkgVehicle);

    if (!fs::exists(kStdVehicle)) {
        throw std::runtime_error("std_vehicle backup not found at " +
                                 kStdVehicle.string());
    }
    fs::copy(kStdVehicle, kPkgVehicle,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);

    copyAndOverride(kDefaultVss, kVssPath);
    restartDatabroker();

    std::cout << "Reverted back to standard vehicle model." << std::endl;
}

} // namespace vehicle_model
} // namespace sdv
