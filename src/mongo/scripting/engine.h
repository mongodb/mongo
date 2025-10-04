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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
typedef unsigned long long ScriptingFunction;
typedef BSONObj (*NativeFunction)(const BSONObj& args, void* data);
typedef std::map<std::string, ScriptingFunction> FunctionCacheMap;

class DBClientBase;
class OperationContext;

struct MONGO_MOD_NEEDS_REPLACEMENT JSFile {
    const char* name;
    const StringData source;
};

struct JSRegEx {
    std::string pattern;
    std::string flags;

    JSRegEx() = default;
    JSRegEx(std::string pattern, std::string flags)
        : pattern(std::move(pattern)), flags(std::move(flags)) {}
};

class MONGO_MOD_PUB Scope {
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

public:
    Scope();
    virtual ~Scope();

    virtual void reset() = 0;
    virtual void init(const BSONObj* data) = 0;
    virtual void registerOperation(OperationContext* opCtx) = 0;
    virtual void unregisterOperation() = 0;

    void init(const char* data) {
        BSONObj o(data);
        init(&o);
    }

    virtual void externalSetup() = 0;
    virtual void setLocalDB(const DatabaseName& localDBName) {
        _localDBName = localDBName;
    }

    virtual BSONObj getObject(const char* field) = 0;
    virtual std::string getString(const char* field) = 0;
    virtual bool getBoolean(const char* field) = 0;
    virtual double getNumber(const char* field) = 0;
    virtual int getNumberInt(const char* field) = 0;
    virtual long long getNumberLongLong(const char* field) = 0;
    virtual Decimal128 getNumberDecimal(const char* field) = 0;
    virtual OID getOID(const char* field) = 0;
    // Note: The resulting BSONBinData is only valid within the scope of the 'withBinData' callback.
    virtual void getBinData(const char* field,
                            std::function<void(const BSONBinData&)> withBinData) = 0;
    virtual Timestamp getTimestamp(const char* field) = 0;
    virtual JSRegEx getRegEx(const char* field) = 0;

    virtual void setElement(const char* field, const BSONElement& e, const BSONObj& parent) = 0;
    virtual void setNumber(const char* field, double val) = 0;
    virtual void setString(const char* field, StringData val) = 0;
    virtual void setObject(const char* field, const BSONObj& obj, bool readOnly = true) = 0;
    virtual void setBoolean(const char* field, bool val) = 0;
    virtual void setFunction(const char* field, const char* code) = 0;

    virtual int type(const char* field) = 0;

    virtual void append(BSONObjBuilder& builder, const char* fieldName, const char* scopeName);

    virtual void rename(const char* from, const char* to) = 0;

    virtual std::string getError() = 0;

    virtual std::string getBaseURL() const = 0;

    virtual bool hasOutOfMemoryException() = 0;

    virtual void kill() = 0;

    virtual bool isKillPending() const = 0;

    virtual void gc() = 0;

    virtual void advanceGeneration() = 0;

    virtual void requireOwnedObjects() = 0;

    virtual ScriptingFunction createFunction(const char* code);

    /**
     * @return 0 on success
     */
    int invoke(const char* code, const BSONObj* args, const BSONObj* recv, int timeoutMs = 0);

    virtual int invoke(ScriptingFunction func,
                       const BSONObj* args,
                       const BSONObj* recv,
                       int timeoutMs = 0,
                       bool ignoreReturn = false,
                       bool readOnlyArgs = false,
                       bool readOnlyRecv = false) = 0;

    void invokeSafe(ScriptingFunction func,
                    const BSONObj* args,
                    const BSONObj* recv,
                    int timeoutMs = 0,
                    bool ignoreReturn = false,
                    bool readOnlyArgs = false,
                    bool readOnlyRecv = false) {
        int res = invoke(func, args, recv, timeoutMs, ignoreReturn, readOnlyArgs, readOnlyRecv);
        if (res == 0)
            return;
        uasserted(9004, std::string("invoke failed: ") + getError());
    }

    void invokeSafe(const char* code, const BSONObj* args, const BSONObj* recv, int timeoutMs = 0) {
        if (invoke(code, args, recv, timeoutMs) == 0)
            return;
        uasserted(9005, std::string("invoke failed: ") + getError());
    }

    virtual void injectNative(const char* field, NativeFunction func, void* data = nullptr) = 0;

    virtual bool exec(StringData code,
                      const std::string& name,
                      bool printResult,
                      bool reportError,
                      bool assertOnError,
                      int timeoutMs = 0) = 0;

    virtual void execSetup(StringData code, const std::string& name = "setup") {
        exec(code, name, false, true, true, 0);
    }

    void execSetup(const JSFile& file) {
        execSetup(file.source, file.name);
    }

    virtual bool execFile(const std::string& filename,
                          bool printResult,
                          bool reportError,
                          int timeoutMs = 0);

    void execCoreFiles();
    void execPrelude();

    virtual void loadStored(OperationContext* opCtx, bool ignoreNotConnected = false);

    /**
     * if any changes are made to .system.js, call this
     * right now its just global - slightly inefficient, but a lot simpler
     */
    static void storedFuncMod(OperationContext* opCtx);

    static void validateObjectIdString(const std::string& str);

    /** gets the time at which the scope was created */
    Date_t getCreateTime() const {
        return _createTime;
    }

    /** return true if last invoke() return'd native code */
    virtual bool isLastRetNativeCode() {
        return _lastRetIsNativeCode;
    }

protected:
    friend class PooledScope;

    virtual ScriptingFunction _createFunction(const char* code) = 0;

    DatabaseName _localDBName;
    int64_t _loadedVersion;
    std::set<std::string> _storedNames;
    static AtomicWord<long long> _lastVersion;
    FunctionCacheMap _cachedFunctions;
    Date_t _createTime;
    bool _lastRetIsNativeCode;  // v8 only: set to true if eval'd script returns a native func
};

enum class MONGO_MOD_PUB ExecutionEnvironment { Server, TestRunner };

class MONGO_MOD_PUB ScriptEngine : public KillOpListenerInterface {
    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

public:
    ScriptEngine();
    ~ScriptEngine() override = default;

    virtual Scope* newScope() {
        return createScope();
    }

    virtual Scope* newScopeForCurrentThread(boost::optional<int> jsHeapLimitMB) {
        return createScopeForCurrentThread(jsHeapLimitMB);
    }

    Scope* newScopeForCurrentThread() {
        return newScopeForCurrentThread(boost::none);
    }

    virtual void runTest() = 0;

    virtual bool utf8Ok() const = 0;

    virtual void enableJavaScriptProtection(bool value) = 0;
    virtual bool isJavaScriptProtectionEnabled() const = 0;

    virtual int getJSHeapLimitMB() const = 0;
    virtual void setJSHeapLimitMB(int limit) = 0;

    virtual std::string getLoadPath() const = 0;
    virtual void setLoadPath(const std::string& loadPath) = 0;

    /**
     * Calls the constructor for the Global ScriptEngine.
     */
    static void setup(ExecutionEnvironment environment);
    static void dropScopeCache();

    /** gets a scope from the pool or a new one if pool is empty
     * @param db The db name
     * @param scopeType A unique id to limit scope sharing.
     *                  This must include authenticated users.
     * @return the scope
     */
    std::unique_ptr<Scope> getPooledScope(OperationContext* opCtx,
                                          const DatabaseName& db,
                                          const std::string& scopeType);

    using ScopeCallback = void (*)(Scope&);
    ScopeCallback getScopeInitCallback() {
        return _scopeInitCallback;
    };
    void setScopeInitCallback(void (*func)(Scope&)) {
        _scopeInitCallback = func;
    }
    static void setConnectCallback(void (*func)(DBClientBase&, StringData)) {
        _connectCallback = func;
    }
    static void runConnectCallback(DBClientBase& c, StringData uri) {
        if (_connectCallback)
            _connectCallback(c, uri);
    }

    // engine implementation may either respond to interrupt events or
    // poll for interrupts.  the interrupt functions must not wait indefinitely on a lock.
    void interrupt(ClientLock&, OperationContext*) override {}
    void interruptAll(ServiceContextLock&) override {}

    static std::string getInterpreterVersionString();

protected:
    virtual Scope* createScope() = 0;
    virtual Scope* createScopeForCurrentThread(boost::optional<int> jsHeapLimitMB) = 0;
    void (*_scopeInitCallback)(Scope&);

private:
    static void (*_connectCallback)(DBClientBase&, StringData);
};

void installGlobalUtils(Scope& scope);
bool hasJSReturn(const std::string& s);
const char* jsSkipWhiteSpace(const char* raw);

MONGO_MOD_PUB ScriptEngine* getGlobalScriptEngine();
void setGlobalScriptEngine(ScriptEngine* impl);

}  // namespace mongo
