// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <wiredtiger.h>

namespace mongo {

class WiredTigerSession;

/**
 * A single compiled configuration.  An instance of this class can be declared statically
 * near the point of use.
 */
class CompiledConfiguration {
public:
    CompiledConfiguration(const char* apiName, const char* config);

    const char* getConfig(WiredTigerSession* session) const;

    const std::string& getRawConfig() const;

private:
    std::string _apiName;
    std::string _config;
    int _compileToken;
};

/**
 * All compiled configurations used for a single connection.
 */
class CompiledConfigurationsPerConnection {
public:
    CompiledConfigurationsPerConnection() {}

    Status compileAll(WT_CONNECTION* conn);
    const char* get(int compileToken) const {
        return _compileResults[compileToken];
    }

private:
    // Cannot be std::string, as it is the address returned by the compile call that is important,
    // any copying of the C string would not work.
    std::vector<const char*> _compileResults;
};

/**
 * Registry of configuration strings that are to be compiled.
 */
class CompiledConfigurationRegistry {
public:
    struct CompileSpec {
        std::string apiName;
        std::string config;
        CompileSpec(const char* _apiName, const char* _config)
            : apiName(_apiName), config(_config) {}
    };

    CompiledConfigurationRegistry() {}

    int add(const char* apiName, const char* config);

    int size() const {
        return _configs.size();
    }

    CompileSpec get(int i) const {
        return _configs[i];
    }

    static CompiledConfigurationRegistry* instance() {
        static CompiledConfigurationRegistry _instance;
        return &_instance;
    }

private:
    std::vector<CompileSpec> _configs;
};

}  // namespace mongo
