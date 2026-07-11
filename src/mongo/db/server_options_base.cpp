// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/server_options_base.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_base_gen.h"
#include "mongo/db/server_options_general_gen.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/str.h"

#include <ostream>
#include <string_view>

namespace moe = mongo::optionenvironment;

namespace mongo {
using namespace std::literals::string_view_literals;

// Primarily dispatches to IDL defined addBaseServerOptionDefinitions,
// then adds some complex options inexpressible in IDL.
Status addBaseServerOptions(moe::OptionSection* options) {
    auto status = addBaseServerOptionDefinitions(options);
    if (!status.isOK()) {
        return status;
    }

    moe::OptionSection general_options("General options");

    // log component hierarchy verbosity levels
    for (int i = 0; i < int(logv2::LogComponent::kNumLogComponents); ++i) {
        logv2::LogComponent component = static_cast<logv2::LogComponent::Value>(i);
        if (component == logv2::LogComponent::kDefault) {
            continue;
        }
        general_options
            .addOptionChaining("systemLog.component." + component.getDottedName() + ".verbosity",
                               "",
                               moe::Int,
                               "set component verbose level for " + component.getDottedName(),
                               {},
                               {})
            .setSources(moe::SourceYAMLConfig);
    }

    /* support for -vv -vvvv etc. */
    for (std::string s = "vv"; s.length() <= 12; s.append("v")) {
        general_options.addOptionChaining(s.c_str(), s.c_str(), moe::Switch, "verbose", {}, {})
            .hidden()
            .setSources(moe::SourceAllLegacy);
    }

    return options->addSection(general_options);
}

// Proxies call to IDL generated general option registrations,
// and implicitly includes base options as well.
Status addGeneralServerOptions(moe::OptionSection* options) {
    auto status = addGeneralServerOptionDefinitions(options);
    if (!status.isOK()) {
        return status;
    }

    return addBaseServerOptions(options);
}

Status validateSystemLogDestinationSetting(const std::string& value) {
    if (!(str::equalCaseInsensitive(value, "syslog"sv) ||
          str::equalCaseInsensitive(value, "file"sv))) {
        return {ErrorCodes::BadValue, "systemLog.destination expects one of 'syslog' or 'file'"};
    }

    return Status::OK();
}

Status validateSecurityClusterAuthModeSetting(const std::string& value) {
    if (!ClusterAuthMode::parse(value).isOK()) {
        return {ErrorCodes::BadValue,
                "security.clusterAuthMode expects one of 'keyFile', 'sendKeyFile', 'sendX509', or "
                "'X509'"};
    }

    return Status::OK();
}

Status canonicalizeNetBindIpAll(moe::Environment* env) {
    const bool all = (*env)["net.bindIpAll"].as<bool>();
    auto status = env->remove("net.bindIpAll");
    if (!status.isOK()) {
        return status;
    }
    return all ? env->set("net.bindIp", moe::Value("*")) : Status::OK();
}

std::string getUnixDomainSocketFilePermissionsHelpText() {
    std::stringstream ss;
    ss << "Permissions to set on UNIX domain socket file - "
       << "0" << std::oct << DEFAULT_UNIX_PERMS << " by default";
    return ss.str();
}

}  // namespace mongo
