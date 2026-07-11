// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_compiled_configuration.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"

namespace mongo {

CompiledConfiguration::CompiledConfiguration(const char* apiName, const char* config)
    : _apiName(apiName), _config(config) {
    _compileToken = CompiledConfigurationRegistry::instance()->add(apiName, config);
}

const char* CompiledConfiguration::getConfig(WiredTigerSession* session) const {
    CompiledConfigurationsPerConnection* compiled =
        session->getCompiledConfigurationsPerConnection();
    return (compiled->get(_compileToken));
}

const std::string& CompiledConfiguration::getRawConfig() const {
    return _config;
}

// -----------------------


Status CompiledConfigurationsPerConnection::compileAll(WT_CONNECTION* conn) {
    auto registry = CompiledConfigurationRegistry::instance();
    _compileResults.clear();
    for (int i = 0; i < registry->size(); ++i) {
        auto spec = registry->get(i);
        const char* compiled;
        int ret =
            conn->compile_configuration(conn, spec.apiName.c_str(), spec.config.c_str(), &compiled);
        if (ret != 0) {
            return wtRCToStatus(ret, nullptr, [&]() {
                return fmt::format(
                    "CompiledConfigurationsPerConnection::compileAll: apiName: {}; config: {}",
                    spec.apiName,
                    spec.config);
            });
        }
        _compileResults.push_back(compiled);
    }
    return Status::OK();
}

// -----------------------

int CompiledConfigurationRegistry::add(const char* apiName, const char* config) {
    int compileToken = _configs.size();
    _configs.emplace_back(apiName, config);
    return compileToken;
}

}  // namespace mongo
