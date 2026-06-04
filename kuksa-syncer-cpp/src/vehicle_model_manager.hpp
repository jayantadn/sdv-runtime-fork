// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace sdv {
namespace vehicle_model {

// ---------------------------------------------------------------------------
// generateVehicleModel
//
// Fully native — no Python subprocess involved.
//   1. Parses the VSS JSON and fixes invalid/missing unit fields in C++.
//   2. Writes the corrected vss.json to disk.
//   3. Generates the velocitas Python vehicle model natively in C++
//      (walks the VSS tree and emits the __init__.py directly — no python3).
//   4. Safety-net correctParentClass pass on the generated __init__.py.
//   5. Moves generated model into the python-packages directory.
//   6. Optionally restarts the databroker.
//
// input_json — serialised JSON string of the VSS tree.
//
// Throws std::runtime_error on failure.
// ---------------------------------------------------------------------------
void generateVehicleModel(const std::string& inputJson);

// ---------------------------------------------------------------------------
// revertVehicleModel
//
// Ports Python vehicle_model_manager.revert_vehicle_model():
//   Removes the current /home/dev/python-packages/vehicle directory and
//   copies the std_vehicle backup back.  Also restores the default vss.json
//   and restarts the databroker.
// ---------------------------------------------------------------------------
void revertVehicleModel();

// ---------------------------------------------------------------------------
// restartDatabroker
//
// Kill any running databroker process and start a fresh instance.
// ---------------------------------------------------------------------------
void restartDatabroker();

} // namespace vehicle_model
} // namespace sdv
