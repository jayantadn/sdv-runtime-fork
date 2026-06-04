// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace sdv {
namespace project_utils {

// Recreate a project directory tree from a JSON description.
//
// json_string  — serialised JSON array following the "digitalauto" project
//                format used by the Python create_project_from_json helper:
//
//  [
//    { "name": "main.py", "type": "file",   "content": "..." },
//    { "name": "subdir",  "type": "folder", "items": [ ... ] }
//  ]
//
//  Files may carry an optional "isBase64" boolean; when true the content
//  field is decoded from Base64 before writing.
//
// base_dir     — root directory to write into (default: "app").
//                Existing directory is removed and re-created.
//
// Throws std::runtime_error on any I/O failure or malformed JSON.
void createProjectFromJson(const std::string& jsonString,
                           const std::string& baseDir = "app");

} // namespace project_utils
} // namespace sdv
