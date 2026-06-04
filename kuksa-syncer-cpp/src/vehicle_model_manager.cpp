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
#include <unordered_map>
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
// Native velocitas vehicle-model generator
// ---------------------------------------------------------------------------
// Replaces the former `python3 -c velocitas.model_generator.generate_model`
// subprocess call.  Walks the VSS JSON tree in post-order (children before
// parents) and emits a Python __init__.py whose structure matches the output
// produced by the velocitas SDK model generator.
// ---------------------------------------------------------------------------

// Map a VSS datatype string to the corresponding velocitas DataPoint class.
static std::string vssDataTypeToPython(const std::string& dt) {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"boolean",   "DataPointBoolean"},
        {"float",     "DataPointFloat"},
        {"double",    "DataPointDouble"},
        {"int8",      "DataPointInt8"},
        {"int16",     "DataPointInt16"},
        {"int32",     "DataPointInt32"},
        {"int64",     "DataPointInt64"},
        {"uint8",     "DataPointUint8"},
        {"uint16",    "DataPointUint16"},
        {"uint32",    "DataPointUint32"},
        {"uint64",    "DataPointUint64"},
        {"string",    "DataPointString"},
        {"boolean[]", "DataPointBooleanArray"},
        {"float[]",   "DataPointFloat32Array"},
        {"double[]",  "DataPointFloat64Array"},
        {"int8[]",    "DataPointInt8Array"},
        {"int16[]",   "DataPointInt16Array"},
        {"int32[]",   "DataPointInt32Array"},
        {"int64[]",   "DataPointInt64Array"},
        {"uint8[]",   "DataPointUint8Array"},
        {"uint16[]",  "DataPointUint16Array"},
        {"uint32[]",  "DataPointUint32Array"},
        {"uint64[]",  "DataPointUint64Array"},
        {"string[]",  "DataPointStringArray"},
    };
    auto it = kMap.find(dt);
    return (it != kMap.end()) ? it->second : "DataPointString";
}

// One Python class definition to be emitted.
struct ClassDef {
    std::string className;
    // child branch attributes: (attribute_name, child_class_name)
    std::vector<std::pair<std::string,std::string>> branches;
    // signal attributes: (attribute_name, DataPointType)
    std::vector<std::pair<std::string,std::string>> signals;
};

// Collect all DataPoint types actually used (for the import block).
static void collectUsedTypes(const std::vector<ClassDef>& classes,
                             std::unordered_set<std::string>& out) {
    for (const auto& c : classes)
        for (const auto& s : c.signals)
            out.insert(s.second);
}

// Disambiguate a proposed class name so it is unique within `seen`.
// Strategy: if `name` is already taken, prefix with the parent name.
static std::string uniqueClassName(const std::string& parentClass,
                                   const std::string& localName,
                                   std::unordered_map<std::string,int>& nameCount) {
    auto& cnt = nameCount[localName];
    ++cnt;
    if (cnt == 1) return localName;          // first use — no suffix needed
    // collision: qualify with parent class name
    return parentClass + "_" + localName;
}

// Post-order DFS: visit all children before the current node so child classes
// are always defined before the class that references them.
static std::string walkVssTree(const std::string& localName,
                                const nlohmann::json& node,
                                const std::string& parentClassName,
                                std::vector<ClassDef>& classes,
                                std::unordered_map<std::string,int>& nameCount) {
    // Determine if this node is a branch (has children) or a signal leaf.
    bool isBranch = node.contains("children") && node["children"].is_object();

    if (!isBranch) {
        // Leaf signal — no class emitted, just return the DataPoint type.
        std::string dt = node.value("datatype", "string");
        return vssDataTypeToPython(dt);  // caller uses this as the DataPoint type
    }

    // Branch: assign a unique class name.
    std::string className = uniqueClassName(parentClassName, localName, nameCount);

    ClassDef def;
    def.className = className;

    const auto& children = node["children"];
    for (auto it = children.begin(); it != children.end(); ++it) {
        const std::string& childName = it.key();
        const auto& childNode = it.value();

        bool childIsBranch = childNode.contains("children") && childNode["children"].is_object();
        if (childIsBranch) {
            // Recurse first (post-order) to get the child's class name.
            std::string childClass = walkVssTree(childName, childNode, className,
                                                 classes, nameCount);
            def.branches.emplace_back(childName, childClass);
        } else {
            std::string dt = childNode.value("datatype", "string");
            def.signals.emplace_back(childName, vssDataTypeToPython(dt));
        }
    }

    classes.push_back(std::move(def));
    return className;
}

// Build the complete Python source for the vehicle model __init__.py.
static std::string buildPythonModel(const nlohmann::json& vssRoot) {
    // vssRoot is expected to have a single top-level key (e.g. "Vehicle").
    if (!vssRoot.is_object() || vssRoot.empty())
        throw std::runtime_error("VSS JSON root must be a non-empty object");

    std::string rootKey  = vssRoot.begin().key();
    const auto& rootNode = vssRoot.begin().value();

    std::vector<ClassDef> classes;
    std::unordered_map<std::string,int> nameCount;

    std::string rootClass = walkVssTree(rootKey, rootNode, "", classes, nameCount);

    // Collect which DataPoint types are actually referenced.
    std::unordered_set<std::string> usedTypes;
    collectUsedTypes(classes, usedTypes);

    // Emit the Python source.
    std::ostringstream out;

    // --- imports ---
    out << "from velocitas_sdk.model import (\n";
    // Emit used DataPoint types in deterministic order.
    static const std::vector<std::string> kAllTypes = {
        "DataPointBoolean", "DataPointFloat", "DataPointDouble",
        "DataPointInt8",  "DataPointInt16",  "DataPointInt32",  "DataPointInt64",
        "DataPointUint8", "DataPointUint16", "DataPointUint32", "DataPointUint64",
        "DataPointString",
        "DataPointBooleanArray",
        "DataPointFloat32Array", "DataPointFloat64Array",
        "DataPointInt8Array",  "DataPointInt16Array",
        "DataPointInt32Array", "DataPointInt64Array",
        "DataPointUint8Array", "DataPointUint16Array",
        "DataPointUint32Array","DataPointUint64Array",
        "DataPointStringArray",
    };
    for (const auto& t : kAllTypes) {
        if (usedTypes.count(t)) out << "    " << t << ",\n";
    }
    out << "    Model,\n";
    out << "    ModelCollection,\n";
    out << "    Dictionary,\n";
    out << "    NamedRange\n";
    out << ")\n\n\n";

    // --- class definitions (children-first order) ---
    for (const auto& cls : classes) {
        bool isRoot = (cls.className == rootClass);

        out << "class " << cls.className << "(Model):\n";
        if (isRoot)
            out << "    def __init__(self, name: str = \"" << rootClass
                << "\", parent=None):\n";
        else
            out << "    def __init__(self, name, parent):\n";
        out << "        super().__init__(name, parent)\n";

        for (const auto& s : cls.signals)
            out << "        self." << s.first << " = "
                << s.second << "(\"" << s.first << "\", self)\n";
        for (const auto& b : cls.branches)
            out << "        self." << b.first << " = "
                << b.second << "(\"" << b.first << "\", self)\n";
        out << "\n\n";
    }

    // --- module-level singleton ---
    out << "vehicle = " << rootClass << "(\"" << rootClass << "\")\n";

    return out.str();
}

// Entry point: parse vssJson, generate the Python model, write it to outDir/vehicle/__init__.py.
static void generateModelNative(const std::string& vssJson,
                                 const fs::path& outDir) {
    nlohmann::json vssRoot = nlohmann::json::parse(vssJson);

    std::string pySource = buildPythonModel(vssRoot);

    fs::path vehicleDir = outDir / "vehicle";
    fs::create_directories(vehicleDir);

    fs::path initFile = vehicleDir / "__init__.py";
    std::ofstream f(initFile, std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write " + initFile.string());
    f << pySource;

    std::cout << "Native model generator: wrote " << initFile << std::endl;
}

// ---------------------------------------------------------------------------
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
// Fully native — no Python subprocess involved.
// 1. Parses the incoming VSS JSON and fixes invalid/missing unit fields.
// 2. Writes the corrected tree to vss.json on disk.
// 3. Generates the velocitas Python vehicle model via generateModelNative()
//    (C++ walks the VSS tree and emits the __init__.py directly).
// 4. Safety-net correctParentClass pass on the generated __init__.py.
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

    // 4. Generate the velocitas Python vehicle model natively (no python3 subprocess).
    if (fs::exists(kGenModel)) fs::remove_all(kGenModel);
    fs::create_directories(kGenModel);
    generateModelNative(data.dump(), kGenModel);
    // correctParentClass is no longer needed — the native generator always
    // emits the correct instantiation line — but kept as a safety net.
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
