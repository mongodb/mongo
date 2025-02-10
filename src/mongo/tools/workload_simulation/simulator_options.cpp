/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/tools/workload_simulation/simulator_options.h"

#include <boost/filesystem/operations.hpp>
#include <iostream>
#include <map>
#include <utility>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/tools/workload_simulation/simulation.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::workload_simulation {

SimulatorParams simulatorGlobalParams;

std::string getSimulatorHelp(StringData name, const optionenvironment::OptionSection& options) {
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
