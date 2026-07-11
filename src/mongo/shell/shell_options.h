// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

/**
 * The sole purpose of this class is to avoid compilation errors with the definition of 'nokillop'
 * field in shell_options.idl which the generated option parser requires assignment operator for the
 * field to work but Atomic<T> does not support assignment operator(s).
 */
class AssignableAtomicBool {
public:
    AssignableAtomicBool() = default;
    explicit AssignableAtomicBool(bool value) : _value(value) {}

    AssignableAtomicBool& operator=(bool value) {
        store(value);
        return *this;
    }

    bool load() const {
        return _value.load();
    }
    void store(bool value) {
        _value.store(value);
    }

private:
    Atomic<bool> _value;
};

struct ShellGlobalParams {
    std::string url;
    std::string dbhost;
    std::string port;
    std::vector<std::string> files;

    std::string username;
    std::string password;
    std::string authenticationMechanism;
    std::string authenticationDatabase;
    std::string gssapiServiceName;
    std::string gssapiHostName;
    std::string networkMessageCompressors;

    bool runShell;
    bool nodb;
    bool norc;
    bool nojit = true;
    bool javascriptProtection = true;
    bool enableIPv6 = false;

    std::string script;

    std::string apiVersion;
    bool apiStrict;
    bool apiDeprecationErrors;

    bool autoKillOp = false;

    bool shouldRetryWrites = false;
    bool shouldUseImplicitSessions = true;

    bool jsDebugMode = false;

    int jsHeapLimitMB = 0;
    AssignableAtomicBool nokillop{false};
    Seconds idleSessionTimeout = Seconds{0};

#ifdef MONGO_CONFIG_GRPC
    bool gRPC = false;
    boost::optional<std::string> gRPCAuthToken;
#endif
};

extern ShellGlobalParams shellGlobalParams;

std::string getMongoShellHelp(std::string_view name, const moe::OptionSection& options);

/**
 * Handle options that should come before validation, such as "help".
 *
 * Returns false if an option was found that implies we should prematurely exit with success.
 */
bool handlePreValidationMongoShellOptions(const moe::Environment& params,
                                          const std::vector<std::string>& args);

Status storeMongoShellOptions(const moe::Environment& params, const std::vector<std::string>& args);

void redactPasswordOptions(int argc, char** argv);

std::string getApiParametersJSON();
}  // namespace mongo
