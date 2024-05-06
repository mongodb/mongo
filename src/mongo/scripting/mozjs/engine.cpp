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

#include <absl/meta/type_traits.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <js-config.h>
#include <js/Initialization.h>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/compiler.h"
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

namespace {
auto operationMozJSScopeDecoration = OperationContext::declareDecoration<mozjs::MozJSImplScope*>();
}

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

void MozJSScriptEngine::interrupt(ClientLock&, OperationContext* opCtx) {
    if (opCtx && (*opCtx)[operationMozJSScopeDecoration]) {
        (*opCtx)[operationMozJSScopeDecoration]->kill();
        LOGV2_DEBUG(22808, 2, "Interrupting op", "opId"_attr = opCtx->getOpID());
    } else if (opCtx) {
        LOGV2_DEBUG(
            22790, 2, "Received interrupt request for unknown op", "opId"_attr = opCtx->getOpID());
    } else {
        LOGV2_DEBUG(8972600, 2, "Received interrupt request for unknown op without opId");
    }
}

void MozJSScriptEngine::interruptAll(ServiceContextLock& svcCtxLock) {
    ServiceContext::LockedClientsCursor cursor(&*svcCtxLock);
    while (auto client = cursor.next()) {
        stdx::lock_guard lk(*client);
        if (auto opCtx = client->getOperationContext();
            opCtx && (*opCtx)[operationMozJSScopeDecoration]) {
            (*opCtx)[operationMozJSScopeDecoration]->kill();
        }
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
    LOGV2_DEBUG(22785,
                2,
                "scope registered for op",
                "scope"_attr = reinterpret_cast<uint64_t>(scope),
                "opId"_attr = opCtx->getOpID());

    stdx::lock_guard lk(*opCtx->getClient());
    (*opCtx)[operationMozJSScopeDecoration] = scope;

    if (auto status = opCtx->checkForInterruptNoAssert(); !status.isOK()) {
        scope->kill();
    }
}

void MozJSScriptEngine::unregisterOperation(OperationContext* opCtx) {
    LOGV2_DEBUG(22786,
                2,
                "scope unregistered for op",
                "scope"_attr = reinterpret_cast<uint64_t>(this),
                "opId"_attr = opCtx->getOpID());

    stdx::lock_guard lk(*opCtx->getClient());
    (*opCtx)[operationMozJSScopeDecoration] = nullptr;
}

}  // namespace mozjs
}  // namespace mongo
