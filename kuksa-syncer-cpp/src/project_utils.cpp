// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "project_utils.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace sdv {
namespace project_utils {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ---------------------------------------------------------------------------
// Minimal Base64 decoder (RFC 4648) — avoids an external dependency
// ---------------------------------------------------------------------------
static const std::string kB64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Decode(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.size() * 3 / 4);

    int      bits = 0;
    int      bitCount = 0;

    for (unsigned char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        auto pos = kB64Chars.find(static_cast<char>(c));
        if (pos == std::string::npos) continue;

        bits = (bits << 6) | static_cast<int>(pos);
        bitCount += 6;
        if (bitCount >= 8) {
            bitCount -= 8;
            decoded.push_back(static_cast<char>((bits >> bitCount) & 0xFF));
        }
    }
    return decoded;
}

// ---------------------------------------------------------------------------
// Recursive item creator
// ---------------------------------------------------------------------------
static void filterMacOSX(json& items) {
    if (!items.is_array()) return;
    items.erase(
        std::remove_if(items.begin(), items.end(),
            [](const json& item) {
                return item.value("name", "") == "__MACOSX";
            }),
        items.end());
    for (auto& item : items) {
        if (item.value("type", "") == "folder" && item.contains("items")) {
            filterMacOSX(item["items"]);
        }
    }
}

static void createItems(const json& items, const fs::path& currentDir) {
    for (const auto& item : items) {
        std::string name = item.at("name").get<std::string>();
        std::string type = item.at("type").get<std::string>();
        fs::path    itemPath = currentDir / name;

        if (type == "folder") {
            fs::create_directories(itemPath);
            if (item.contains("items")) {
                createItems(item["items"], itemPath);
            }
        } else if (type == "file") {
            std::string content  = item.value("content", "");
            bool        isBase64 = item.value("isBase64", false);

            if (isBase64) {
                std::string decoded = base64Decode(content);
                std::ofstream f(itemPath, std::ios::binary | std::ios::trunc);
                if (!f) throw std::runtime_error("Cannot write: " +
                                                  itemPath.string());
                f.write(decoded.data(),
                        static_cast<std::streamsize>(decoded.size()));
            } else {
                std::ofstream f(itemPath, std::ios::out | std::ios::trunc);
                if (!f) throw std::runtime_error("Cannot write: " +
                                                  itemPath.string());
                f << content;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void createProjectFromJson(const std::string& jsonString,
                           const std::string& baseDir) {
    json data = json::parse(jsonString);  // throws on invalid JSON

    // Remove __MACOSX entries
    filterMacOSX(data);

    // If root is a single folder, treat its children as root
    if (data.is_array() && data.size() == 1 &&
        data[0].value("type", "") == "folder" &&
        data[0].contains("items")) {
        data = data[0]["items"];
    }

    fs::path base(baseDir);
    if (fs::exists(base)) fs::remove_all(base);
    fs::create_directories(base);

    createItems(data, base);
}

} // namespace project_utils
} // namespace sdv
