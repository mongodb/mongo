// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/tools/workload_simulation/simulator_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/tools/workload_simulation/simulation.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/str.h"

#include <iostream>
#include <map>
#include <string_view>
#include <utility>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::workload_simulation {

SimulatorParams simulatorGlobalParams;

std::string getSimulatorHelp(std::string_view name,
                             const optionenvironment::OptionSection& options) {
    StringBuilder sb;
    sb << "usage: " << name << " [options] [suite]...\n"
       << options.helpString() << "suite: run the specified workload suite(s) only\n";
    return sb.str();
}

bool handlePreValidationSimulatorOptions(const optionenvironment::Environment& params,
                                         const std::vector<std::string>& args) {
    if (params.count("help")) {
        std::cout << getSimulatorHelp(args[0], optionenvironment::startupOptions) << std::endl;
        return false;
    }

    if (params.count("list")) {
        SimulationRegistry::get().list();
        return false;
    }

    return true;
}

Status storeSimulatorOptions(const optionenvironment::Environment& params,
                             const std::vector<std::string>& args) {
    if (!params.count("setParameter")) {
        return Status::OK();
    }

    std::map<std::string, std::string> parameters =
        params["setParameter"].as<std::map<std::string, std::string>>();
    auto* paramSet = ServerParameterSet::getNodeParameterSet();
    for (const auto& it : parameters) {
        auto parameter = paramSet->getIfExists(it.first);
        if (nullptr == parameter) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Illegal --setParameter parameter: \"" << it.first << "\""};
        }
        if (!parameter->allowedToChangeAtStartup()) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Cannot use --setParameter to set \"" << it.first
                                  << "\" at startup"};
        }
        Status status = parameter->setFromString(it.second, boost::none);
        if (!status.isOK()) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Bad value for parameter \"" << it.first
                                  << "\": " << status.reason()};
        }

        LOGV2(7782104,
              "Setting server parameter",
              "parameter"_attr = it.first,
              "value"_attr = it.second);
    }

    return Status::OK();
}
}  // namespace mongo::workload_simulation
