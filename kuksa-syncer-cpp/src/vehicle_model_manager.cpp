// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "vehicle_model_manager.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace sdv {
namespace vehicle_model {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Run a subprocess synchronously via fork+execvp (no shell — safe from injection).
// args[0] is the executable (resolved via PATH); remaining entries are arguments.
// Throws std::runtime_error on fork failure or non-zero exit.
static void runSubprocess(const std::vector<std::string>& args) {
    if (args.empty()) throw std::runtime_error("runSubprocess: empty argument list");

    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t child = ::fork();
    if (child < 0) {
        throw std::runtime_error(std::string("fork() failed: ") + ::strerror(errno));
    }
    if (child == 0) {
        ::execvp(argv[0], const_cast<char* const*>(argv.data()));
        ::_exit(127); // execvp failed
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            throw std::runtime_error(std::string("waitpid failed: ") + ::strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error(args[0] + " exited with status " +
                                 std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1));
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

// ---------------------------------------------------------------------------
// loadValidUnits — parse a VSS units.yaml and return the set of valid unit keys
// ---------------------------------------------------------------------------
static std::unordered_set<std::string> loadValidUnits(const fs::path& yamlPath) {
    std::unordered_set<std::string> units;
    std::ifstream f(yamlPath);
    if (!f) return units;

    std::string line;
    bool inUnitsSection = false;
    while (std::getline(f, line)) {
        if (line == "units:") { inUnitsSection = true; continue; }
        if (!inUnitsSection) continue;

        if (line.size() >= 3 && line[0] == ' ' && line[1] == ' ' && line[2] != ' ') {
            // Top-level key under units: — everything before the first colon
            auto colon = line.find(':', 2);
            if (colon != std::string::npos) {
                std::string key = line.substr(2, colon - 2);
                while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
                    key.pop_back();
                if (!key.empty()) units.insert(key);
            }
        } else if (!line.empty() && line[0] != ' ' && line[0] != '#') {
            inUnitsSection = false; // entered a different top-level section
        }
    }
    return units;
}

// ---------------------------------------------------------------------------
// traverseAndFix — walk the VSS JSON tree and assign unit "m" to any signal
// that has a missing or unrecognised unit field (mirrors Python traverse_and_fix).
// ---------------------------------------------------------------------------
static void traverseAndFix(nlohmann::json& tree,
                            const std::unordered_set<std::string>& validUnits,
                            const std::string& currentPath = "") {
    if (!tree.is_object()) return;
    for (auto& [key, value] : tree.items()) {
        if (!value.is_object()) continue;
        std::string newPath = currentPath.empty() ? key : currentPath + "." + key;

        auto typeIt = value.find("type");
        if (typeIt != value.end() && typeIt->is_string() &&
            typeIt->get<std::string>() != "branch") {
            // It is a signal node — validate/fix its unit field
            std::string unit;
            auto unitIt = value.find("unit");
            if (unitIt != value.end() && unitIt->is_string())
                unit = unitIt->get<std::string>();
            if (unit.empty() || validUnits.find(unit) == validUnits.end()) {
                value["unit"] = "m";
                std::cout << "Set default unit 'm' for signal " << newPath << std::endl;
            }
        }

        auto childrenIt = value.find("children");
        if (childrenIt != value.end() && childrenIt->is_object())
            traverseAndFix(*childrenIt, validUnits, newPath);
    }
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
// 1. Parses the incoming VSS JSON and fixes invalid/missing unit fields in
//    C++ (equiv. of Python traverse_and_fix) — no Python needed for this step.
// 2. Writes the corrected tree to vss.json on disk.
// 3. Invokes the velocitas Python model generator via fork+execvp (no shell,
//    no command-injection risk; all arguments are static paths, not user data).
// 4. Corrects the parent-class instantiation in the generated __init__.py.
// 5. Moves the generated model into the python-packages directory.
// 6. Optionally restarts the databroker.
// ---------------------------------------------------------------------------
void generateVehicleModel(const std::string& inputJson) {
    static const fs::path kVssPath    = "/home/dev/ws/vss.json";
    static const fs::path kGenModel   = "/home/dev/ws/gen_model";
    static const fs::path kPkgVehicle = "/home/dev/python-packages/vehicle";
    static const fs::path kUnitFile   =
        "/home/dev/python-packages/vehicle_signal_specification/spec/units.yaml";
    static const fs::path kIncludeDir =
        "/home/dev/python-packages/vehicle_signal_specification/spec";

    // 1. Parse JSON and fix invalid unit fields natively (no Python subprocess)
    nlohmann::json data = nlohmann::json::parse(inputJson);
    auto validUnits = loadValidUnits(kUnitFile);
    traverseAndFix(data, validUnits);

    // 2. Write corrected vss.json
    {
        std::ofstream f(kVssPath, std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot write vss.json");
        f << data.dump(4);
    }

    // 3. Remove stale vehicle package
    if (fs::exists(kPkgVehicle)) fs::remove_all(kPkgVehicle);

    // 4. Invoke the velocitas Python model generator via fork+execvp.
    //    Arguments are passed as a proper array — the shell is never involved,
    //    eliminating any command-injection surface.
    runSubprocess({
        "python3", "-c",
        "from velocitas.model_generator import generate_model;"
        "generate_model('" + kVssPath.string() + "',"
        "['" + kUnitFile.string() + "'],"
        "'python','" + kGenModel.string() + "',"
        "'vehicle',True,'" + kIncludeDir.string() + "')"
    });

    // 5. Correct parent class in generated __init__.py
    fs::path initFile = kGenModel / "vehicle" / "__init__.py";
    if (fs::exists(initFile)) correctParentClass(initFile);

    // 6. Move generated model into python-packages
    if (fs::exists(kPkgVehicle)) fs::remove_all(kPkgVehicle);
    fs::rename(kGenModel / "vehicle", kPkgVehicle);

    // 7. Restart databroker (unless disabled)
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
