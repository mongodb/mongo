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

#include <jsapi.h>
#include <vm/PosixNSPR.h>


#include "mongo/client/dbclient_cursor.h"
#include "mongo/scripting/mozjs/bindata.h"
#include "mongo/scripting/mozjs/bson.h"
#include "mongo/scripting/mozjs/code.h"
#include "mongo/scripting/mozjs/countdownlatch.h"
#include "mongo/scripting/mozjs/cursor.h"
#include "mongo/scripting/mozjs/cursor_handle.h"
#include "mongo/scripting/mozjs/db.h"
#include "mongo/scripting/mozjs/dbcollection.h"
#include "mongo/scripting/mozjs/dbpointer.h"
#include "mongo/scripting/mozjs/dbquery.h"
#include "mongo/scripting/mozjs/dbref.h"
#include "mongo/scripting/mozjs/engine.h"
#include "mongo/scripting/mozjs/error.h"
#include "mongo/scripting/mozjs/freeOpToJSContext.h"
#include "mongo/scripting/mozjs/global.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/jsthread.h"
#include "mongo/scripting/mozjs/maxkey.h"
#include "mongo/scripting/mozjs/minkey.h"
#include "mongo/scripting/mozjs/mongo.h"
#include "mongo/scripting/mozjs/mongohelpers.h"
#include "mongo/scripting/mozjs/nativefunction.h"
#include "mongo/scripting/mozjs/numberdecimal.h"
#include "mongo/scripting/mozjs/numberint.h"
#include "mongo/scripting/mozjs/numberlong.h"
#include "mongo/scripting/mozjs/object.h"
#include "mongo/scripting/mozjs/oid.h"
#include "mongo/scripting/mozjs/regexp.h"
#include "mongo/scripting/mozjs/session.h"
#include "mongo/scripting/mozjs/status.h"
#include "mongo/scripting/mozjs/timestamp.h"
#include "mongo/scripting/mozjs/uri.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {
namespace mozjs {

/**
 * Implementation Scope for MozJS
 *
 * The Implementation scope holds the actual mozjs runtime and context objects,
 * along with a number of global prototypes for mongoDB specific types. Each
 * ImplScope requires it's own thread and cannot be accessed from any thread
 * other than the one it was created on (this is a detail inherited from the
 * JSRuntime). If you need a scope that can be accessed by different threads
 * over the course of it's lifetime, see MozJSProxyScope
 *
 * For more information about overriden fields, see mongo::Scope
 */
class MozJSImplScope final : public Scope {
    MozJSImplScope(const MozJSImplScope&) = delete;
    MozJSImplScope& operator=(const MozJSImplScope&) = delete;

public:
    explicit MozJSImplScope(MozJSScriptEngine* engine, boost::optional<int> jsHeapLimitMB);
    ~MozJSImplScope();

    void init(const BSONObj* data) override;

    void reset() override;

    void kill() override;

    void interrupt();

    bool isKillPending() const override;

    OperationContext* getOpContext() const;

    void registerOperation(OperationContext* opCtx) override;

    void unregisterOperation() override;

    void externalSetup() override;

    std::string getError() override;

    bool hasOutOfMemoryException() override;

    void gc() override;

    void sleep(Milliseconds ms);

    bool isJavaScriptProtectionEnabled() const;

    double getNumber(const char* field) override;
    int getNumberInt(const char* field) override;
    long long getNumberLongLong(const char* field) override;
    Decimal128 getNumberDecimal(const char* field) override;
    std::string getString(const char* field) override;
    bool getBoolean(const char* field) override;
    BSONObj getObject(const char* field) override;

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

    void newFunction(StringData code, JS::MutableHandleValue out);

    BSONObj callThreadArgs(const BSONObj& obj);

    template <typename T>
    typename std::enable_if<std::is_same<T, BinDataInfo>::value, WrapType<T>&>::type getProto() {
        return _binDataProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, BSONInfo>::value, WrapType<T>&>::type getProto() {
        return _bsonProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, CountDownLatchInfo>::value, WrapType<T>&>::type
    getProto() {
        return _countDownLatchProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, CursorInfo>::value, WrapType<T>&>::type getProto() {
        return _cursorProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, CursorHandleInfo>::value, WrapType<T>&>::type
    getProto() {
        return _cursorHandleProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, DBCollectionInfo>::value, WrapType<T>&>::type
    getProto() {
        return _dbCollectionProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, DBPointerInfo>::value, WrapType<T>&>::type getProto() {
        return _dbPointerProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, DBQueryInfo>::value, WrapType<T>&>::type getProto() {
        return _dbQueryProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, DBInfo>::value, WrapType<T>&>::type getProto() {
        return _dbProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, DBRefInfo>::value, WrapType<T>&>::type getProto() {
        return _dbRefProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, ErrorInfo>::value, WrapType<T>&>::type getProto() {
        return _errorProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, JSThreadInfo>::value, WrapType<T>&>::type getProto() {
        return _jsThreadProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, MaxKeyInfo>::value, WrapType<T>&>::type getProto() {
        return _maxKeyProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, MinKeyInfo>::value, WrapType<T>&>::type getProto() {
        return _minKeyProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, MongoExternalInfo>::value, WrapType<T>&>::type
    getProto() {
        return _mongoExternalProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, MongoHelpersInfo>::value, WrapType<T>&>::type
    getProto() {
        return _mongoHelpersProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, NativeFunctionInfo>::value, WrapType<T>&>::type
    getProto() {
        return _nativeFunctionProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, NumberIntInfo>::value, WrapType<T>&>::type getProto() {
        return _numberIntProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, NumberLongInfo>::value, WrapType<T>&>::type getProto() {
        return _numberLongProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, NumberDecimalInfo>::value, WrapType<T>&>::type
    getProto() {
        return _numberDecimalProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, CodeInfo>::value, WrapType<T>&>::type getProto() {
        return _codeProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, ObjectInfo>::value, WrapType<T>&>::type getProto() {
        return _objectProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, OIDInfo>::value, WrapType<T>&>::type getProto() {
        return _oidProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, RegExpInfo>::value, WrapType<T>&>::type getProto() {
        return _regExpProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, SessionInfo>::value, WrapType<T>&>::type getProto() {
        return _sessionProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, MongoStatusInfo>::value, WrapType<T>&>::type
    getProto() {
        return _statusProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, TimestampInfo>::value, WrapType<T>&>::type getProto() {
        return _timestampProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, URIInfo>::value, WrapType<T>&>::type getProto() {
        return _uriProto;
    }

    template <typename T>
    typename std::enable_if<std::is_same<T, GlobalInfo>::value, WrapType<T>&>::type getProto() {
        return _globalProto;
    }

    static const char* const kExecResult;
    static const char* const kInvokeResult;

    static MozJSImplScope* getThreadScope();
    void setOOM();
    void setParentStack(std::string);
    const std::string& getParentStack() const;

    std::size_t getGeneration() const;

    void advanceGeneration() override;

    void requireOwnedObjects() override;

    bool requiresOwnedObjects() const;

    JS::HandleId getInternedStringId(InternedString name) {
        return _internedStrings.getInternedString(name);
    }

    std::string buildStackString();

    template <typename T, typename... Args>
    T* trackedNew(Args&&... args) {
        T* t = new T(std::forward<Args>(args)...);
        _asanHandles.addPointer(t);
        return t;
    }

    template <typename T>
    void trackedDelete(T* t) {
        _asanHandles.removePointer(t);
        delete (t);
    }

    struct ASANHandles {
        ASANHandles();
        ~ASANHandles();

        void addPointer(void* ptr);
        void removePointer(void* ptr);

        stdx::unordered_set<void*> _handles;

        static ASANHandles* getThreadASANHandles();
    };

    void setStatus(Status status);

private:
    template <typename ImplScopeFunction>
    auto _runSafely(ImplScopeFunction&& functionToRun) -> decltype(functionToRun());

    void _MozJSCreateFunction(StringData raw, JS::MutableHandleValue fun);

    /**
     * This structure exists exclusively to construct the runtime and context
     * ahead of the various global prototypes in the ImplScope construction.
     * Basically, we have to call some c apis on the way up and down and this
     * takes care of that
     */
    struct MozRuntime {
    public:
        MozRuntime(const MozJSScriptEngine* engine, boost::optional<int> jsHeapLimitMB);

        std::thread _thread;  // NOLINT
        std::unique_ptr<JSRuntime, std::function<void(JSRuntime*)>> _runtime;
        std::unique_ptr<JSContext, std::function<void(JSContext*)>> _context;
    };

    /**
     * The connection state of the scope.
     */
    enum class ConnectState : char {
        Not,
        External,
    };

    struct MozJSEntry;
    friend struct MozJSEntry;

    static bool _interruptCallback(JSContext* cx);
    static void _gcCallback(JSContext* rt, JSGCStatus status, void* data);
    bool _checkErrorState(bool success, bool reportError = true, bool assertOnError = true);

    void installDBAccess();
    void installBSONTypes();
    void installFork();

    void setCompileOptions(JS::CompileOptions* co);

    ASANHandles _asanHandles;
    MozJSScriptEngine* _engine;
    MozRuntime _mr;
    JSContext* _context;
    WrapType<GlobalInfo> _globalProto;
    JS::HandleObject _global;
    std::vector<JS::PersistentRootedValue> _funcs;
    InternedStringTable _internedStrings;
    Status _killStatus;
    mutable Mutex _mutex = MONGO_MAKE_LATCH("MozJSImplScope::_mutex");
    stdx::condition_variable _sleepCondition;
    std::string _error;
    unsigned int _opId;        // op id for this scope
    OperationContext* _opCtx;  // Op context for DbEval
    std::size_t _inOp;
    std::atomic<bool> _pendingGC;  // NOLINT
    ConnectState _connectState;
    Status _status;
    std::string _parentStack;
    std::size_t _generation;
    bool _requireOwnedObjects;
    bool _hasOutOfMemoryException;

    bool _inReportError;

    WrapType<BinDataInfo> _binDataProto;
    WrapType<BSONInfo> _bsonProto;
    WrapType<CodeInfo> _codeProto;
    WrapType<CountDownLatchInfo> _countDownLatchProto;
    WrapType<CursorHandleInfo> _cursorHandleProto;
    WrapType<CursorInfo> _cursorProto;
    WrapType<DBCollectionInfo> _dbCollectionProto;
    WrapType<DBInfo> _dbProto;
    WrapType<DBPointerInfo> _dbPointerProto;
    WrapType<DBQueryInfo> _dbQueryProto;
    WrapType<DBRefInfo> _dbRefProto;
    WrapType<ErrorInfo> _errorProto;
    WrapType<JSThreadInfo> _jsThreadProto;
    WrapType<MaxKeyInfo> _maxKeyProto;
    WrapType<MinKeyInfo> _minKeyProto;
    WrapType<MongoExternalInfo> _mongoExternalProto;
    WrapType<MongoHelpersInfo> _mongoHelpersProto;
    WrapType<NativeFunctionInfo> _nativeFunctionProto;
    WrapType<NumberDecimalInfo> _numberDecimalProto;
    WrapType<NumberIntInfo> _numberIntProto;
    WrapType<NumberLongInfo> _numberLongProto;
    WrapType<ObjectInfo> _objectProto;
    WrapType<OIDInfo> _oidProto;
    WrapType<RegExpInfo> _regExpProto;
    WrapType<SessionInfo> _sessionProto;
    WrapType<MongoStatusInfo> _statusProto;
    WrapType<TimestampInfo> _timestampProto;
    WrapType<URIInfo> _uriProto;
};

inline MozJSImplScope* getScope(JSContext* cx) {
    return static_cast<MozJSImplScope*>(JS_GetContextPrivate(cx));
}

inline MozJSImplScope* getScope(js::FreeOp* fop) {
    return getScope(freeOpToJSContext(fop));
}


}  // namespace mozjs
}  // namespace mongo
