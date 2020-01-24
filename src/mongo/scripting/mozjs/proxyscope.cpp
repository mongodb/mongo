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

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/proxyscope.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/functional.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace mozjs {

MozJSProxyScope::MozJSProxyScope(MozJSScriptEngine* engine)
    : _engine(engine),
      _implScope(nullptr),
      _mutex(),
      _state(State::Idle),
      _status(Status::OK()),
      _thread(implThread, this) {
    // Test the child on startup to make sure it's awake and that the
    // implementation scope sucessfully constructed.
    try {
        run([] {});
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
    unregisterOperation();
    runWithoutInterruptionExceptAtGlobalShutdown([&] { _implScope->reset(); });
}

bool MozJSProxyScope::isKillPending() const {
    return _implScope->isKillPending();
}

void MozJSProxyScope::registerOperation(OperationContext* opCtx) {
    _opCtx = opCtx;
}

void MozJSProxyScope::unregisterOperation() {
    _opCtx = nullptr;
}

void MozJSProxyScope::externalSetup() {
    run([&] { _implScope->externalSetup(); });
}

std::string MozJSProxyScope::getError() {
    std::string out;
    runWithoutInterruptionExceptAtGlobalShutdown([&] { out = _implScope->getError(); });
    return out;
}

bool MozJSProxyScope::hasOutOfMemoryException() {
    bool out;
    runWithoutInterruptionExceptAtGlobalShutdown(
        [&] { out = _implScope->hasOutOfMemoryException(); });
    return out;
}

void MozJSProxyScope::gc() {
    _implScope->gc();
}

void MozJSProxyScope::advanceGeneration() {
    runWithoutInterruptionExceptAtGlobalShutdown([&] { _implScope->advanceGeneration(); });
}

void MozJSProxyScope::requireOwnedObjects() {
    runWithoutInterruptionExceptAtGlobalShutdown([&] { _implScope->requireOwnedObjects(); });
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

ScriptingFunction MozJSProxyScope::_createFunction(const char* raw) {
    ScriptingFunction out;
    run([&] { out = _implScope->_createFunction(raw); });
    return out;
}

void MozJSProxyScope::kill() {
    _implScope->kill();
}

void MozJSProxyScope::interrupt() {
    _implScope->interrupt();
}

/**
 * Invokes a function on the implementation thread
 *
 * It does this by serializing the invocation through a unique_function and
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

    if (_thread.get_id() == stdx::this_thread::get_id()) {
        return closure();
    }

    runOnImplThread(std::move(closure));
}

template <typename Closure>
void MozJSProxyScope::runWithoutInterruptionExceptAtGlobalShutdown(Closure&& closure) {
    auto toRun = [&] { run(std::forward<Closure>(closure)); };

    if (_opCtx) {
        return _opCtx->runWithoutInterruptionExceptAtGlobalShutdown(toRun);
    } else {
        return toRun();
    }
}

void MozJSProxyScope::runOnImplThread(unique_function<void()> f) {
    stdx::unique_lock<Latch> lk(_mutex);
    _function = std::move(f);

    invariant(_state == State::Idle);
    _state = State::ProxyRequest;

    lk.unlock();
    _implCondvar.notify_one();
    lk.lock();

    Interruptible* interruptible = _opCtx ? _opCtx : Interruptible::notInterruptible();

    auto pred = [&] { return _state == State::ImplResponse; };

    try {
        interruptible->waitForConditionOrInterrupt(_proxyCondvar, lk, pred);
    } catch (const DBException& ex) {
        _implScope->kill();
        _proxyCondvar.wait(lk, pred);

        // update _status after the wait, otherwise it would get overwritten in implThread
        _status = ex.toStatus();
    }

    _state = State::Idle;

    // Clear the _status state and throw it if necessary
    auto status = std::move(_status);

    // Can validate the status outside the lock
    lk.unlock();

    uassertStatusOK(status);
}

void MozJSProxyScope::shutdownThread() {
    {
        stdx::lock_guard<Latch> lk(_mutex);

        invariant(_state == State::Idle);

        _state = State::Shutdown;
    }

    _implCondvar.notify_one();

    _thread.join();
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
void MozJSProxyScope::implThread(MozJSProxyScope* proxy) {
    if (hasGlobalServiceContext())
        Client::initThread("js");

    std::unique_ptr<MozJSImplScope> scope;

    // This will leave _status set for the first noop runOnImplThread(), which
    // captures the startup exception that way
    try {
        scope.reset(new MozJSImplScope(proxy->_engine,
                                       boost::none /* Don't override global jsHeapLimitMB */));
        proxy->_implScope = scope.get();
    } catch (...) {
        proxy->_status = exceptionToStatus();
    }

    // This is mostly to silence coverity, so that it sees that the
    // ProxyScope doesn't hold a reference to the ImplScope after it
    // is deleted by the unique_ptr.
    const auto unbindImplScope = makeGuard([&proxy] { proxy->_implScope = nullptr; });

    while (true) {
        stdx::unique_lock<Latch> lk(proxy->_mutex);
        {
            MONGO_IDLE_THREAD_BLOCK;
            proxy->_implCondvar.wait(lk, [proxy] {
                return proxy->_state == State::ProxyRequest || proxy->_state == State::Shutdown;
            });
        }

        if (proxy->_state == State::Shutdown)
            break;

        try {
            lk.unlock();
            const auto unlockGuard = makeGuard([&] { lk.lock(); });
            proxy->_function();
        } catch (...) {
            proxy->_status = exceptionToStatus();
        }

        proxy->_state = State::ImplResponse;

        lk.unlock();
        proxy->_proxyCondvar.notify_one();
    }
}

}  // namespace mozjs
}  // namespace mongo
