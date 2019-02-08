
/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/server_options_base.h"

#include "mongo/base/string_data.h"
#include "mongo/db/server_options_base_gen.h"
#include "mongo/logger/log_component.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace moe = mongo::optionenvironment;

namespace mongo {

// Primarily dispatches to IDL defined addBaseServerOptionDefinitions,
// then adds some complex options inexpressible in IDL.
Status addBaseServerOptions(moe::OptionSection* options) {
    auto status = addBaseServerOptionDefinitions(options);
    if (!status.isOK()) {
        return status;
    }

    moe::OptionSection general_options("General options");

    // log component hierarchy verbosity levels
    for (int i = 0; i < int(logger::LogComponent::kNumLogComponents); ++i) {
        logger::LogComponent component = static_cast<logger::LogComponent::Value>(i);
        if (component == logger::LogComponent::kDefault) {
            continue;
        }
        general_options
            .addOptionChaining("systemLog.component." + component.getDottedName() + ".verbosity",
                               "",
                               moe::Int,
                               "set component verbose level for " + component.getDottedName())
            .setSources(moe::SourceYAMLConfig);
    }

    /* support for -vv -vvvv etc. */
    for (std::string s = "vv"; s.length() <= 12; s.append("v")) {
        general_options.addOptionChaining(s.c_str(), s.c_str(), moe::Switch, "verbose")
            .hidden()
            .setSources(moe::SourceAllLegacy);
    }

    return options->addSection(general_options);
}

Status validateSystemLogDestinationSetting(const std::string& value) {
    constexpr auto kSyslog = "syslog"_sd;
    constexpr auto kFile = "file"_sd;

    if (!kSyslog.equalCaseInsensitive(value) && !kFile.equalCaseInsensitive(value)) {
        return {ErrorCodes::BadValue, "systemLog.destination expects one of 'syslog' or 'file'"};
    }

    return Status::OK();
}

}  // namespace mongo
