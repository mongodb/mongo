// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/engine.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/scripting/config_engine_gen.h"
#include "mongo/scripting/config_gen.h"
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/scripting/mozjs/shell/proxyscope.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <utility>
#include <vector>

#include <js-config.h>

#include <absl/meta/type_traits.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <js/Initialization.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace js {
void DisableExtraThreads();
}

namespace mongo {

namespace {
auto operationMozJSShellRuntimeInterfaceDecoration =
    OperationContext::declareDecoration<mozjs::MozJSImplScope*>();
}  // namespace

void ScriptEngine::setup(ExecutionEnvironment environment) {
    if (getGlobalScriptEngine()) {
        return;
    }

    if (!serverGlobalParams.quiet.load()) {
        LOGV2_INFO(8972602, "Setting up MozJS engine.");
    }

    setGlobalScriptEngine(new mozjs::MozJSScriptEngine(environment));

    if (hasGlobalServiceContext()) {
        registerScriptEngineKillOpProxy(getGlobalServiceContext());
    }
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
    if (opCtx && (*opCtx)[operationMozJSShellRuntimeInterfaceDecoration]) {
        (*opCtx)[operationMozJSShellRuntimeInterfaceDecoration]->kill();
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
        std::lock_guard lk(*client);
        if (auto opCtx = client->getOperationContext();
            opCtx && (*opCtx)[operationMozJSShellRuntimeInterfaceDecoration]) {
            (*opCtx)[operationMozJSShellRuntimeInterfaceDecoration]->kill();
        }
    }
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

bool MozJSScriptEngine::getJSUseLegacyMemoryTracking() const {
    return gJSUseLegacyMemoryTracking.load();
}

void MozJSScriptEngine::setJSUseLegacyMemoryTracking(bool shouldUseLegacy) {
    gJSUseLegacyMemoryTracking.store(shouldUseLegacy);
}

std::string MozJSScriptEngine::getLoadPath() const {
    return _loadPath;
}

void MozJSScriptEngine::setLoadPath(const std::string& loadPath) {
    _loadPath = loadPath;
}

std::string MozJSScriptEngine::getInterpreterVersionString() const {
    return fmt::format("MozJS-{}", MOZJS_MAJOR_VERSION);
}

void MozJSScriptEngine::registerOperation(OperationContext* opCtx, MozJSImplScope* scope) {
    LOGV2_DEBUG(22785,
                2,
                "scope registered for op",
                "scope"_attr = reinterpret_cast<uint64_t>(scope),
                "opId"_attr = opCtx->getOpID());

    std::lock_guard lk(*opCtx->getClient());
    (*opCtx)[operationMozJSShellRuntimeInterfaceDecoration] = scope;

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

    std::lock_guard lk(*opCtx->getClient());
    (*opCtx)[operationMozJSShellRuntimeInterfaceDecoration] = nullptr;
}

}  // namespace mozjs
}  // namespace mongo
