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
