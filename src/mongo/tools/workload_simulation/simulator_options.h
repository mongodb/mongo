// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"
#include "mongo/util/options_parser/option_section.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo::workload_simulation {

struct SimulatorParams {
    std::vector<std::string> suites;
    std::string filter;
};

extern SimulatorParams simulatorGlobalParams;

Status addSimulatorOptions(optionenvironment::OptionSection* options);

std::string getSimulatorHelp(std::string_view name,
                             const optionenvironment::OptionSection& options);

/**
 * Handle options that should come before validation, such as "help".
 *
 * Returns false if an option was found that implies we should prematurely exit with success.
 */
bool handlePreValidationSimulatorOptions(const optionenvironment::Environment& params,
                                         const std::vector<std::string>& args);

Status storeSimulatorOptions(const optionenvironment::Environment& params,
                             const std::vector<std::string>& args);

}  // namespace mongo::workload_simulation
