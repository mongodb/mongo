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

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/proxyscope.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/quick_exit.h"

namespace mongo {
namespace mozjs {

MozJSProxyScope::MozJSProxyScope(MozJSScriptEngine* engine)
    : _engine(engine),
      _implScope(nullptr),
      _mutex(),
      _state(State::Idle),
      _status(Status::OK()),
      _condvar(),
      // Despite calling PR_CreateThread, we're actually using our own
      // implementation of PosixNSPR.cpp in this directory. So these threads
      // are actually hosted on top of stdx::threads and most of the flags
      // don't matter.
      _thread(PR_CreateThread(PR_USER_THREAD,
                              implThread,
                              this,
                              PR_PRIORITY_NORMAL,
                              PR_LOCAL_THREAD,
                              PR_JOINABLE_THREAD,
                              0)) {
    // Test the child on startup to make sure it's awake and that the
    // implementation scope sucessfully constructed.
    try {
        runOnImplThread([] {});
    } catch (...) {
        shutdownThread();
        throw;
    }
}

MozJSProxyScope::~MozJSProxyScope() {
    DESTRUCTOR_GUARD(kill(); shutdownThread(););
}

void MozJSProxyScope::init(const BSONObj* data) {
    run([&] { _implScope->init(data); });
}

void MozJSProxyScope::reset() {
    run([&] { _implScope->reset(); });
}

bool MozJSProxyScope::isKillPending() const {
    return _implScope->isKillPending();
}

void MozJSProxyScope::registerOperation(OperationContext* txn) {
    run([&] { _implScope->registerOperation(txn); });
}

void MozJSProxyScope::unregisterOperation() {
    run([&] { _implScope->unregisterOperation(); });
}

void MozJSProxyScope::localConnectForDbEval(OperationContext* txn, const char* dbName) {
    run([&] { _implScope->localConnectForDbEval(txn, dbName); });
}

void MozJSProxyScope::externalSetup() {
    run([&] { _implScope->externalSetup(); });
}

std::string MozJSProxyScope::getError() {
    std::string out;
    run([&] { out = _implScope->getError(); });
    return out;
}

bool MozJSProxyScope::hasOutOfMemoryException() {
    bool out;
    run([&] { out = _implScope->hasOutOfMemoryException(); });
    return out;
}

void MozJSProxyScope::gc() {
    _implScope->gc();
}

void MozJSProxyScope::advanceGeneration() {
    run([&] { _implScope->advanceGeneration(); });
}

double MozJSProxyScope::getNumber(const char* field) {
    double out;
    run([&] { out = _implScope->getNumber(field); });
    return out;
}

int MozJSProxyScope::getNumberInt(const char* field) {
    int out;
    run([&] { out = _implScope->getNumberInt(field); });
    return out;
}

long long MozJSProxyScope::getNumberLongLong(const char* field) {
    long long out;
    run([&] { out = _implScope->getNumberLongLong(field); });
    return out;
}

Decimal128 MozJSProxyScope::getNumberDecimal(const char* field) {
    Decimal128 out;
    run([&] { out = _implScope->getNumberDecimal(field); });
    return out;
}

std::string MozJSProxyScope::getString(const char* field) {
    std::string out;
    run([&] { out = _implScope->getString(field); });
    return out;
}

bool MozJSProxyScope::getBoolean(const char* field) {
    bool out;
    run([&] { out = _implScope->getBoolean(field); });
    return out;
}

BSONObj MozJSProxyScope::getObject(const char* field) {
    BSONObj out;
    run([&] { out = _implScope->getObject(field); });
    return out;
}

void MozJSProxyScope::setNumber(const char* field, double val) {
    run([&] { _implScope->setNumber(field, val); });
}

void MozJSProxyScope::setString(const char* field, StringData val) {
    run([&] { _implScope->setString(field, val); });
}

void MozJSProxyScope::setBoolean(const char* field, bool val) {
    run([&] { _implScope->setBoolean(field, val); });
}

void MozJSProxyScope::setElement(const char* field, const BSONElement& e, const BSONObj& parent) {
    run([&] { _implScope->setElement(field, e, parent); });
}

void MozJSProxyScope::setObject(const char* field, const BSONObj& obj, bool readOnly) {
    run([&] { _implScope->setObject(field, obj, readOnly); });
}

void MozJSProxyScope::setFunction(const char* field, const char* code) {
    run([&] { _implScope->setFunction(field, code); });
}

int MozJSProxyScope::type(const char* field) {
    int out;
    run([&] { out = _implScope->type(field); });
    return out;
}

void MozJSProxyScope::rename(const char* from, const char* to) {
    run([&] { _implScope->rename(from, to); });
}

int MozJSProxyScope::invoke(ScriptingFunction func,
                            const BSONObj* argsObject,
                            const BSONObj* recv,
                            int timeoutMs,
                            bool ignoreReturn,
                            bool readOnlyArgs,
                            bool readOnlyRecv) {
    int out;
    run([&] {
        out = _implScope->invoke(
            func, argsObject, recv, timeoutMs, ignoreReturn, readOnlyArgs, readOnlyRecv);
    });

    return out;
}

bool MozJSProxyScope::exec(StringData code,
                           const std::string& name,
                           bool printResult,
                           bool reportError,
                           bool assertOnError,
                           int timeoutMs) {
    bool out;
    run([&] {
        out = _implScope->exec(code, name, printResult, reportError, assertOnError, timeoutMs);
    });
    return out;
}

void MozJSProxyScope::injectNative(const char* field, NativeFunction func, void* data) {
    run([&] { _implScope->injectNative(field, func, data); });
}

ScriptingFunction MozJSProxyScope::_createFunction(const char* raw,
                                                   ScriptingFunction functionNumber) {
    ScriptingFunction out;
    run([&] { out = _implScope->_createFunction(raw, functionNumber); });
    return out;
}

OperationContext* MozJSProxyScope::getOpContext() const {
    return _implScope->getOpContext();
}

void MozJSProxyScope::kill() {
    _implScope->kill();
}

/**
 * Invokes a function on the implementation thread
 *
 * It does this by serializing the invocation through a stdx::function and
 * capturing any exceptions through _status.
 *
 * We transition:
 *
 * Idle -> ProxyRequest -> ImplResponse -> Idle
 */
template <typename Closure>
void MozJSProxyScope::run(Closure&& closure) {
    // We can end up calling functions on the proxy scope from the impl thread
    // when callbacks from javascript have a handle to the proxy scope and call
    // methods on it from there. If we're on the same thread, it's safe to
    // simply call back in, so let's do that.

    if (_thread == PR_GetCurrentThread()) {
        return closure();
    }

    runOnImplThread(std::move(closure));
}

void MozJSProxyScope::runOnImplThread(stdx::function<void()> f) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _function = std::move(f);

    invariant(_state == State::Idle);
    _state = State::ProxyRequest;

    _condvar.notify_one();

    _condvar.wait(lk, [this] { return _state == State::ImplResponse; });

    _state = State::Idle;

    // Clear the _status state and throw it if necessary
    auto status = std::move(_status);

    // Can validate the status outside the lock
    lk.unlock();

    uassertStatusOK(status);
}

void MozJSProxyScope::shutdownThread() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        invariant(_state == State::Idle);

        _state = State::Shutdown;
    }

    _condvar.notify_one();

    PR_JoinThread(_thread);
}

/**
 * The main loop for the implementation thread
 *
 * This owns the actual implementation scope (which needs to be created on this
 * child thread) and has essentially two transition paths:
 *
 * Standard: ProxyRequest -> ImplResponse
 *   Invoke _function. Serialize exceptions to _status.
 *
 * Shutdown: Shutdown -> _
 *   break out of the loop and return.
 */
void MozJSProxyScope::implThread(void* arg) {
    auto proxy = static_cast<MozJSProxyScope*>(arg);

    if (hasGlobalServiceContext())
        Client::initThread("js");

    std::unique_ptr<MozJSImplScope> scope;

    // This will leave _status set for the first noop runOnImplThread(), which
    // captures the startup exception that way
    try {
        scope.reset(new MozJSImplScope(proxy->_engine));
        proxy->_implScope = scope.get();
    } catch (...) {
        proxy->_status = exceptionToStatus();
    }

    while (true) {
        stdx::unique_lock<stdx::mutex> lk(proxy->_mutex);
        proxy->_condvar.wait(lk, [proxy] {
            return proxy->_state == State::ProxyRequest || proxy->_state == State::Shutdown;
        });

        if (proxy->_state == State::Shutdown)
            break;

        try {
            proxy->_function();
        } catch (...) {
            proxy->_status = exceptionToStatus();
        }

        int exitCode;
        if (proxy->_implScope && proxy->_implScope->getQuickExit(&exitCode)) {
            scope.reset();
            quickExit(exitCode);
        }

        proxy->_state = State::ImplResponse;

        proxy->_condvar.notify_one();
    }
}

}  // namespace mozjs
}  // namespace mongo
