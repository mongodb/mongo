/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/engine.h"

#include <js/Initialization.h>

#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/proxyscope.h"
#include "mongo/util/log.h"

namespace js {
void DisableExtraThreads();
}

namespace mongo {

namespace {

MONGO_EXPORT_SERVER_PARAMETER(disableJavaScriptJIT, bool, false);
MONGO_EXPORT_SERVER_PARAMETER(javascriptProtection, bool, false);

}  // namespace

void ScriptEngine::setup() {
    if (!globalScriptEngine) {
        globalScriptEngine = new mozjs::MozJSScriptEngine();

        if (hasGlobalServiceContext()) {
            getGlobalServiceContext()->registerKillOpListener(globalScriptEngine);
        }
    }
}

std::string ScriptEngine::getInterpreterVersionString() {
    return "MozJS-38";
}

namespace mozjs {

MozJSScriptEngine::MozJSScriptEngine() {
    uassert(ErrorCodes::JSInterpreterFailure, "Failed to JS_Init()", JS_Init());
    js::DisableExtraThreads();
}

MozJSScriptEngine::~MozJSScriptEngine() {
    JS_ShutDown();
}

mongo::Scope* MozJSScriptEngine::createScope() {
    return new MozJSProxyScope(this);
}

mongo::Scope* MozJSScriptEngine::createScopeForCurrentThread() {
    return new MozJSImplScope(this);
}

void MozJSScriptEngine::interrupt(unsigned opId) {
    stdx::lock_guard<stdx::mutex> intLock(_globalInterruptLock);
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
    stdx::lock_guard<stdx::mutex> interruptLock(_globalInterruptLock);

    for (auto&& iScope : _opToScopeMap) {
        iScope.second->kill();
    }
}

void MozJSScriptEngine::enableJIT(bool value) {
    disableJavaScriptJIT.store(!value);
}

bool MozJSScriptEngine::isJITEnabled() const {
    return !disableJavaScriptJIT.load();
}

void MozJSScriptEngine::enableJavaScriptProtection(bool value) {
    javascriptProtection.store(value);
}

bool MozJSScriptEngine::isJavaScriptProtectionEnabled() const {
    return javascriptProtection.load();
}

void MozJSScriptEngine::registerOperation(OperationContext* txn, MozJSImplScope* scope) {
    stdx::lock_guard<stdx::mutex> giLock(_globalInterruptLock);

    auto opId = txn->getOpID();

    _opToScopeMap[opId] = scope;

    LOG(2) << "SMScope " << static_cast<const void*>(scope) << " registered for op " << opId;
    Status status = txn->checkForInterruptNoAssert();
    if (!status.isOK()) {
        scope->kill();
    }
}

void MozJSScriptEngine::unregisterOperation(unsigned int opId) {
    stdx::lock_guard<stdx::mutex> giLock(_globalInterruptLock);

    LOG(2) << "ImplScope " << static_cast<const void*>(this) << " unregistered for op " << opId;

    if (opId != 0) {
        // scope is currently associated with an operation id
        auto it = _opToScopeMap.find(opId);
        if (it != _opToScopeMap.end())
            _opToScopeMap.erase(it);
    }
}

}  // namespace mozjs
}  // namespace mongo
