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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/engine.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string>

#include <vm/PosixNSPR.h>

namespace mongo {
namespace mozjs {

class MozJSImplScope;

/**
 * Proxies all methods on the implementation scope over a side channel that
 * allows the SpiderMonkey runtime to operate entirely on one thread. This
 * implements the public scope interface and is the right way to talk to an
 * implementation scope from multiple threads.
 *
 * In terms of implementation, it does most of it's heavy lifting through a
 * unique_function. The proxy scope owns an implementation scope transitively
 * through the thread it owns. They communicate by setting a variable, then
 * signaling each other. That communication has to work, there's no fallback
 * for timing out.
 *
 * There are probably performance gains to be had from making
 * the argument capture and method dispatch explicit, but I'll wait until we've
 * measured it before bothering.
 *
 * See mongo::Scope for details on all of the overridden functions
 *
 */
class MozJSProxyScope final : public Scope {
    MozJSProxyScope(const MozJSProxyScope&) = delete;
    MozJSProxyScope& operator=(const MozJSProxyScope&) = delete;

    /**
     * The FSM is fairly tight:
     *
     * +----------+  shutdownThread()   +--------------------+
     * | Shutdown | <------------------ |        Idle        | <+
     * +----------+                     +--------------------+  |
     *                                    |                     |
     *                                    | runOnImplThread()   |
     *                                    v                     |
     *                                  +--------------------+  |
     *                                  |    ProxyRequest    |  | impl -> proxy
     *                                  +--------------------+  |
     *                                    |                     |
     *                                    | proxy -> impl       |
     *                                    v                     |
     *                                  +--------------------+  |
     *                                  |    ImplResponse    | -+
     *                                  +--------------------+
     *
     * The regular flow:
     * - We start at Idle and on the proxy thread.
     * - runOnImplThread sets ProxyRequest and notifies the impl thread
     * - The impl thread wakes up, invokes _function(), sets ImplResponse and notifies the proxy
     *   thread
     * - The proxy thread wakes up and sets Idle
     *
     * Shutdown:
     * - On destruction, The proxy thread sets Shutdown and notifies the impl thread
     * - The impl thread wakes up, breaks out of it's loop and returns
     * - The proxy thread joins the impl thread
     *
     */
    enum class State : char {
        Idle,
        ProxyRequest,
        ImplResponse,
        Shutdown,
    };

public:
    MozJSProxyScope(MozJSScriptEngine* engine);
    ~MozJSProxyScope() override;

    void init(const BSONObj* data) override;

    void reset() override;

    /**
     * Thread safe. Kills the running operation
     */
    void kill() override;

    bool isKillPending() const override;

    void registerOperation(OperationContext* opCtx) override;

    void unregisterOperation() override;

    void externalSetup() override;

    std::string getError() override;

    std::string getBaseURL() const override;

    bool hasOutOfMemoryException() override;

    void gc() override;

    void advanceGeneration() override;

    void requireOwnedObjects() override;

    double getNumber(const char* field) override;
    int getNumberInt(const char* field) override;
    long long getNumberLongLong(const char* field) override;
    Decimal128 getNumberDecimal(const char* field) override;
    std::string getString(const char* field) override;
    bool getBoolean(const char* field) override;
    BSONObj getObject(const char* field) override;
    OID getOID(const char* field) override;
    // Note: The resulting BSONBinData is only valid within the scope of the 'withBinData' callback.
    void getBinData(const char* field,
                    std::function<void(const BSONBinData&)> withBinData) override;
    Timestamp getTimestamp(const char* field) override;
    JSRegEx getRegEx(const char* field) override;

    void setNumber(const char* field, double val) override;
    void setString(const char* field, StringData val) override;
    void setBoolean(const char* field, bool val) override;
    void setElement(const char* field, const BSONElement& e, const BSONObj& parent) override;
    void setObject(const char* field, const BSONObj& obj, bool readOnly) override;
    void setFunction(const char* field, const char* code) override;

    int type(const char* field) override;

    void rename(const char* from, const char* to) override;

    int invoke(ScriptingFunction func,
               const BSONObj* args,
               const BSONObj* recv,
               int timeoutMs = 0,
               bool ignoreReturn = false,
               bool readOnlyArgs = false,
               bool readOnlyRecv = false) override;

    bool exec(StringData code,
              const std::string& name,
              bool printResult,
              bool reportError,
              bool assertOnError,
              int timeoutMs) override;

    void injectNative(const char* field, NativeFunction func, void* data = nullptr) override;

    ScriptingFunction _createFunction(const char* code) override;

    void interrupt();

private:
    template <typename Closure>
    void run(Closure&& closure);

    template <typename Closure>
    void runWithoutInterruptionExceptAtGlobalShutdown(Closure&& closure);

    void runOnImplThread(unique_function<void()> f);

    void shutdownThread();
    static void implThread(MozJSProxyScope* proxy);

    MozJSScriptEngine* const _engine;
    MozJSImplScope* _implScope;

    /**
     * This mutex protects _function, _state and _status as channels for
     * function invocation and exception handling
     */
    stdx::mutex _mutex;
    unique_function<void()> _function;
    State _state;
    Status _status;
    OperationContext* _opCtx = nullptr;

    stdx::condition_variable _proxyCondvar;
    stdx::condition_variable _implCondvar;
    stdx::thread _thread;
};

}  // namespace mozjs
}  // namespace mongo
