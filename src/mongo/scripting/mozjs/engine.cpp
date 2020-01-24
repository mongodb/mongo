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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/engine.h"

#include <js/Initialization.h>

#include "mongo/db/operation_context.h"
#include "mongo/scripting/mozjs/engine_gen.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/proxyscope.h"
#include "mongo/util/log.h"

namespace js {
void DisableExtraThreads();
}

namespace mongo {

void ScriptEngine::setup(bool disableLoadStored) {
    if (getGlobalScriptEngine())
        return;

    setGlobalScriptEngine(new mozjs::MozJSScriptEngine(disableLoadStored));

    if (hasGlobalServiceContext()) {
        getGlobalServiceContext()->registerKillOpListener(getGlobalScriptEngine());
    }
}

std::string ScriptEngine::getInterpreterVersionString() {
    return "MozJS-" BOOST_PP_STRINGIZE(MOZJS_MAJOR_VERSION);
}

namespace mozjs {

MozJSScriptEngine::MozJSScriptEngine(bool disableLoadStored) : ScriptEngine(disableLoadStored) {
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
    OpIdToScopeMap::iterator iScope = _opToScopeMap.find(opId);
    if (iScope == _opToScopeMap.end()) {
        // got interrupt request for a scope that no longer exists
        LOG(1) << "received interrupt request for unknown op: " << opId << printKnownOps_inlock();
        return;
    }

    LOG(1) << "interrupting op: " << opId << printKnownOps_inlock();
    iScope->second->kill();
}

std::string MozJSScriptEngine::printKnownOps_inlock() {
    str::stream out;

    if (shouldLog(logger::LogSeverity::Debug(2))) {
        out << "  known ops: \n";

        for (auto&& iSc : _opToScopeMap) {
            out << "  " << iSc.first << "\n";
        }
    }

    return out;
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

void MozJSScriptEngine::registerOperation(OperationContext* opCtx, MozJSImplScope* scope) {
    stdx::lock_guard<Latch> giLock(_globalInterruptLock);

    auto opId = opCtx->getOpID();

    _opToScopeMap[opId] = scope;

    LOG(2) << "SMScope " << reinterpret_cast<uint64_t>(scope) << " registered for op " << opId;
    Status status = opCtx->checkForInterruptNoAssert();
    if (!status.isOK()) {
        scope->kill();
    }
}

void MozJSScriptEngine::unregisterOperation(unsigned int opId) {
    stdx::lock_guard<Latch> giLock(_globalInterruptLock);

    LOG(2) << "ImplScope " << reinterpret_cast<uint64_t>(this) << " unregistered for op " << opId;

    if (opId != 0) {
        // scope is currently associated with an operation id
        auto it = _opToScopeMap.find(opId);
        if (it != _opToScopeMap.end())
            _opToScopeMap.erase(it);
    }
}

}  // namespace mozjs
}  // namespace mongo
