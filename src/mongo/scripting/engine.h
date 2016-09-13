// engine.h

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {
typedef unsigned long long ScriptingFunction;
typedef BSONObj (*NativeFunction)(const BSONObj& args, void* data);
typedef std::map<std::string, ScriptingFunction> FunctionCacheMap;

class DBClientWithCommands;
class DBClientBase;
class OperationContext;

struct JSFile {
    const char* name;
    const StringData& source;
};

class Scope {
    MONGO_DISALLOW_COPYING(Scope);

public:
    Scope();
    virtual ~Scope();

    virtual void reset() = 0;
    virtual void init(const BSONObj* data) = 0;
    virtual void registerOperation(OperationContext* txn) = 0;
    virtual void unregisterOperation() = 0;

    void init(const char* data) {
        BSONObj o(data);
        init(&o);
    }

    virtual void localConnectForDbEval(OperationContext* txn, const char* dbName) = 0;
    virtual void externalSetup() = 0;
    virtual void setLocalDB(const std::string& localDBName) {
        _localDBName = localDBName;
    }

    virtual BSONObj getObject(const char* field) = 0;
    virtual std::string getString(const char* field) = 0;
    virtual bool getBoolean(const char* field) = 0;
    virtual double getNumber(const char* field) = 0;
    virtual int getNumberInt(const char* field) = 0;

    virtual long long getNumberLongLong(const char* field) = 0;

    virtual Decimal128 getNumberDecimal(const char* field) = 0;

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

    virtual bool hasOutOfMemoryException() = 0;

    virtual bool isKillPending() const = 0;

    virtual void gc() = 0;

    virtual void advanceGeneration() = 0;

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

    virtual void injectNative(const char* field, NativeFunction func, void* data = 0) = 0;

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

    virtual void loadStored(OperationContext* txn, bool ignoreNotConnected = false);

    /**
     * if any changes are made to .system.js, call this
     * right now its just global - slightly inefficient, but a lot simpler
     */
    static void storedFuncMod(OperationContext* txn);

    static void validateObjectIdString(const std::string& str);

    /** increments the number of times a scope was used */
    void incTimesUsed() {
        ++_numTimesUsed;
    }

    /** gets the number of times a scope was used */
    int getTimesUsed() const {
        return _numTimesUsed;
    }

    /** return true if last invoke() return'd native code */
    virtual bool isLastRetNativeCode() {
        return _lastRetIsNativeCode;
    }

    class NoDBAccess {
        Scope* _s;

    public:
        NoDBAccess(Scope* s) : _s(s) {}
        ~NoDBAccess() {
            _s->rename("____db____", "db");
        }
    };
    NoDBAccess disableDBAccess(const char* why) {
        rename("db", "____db____");
        return NoDBAccess(this);
    }

protected:
    friend class PooledScope;

    /**
     * RecoveryUnit::Change subclass used to commit work for
     * Scope::storedFuncMod logOp listener.
     */
    class StoredFuncModLogOpHandler;

    virtual FunctionCacheMap& getFunctionCache() {
        return _cachedFunctions;
    }
    virtual ScriptingFunction _createFunction(const char* code,
                                              ScriptingFunction functionNumber = 0) = 0;

    std::string _localDBName;
    int64_t _loadedVersion;
    std::set<std::string> _storedNames;
    static AtomicInt64 _lastVersion;
    FunctionCacheMap _cachedFunctions;
    int _numTimesUsed;
    bool _lastRetIsNativeCode;  // v8 only: set to true if eval'd script returns a native func
};

class ScriptEngine : public KillOpListenerInterface {
    MONGO_DISALLOW_COPYING(ScriptEngine);

public:
    ScriptEngine();
    virtual ~ScriptEngine();

    virtual Scope* newScope() {
        return createScope();
    }

    virtual Scope* newScopeForCurrentThread() {
        return createScopeForCurrentThread();
    }

    virtual void runTest() = 0;

    virtual bool utf8Ok() const = 0;

    virtual void enableJIT(bool value) = 0;
    virtual bool isJITEnabled() const = 0;

    virtual void enableJavaScriptProtection(bool value) = 0;
    virtual bool isJavaScriptProtectionEnabled() const = 0;

    static void setup();
    static void dropScopeCache();

    /** gets a scope from the pool or a new one if pool is empty
     * @param db The db name
     * @param scopeType A unique id to limit scope sharing.
     *                  This must include authenticated users.
     * @return the scope
     */
    std::unique_ptr<Scope> getPooledScope(OperationContext* txn,
                                          const std::string& db,
                                          const std::string& scopeType);

    void setScopeInitCallback(void (*func)(Scope&)) {
        _scopeInitCallback = func;
    }
    static void setConnectCallback(void (*func)(DBClientWithCommands&)) {
        _connectCallback = func;
    }
    static void runConnectCallback(DBClientWithCommands& c) {
        if (_connectCallback)
            _connectCallback(c);
    }

    // engine implementation may either respond to interrupt events or
    // poll for interrupts.  the interrupt functions must not wait indefinitely on a lock.
    virtual void interrupt(unsigned opId) {}
    virtual void interruptAll() {}

    static std::string getInterpreterVersionString();

protected:
    virtual Scope* createScope() = 0;
    virtual Scope* createScopeForCurrentThread() = 0;
    void (*_scopeInitCallback)(Scope&);

private:
    static void (*_connectCallback)(DBClientWithCommands&);
};

void installGlobalUtils(Scope& scope);
bool hasJSReturn(const std::string& s);
const char* jsSkipWhiteSpace(const char* raw);

extern ScriptEngine* globalScriptEngine;
}
