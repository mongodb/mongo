/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <js-config.h>
#include <js/Initialization.h>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/scripting/mozjs/engine.h"
#include "mongo/scripting/mozjs/engine_gen.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/proxyscope.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace js {
void DisableExtraThreads();
}

namespace mongo {

void ScriptEngine::setup(ExecutionEnvironment environment) {
    if (getGlobalScriptEngine()) {
        return;
    }

    setGlobalScriptEngine(new mozjs::MozJSScriptEngine(environment));

    if (hasGlobalServiceContext()) {
        getGlobalServiceContext()->registerKillOpListener(getGlobalScriptEngine());
    }
}

std::string ScriptEngine::getInterpreterVersionString() {
    using namespace fmt::literals;
    return "MozJS-{}"_format(MOZJS_MAJOR_VERSION);
}

namespace mozjs {

MozJSScriptEngine::MozJSScriptEngine(ExecutionEnvironment environment)
    : _executionEnvironment(environment), _loadPath(boost::filesystem::current_path().string()) {
    uassert(ErrorCodes::JSInterpreterFailure, "Failed to JS_Init()", JS_Init());
    js::DisableExtraThreads();
}

MozJSScriptEngine::~MozJSScriptEngine() {
    JS_ShutDown();
}

mongo::Scope* MozJSScriptEngine::createScope() {
    return new MozJSProxyScope(this);
}

mongo::Scope* MozJSScriptEngine::createScopeForCurrentThread(boost::optional<int> jsHeapLimitMB) {
    return new MozJSImplScope(this, jsHeapLimitMB);
}

void MozJSScriptEngine::interrupt(unsigned opId) {
    stdx::lock_guard<Latch> intLock(_globalInterruptLock);
    auto knownOps = [&]() {
        std::vector<unsigned> ret;
        for (auto&& iSc : _opToScopeMap) {
            ret.push_back(iSc.first);
        }
        return ret;
    };
    OpIdToScopeMap::iterator iScope = _opToScopeMap.find(opId);
    if (iScope == _opToScopeMap.end()) {
        // got interrupt request for a scope that no longer exists
        if (shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(3))) {
            // This log record gets extra attributes when the log severity is at Debug(3)
            // but we still log the record at log severity Debug(2). Simplify this if SERVER-48671
            // gets done
            LOGV2_DEBUG(22783,
                        2,
                        "Received interrupt request for unknown op",
                        "opId"_attr = opId,
                        "knownOps"_attr = knownOps());
        } else {
            LOGV2_DEBUG(22790, 2, "Received interrupt request for unknown op", "opId"_attr = opId);
        }
        return;
    }
    if (shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(3))) {
        // Like above, this log record gets extra attributes when the log severity is at Debug(3)
        LOGV2_DEBUG(22809, 2, "Interrupting op", "opId"_attr = opId, "knownOps"_attr = knownOps());
    } else {
        LOGV2_DEBUG(22808, 2, "Interrupting op", "opId"_attr = opId);
    }
    iScope->second->kill();
}

void MozJSScriptEngine::interruptAll() {
    stdx::lock_guard<Latch> interruptLock(_globalInterruptLock);

    for (auto&& iScope : _opToScopeMap) {
        iScope.second->kill();
    }
}

void MozJSScriptEngine::enableJIT(bool value) {
    gDisableJavaScriptJIT.store(!value);
}

bool MozJSScriptEngine::isJITEnabled() const {
    return !gDisableJavaScriptJIT.load();
}

void MozJSScriptEngine::enableJavaScriptProtection(bool value) {
    gJavascriptProtection.store(value);
}

bool MozJSScriptEngine::isJavaScriptProtectionEnabled() const {
    return gJavascriptProtection.load();
}

int MozJSScriptEngine::getJSHeapLimitMB() const {
    return gJSHeapLimitMB.load();
}

void MozJSScriptEngine::setJSHeapLimitMB(int limit) {
    gJSHeapLimitMB.store(limit);
}

std::string MozJSScriptEngine::getLoadPath() const {
    return _loadPath;
}

void MozJSScriptEngine::setLoadPath(const std::string& loadPath) {
    _loadPath = loadPath;
}

void MozJSScriptEngine::registerOperation(OperationContext* opCtx, MozJSImplScope* scope) {
    stdx::lock_guard<Latch> giLock(_globalInterruptLock);

    auto opId = opCtx->getOpID();

    _opToScopeMap[opId] = scope;

    LOGV2_DEBUG(22785,
                2,
                "scope registered for op",
                "scope"_attr = reinterpret_cast<uint64_t>(scope),
                "opId"_attr = opId);
    Status status = opCtx->checkForInterruptNoAssert();
    if (!status.isOK()) {
        scope->kill();
    }
}

void MozJSScriptEngine::unregisterOperation(unsigned int opId) {
    stdx::lock_guard<Latch> giLock(_globalInterruptLock);

    LOGV2_DEBUG(22786,
                2,
                "scope unregistered for op",
                "scope"_attr = reinterpret_cast<uint64_t>(this),
                "opId"_attr = opId);

    if (opId != 0) {
        // scope is currently associated with an operation id
        auto it = _opToScopeMap.find(opId);
        if (it != _opToScopeMap.end())
            _opToScopeMap.erase(it);
    }
}

}  // namespace mozjs
}  // namespace mongo
