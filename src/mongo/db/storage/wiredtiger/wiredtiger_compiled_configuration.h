/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/status.h"

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
