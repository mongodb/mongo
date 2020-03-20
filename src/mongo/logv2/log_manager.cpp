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

#include "mongo/platform/basic.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/logv2/log_manager.h"

#include <boost/log/core.hpp>

#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_global.h"

namespace mongo::logv2 {

struct LogManager::Impl {
    Impl() {}

    LogDomain _globalDomain{std::make_unique<LogDomainGlobal>()};
};


LogManager::LogManager() {
    _impl = std::make_unique<Impl>();

    boost::log::core::get()->set_exception_handler([]() {
        thread_local uint32_t depth = 0;
        auto depthGuard = makeGuard([]() { --depth; });
        ++depth;
        // Try and log that we failed to log
        if (depth == 1) {
            std::exception_ptr ex = nullptr;
            try {
                throw;
            } catch (const DBException&) {
                ex = std::current_exception();
            } catch (...) {
                LOGV2(4638200,
                      "Exception during log, message not written to stream",
                      "exception"_attr = exceptionToStatus());
            }
            if (ex)
                std::rethrow_exception(ex);
        }

        // Logging exceptions are fatal in debug builds. Guard ourselves from additional logging
        // during the assert that might also fail
        if (kDebugBuild && depth <= 2) {
            dassert(false, "Exception during log");
        }
    });
}

LogManager::~LogManager() {}

LogManager& LogManager::global() {
    static LogManager* globalLogManager = new LogManager();
    return *globalLogManager;
}

LogDomain& LogManager::getGlobalDomain() {
    return _impl->_globalDomain;
}

LogDomainGlobal& LogManager::getGlobalDomainInternal() {
    return static_cast<LogDomainGlobal&>(_impl->_globalDomain.internal());
}

LogComponentSettings& LogManager::getGlobalSettings() {
    return getGlobalDomainInternal().settings();
}

}  // namespace mongo::logv2
