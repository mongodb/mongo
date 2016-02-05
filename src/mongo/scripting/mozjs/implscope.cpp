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

#include "mongo/scripting/mozjs/implscope.h"

#include <jscustomallocator.h>
#include <jsfriendapi.h>

#include "mongo/base/error_codes.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/stack_locator.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

using namespace mongoutils;

namespace mongo {

// Generated symbols for JS files
namespace JSFiles {
extern const JSFile types;
extern const JSFile assert;
}  // namespace

namespace mozjs {

const char* const MozJSImplScope::kExecResult = "__lastres__";
const char* const MozJSImplScope::kInvokeResult = "__returnValue";

namespace {

/**
 * The maximum amount of memory to be given out per thread to mozilla. We
 * manage this by trapping all calls to malloc, free, etc. and keeping track of
 * counts in some thread locals
 */
const size_t kMallocMemoryLimit = 1024ul * 1024 * 1024 * 1.1;

/**
 * The number of bytes to allocate after which garbage collection is run
 */
const int kMaxBytesBeforeGC = 8 * 1024 * 1024;

/**
 * The size, in bytes, of each "stack chunk". 8192 is the recommended amount
 * from mozilla
 */
const int kStackChunkSize = 8192;

/**
 * Runtime's can race on first creation (on some function statics), so we just
 * serialize the initial Runtime creation.
 */
stdx::mutex gRuntimeCreationMutex;
bool gFirstRuntimeCreated = false;

}  // namespace

MONGO_TRIVIALLY_CONSTRUCTIBLE_THREAD_LOCAL MozJSImplScope* kCurrentScope;

struct MozJSImplScope::MozJSEntry {
    MozJSEntry(MozJSImplScope* scope) : ar(scope->_context), ac(scope->_context, scope->_global) {}

    JSAutoRequest ar;
    JSAutoCompartment ac;
};

void MozJSImplScope::_reportError(JSContext* cx, const char* message, JSErrorReport* report) {
    auto scope = getScope(cx);

    if (!JSREPORT_IS_WARNING(report->flags)) {
        str::stream ss;
        ss << message;

        // TODO: something far more elaborate that mimics the stack printing from v8
        JS::RootedValue excn(cx);
        if (JS_GetPendingException(cx, &excn) && excn.isObject()) {
            JS::RootedValue stack(cx);

            ObjectWrapper(cx, excn).getValue("stack", &stack);

            auto str = ValueWriter(cx, stack).toString();

            if (str.empty()) {
                ss << " @" << report->filename << ":" << report->lineno << ":" << report->column
                   << "\n";
            } else {
                ss << " :\n" << str;
            }
        }

        scope->_status = Status(
            JSErrorReportToStatus(cx, report, ErrorCodes::JSInterpreterFailure, message).code(),
            ss);
    }
}

std::string MozJSImplScope::getError() {
    return "";
}

void MozJSImplScope::registerOperation(OperationContext* txn) {
    invariant(_opId == 0);

    // getPooledScope may call registerOperation with a nullptr, so we have to
    // check for that here.
    if (!txn)
        return;

    _opId = txn->getOpID();

    _engine->registerOperation(txn, this);
}

void MozJSImplScope::unregisterOperation() {
    if (_opId != 0) {
        _engine->unregisterOperation(_opId);

        _opId = 0;
    }
}

void MozJSImplScope::kill() {
    _pendingKill.store(true);
    JS_RequestInterruptCallback(_runtime);
}

bool MozJSImplScope::isKillPending() const {
    return _pendingKill.load();
}

OperationContext* MozJSImplScope::getOpContext() const {
    return _opCtx;
}

bool MozJSImplScope::_interruptCallback(JSContext* cx) {
    auto scope = getScope(cx);

    JS_SetInterruptCallback(scope->_runtime, nullptr);
    auto guard = MakeGuard([&]() { JS_SetInterruptCallback(scope->_runtime, _interruptCallback); });

    if (scope->_pendingGC.load()) {
        scope->_pendingGC.store(false);
        JS_GC(scope->_runtime);
    } else {
        JS_MaybeGC(cx);
    }

    if (scope->_hasOutOfMemoryException) {
        scope->_status = Status(ErrorCodes::JSInterpreterFailure, "Out of memory");
    } else if (scope->isKillPending()) {
        scope->_status = Status(ErrorCodes::Interrupted, "Interrupted by the host");
    }

    if (!scope->_status.isOK()) {
        scope->_engine->getDeadlineMonitor().stopDeadline(scope);
        scope->unregisterOperation();
    }

    return scope->_status.isOK();
}

void MozJSImplScope::_gcCallback(JSRuntime* rt, JSGCStatus status, void* data) {
    if (!shouldLog(logger::LogSeverity::Debug(1))) {
        // don't collect stats unless verbose
        return;
    }

    log() << "MozJS GC " << (status == JSGC_BEGIN ? "prologue" : "epilogue") << " heap stats - "
          << " total: " << mongo::sm::get_total_bytes() << " limit: " << mongo::sm::get_max_bytes()
          << std::endl;
}

MozJSImplScope::MozRuntime::MozRuntime(const MozJSScriptEngine* engine) {
    mongo::sm::reset(kMallocMemoryLimit);

    // If this runtime isn't running on an NSPR thread, then it is
    // running on a mongo thread. In that case, we need to insert a
    // fake NSPR thread so that the SM runtime can call PR functions
    // without falling over.
    auto thread = PR_GetCurrentThread();
    if (!thread) {
        PR_BindThread(_thread = PR_CreateFakeThread());
    }

    {
        stdx::unique_lock<stdx::mutex> lk(gRuntimeCreationMutex);

        if (gFirstRuntimeCreated) {
            // If we've already made a runtime, just proceed
            lk.unlock();
        } else {
            // If this is the first one, hold the lock until after the first
            // one's done
            gFirstRuntimeCreated = true;
        }

        _runtime = JS_NewRuntime(kMaxBytesBeforeGC);
        uassert(ErrorCodes::JSInterpreterFailure, "Failed to initialize JSRuntime", _runtime);

        // We turn on a variety of optimizations if the jit is enabled
        if (engine->isJITEnabled()) {
            JS::RuntimeOptionsRef(_runtime)
                .setAsmJS(true)
                .setBaseline(true)
                .setIon(true)
                .setNativeRegExp(true)
                .setUnboxedObjects(true);
        }

        const StackLocator locator;
        const auto available = locator.available();
        if (available) {
            // We fudge by 64k for a two reasons. First, it appears
            // that the internal recursion checks that SM performs can
            // have stack usage between checks of more than 32k in
            // some builds. Second, some platforms report the guard
            // page (in the linux sense) as "part of the stack", even
            // though accessing that page will fault the process. We
            // don't have a good way of getting information about the
            // guard page on those platforms.
            //
            // TODO: What if we are running on a platform with very
            // large pages, like 4MB?
            JS_SetNativeStackQuota(_runtime, available.get() - (64 * 1024));
        }

        // The memory limit is in megabytes
        JS_SetGCParametersBasedOnAvailableMemory(_runtime, kMallocMemoryLimit / (1024 * 1024));
    }

    _context = JS_NewContext(_runtime, kStackChunkSize);
    uassert(ErrorCodes::JSInterpreterFailure, "Failed to initialize JSContext", _context);
}

MozJSImplScope::MozRuntime::~MozRuntime() {
    JS_DestroyContext(_context);
    JS_DestroyRuntime(_runtime);

    if (_thread) {
        invariant(PR_GetCurrentThread() == _thread);
        PR_DestroyFakeThread(_thread);
        PR_BindThread(nullptr);
    }
}

MozJSImplScope::MozJSImplScope(MozJSScriptEngine* engine)
    : _engine(engine),
      _mr(engine),
      _runtime(_mr._runtime),
      _context(_mr._context),
      _globalProto(_context),
      _global(_globalProto.getProto()),
      _funcs(),
      _internedStrings(_context),
      _pendingKill(false),
      _opId(0),
      _opCtx(nullptr),
      _pendingGC(false),
      _connectState(ConnectState::Not),
      _status(Status::OK()),
      _quickExit(false),
      _generation(0),
      _hasOutOfMemoryException(false),
      _binDataProto(_context),
      _bsonProto(_context),
      _countDownLatchProto(_context),
      _cursorProto(_context),
      _cursorHandleProto(_context),
      _dbCollectionProto(_context),
      _dbPointerProto(_context),
      _dbQueryProto(_context),
      _dbProto(_context),
      _dbRefProto(_context),
      _errorProto(_context),
      _jsThreadProto(_context),
      _maxKeyProto(_context),
      _minKeyProto(_context),
      _mongoExternalProto(_context),
      _mongoHelpersProto(_context),
      _mongoLocalProto(_context),
      _nativeFunctionProto(_context),
      _numberIntProto(_context),
      _numberLongProto(_context),
      _numberDecimalProto(_context),
      _objectProto(_context),
      _oidProto(_context),
      _regExpProto(_context),
      _timestampProto(_context) {
    kCurrentScope = this;

    // The default is quite low and doesn't seem to directly correlate with
    // malloc'd bytes.  Set it to MAX_INT here and catching things in the
    // jscustomallocator.cpp
    JS_SetGCParameter(_runtime, JSGC_MAX_BYTES, 0xffffffff);

    JS_SetInterruptCallback(_runtime, _interruptCallback);
    JS_SetGCCallback(_runtime, _gcCallback, this);
    JS_SetContextPrivate(_context, this);
    JSAutoRequest ar(_context);

    JS_SetErrorReporter(_runtime, _reportError);

    JSAutoCompartment ac(_context, _global);

    _checkErrorState(JS_InitStandardClasses(_context, _global));

    installBSONTypes();

    JS_FireOnNewGlobalObject(_context, _global);

    execSetup(JSFiles::assert);
    execSetup(JSFiles::types);

    // install process-specific utilities in the global scope (dependancy: types.js, assert.js)
    if (_engine->getScopeInitCallback())
        _engine->getScopeInitCallback()(*this);

    // install global utility functions
    installGlobalUtils(*this);
    _mongoHelpersProto.install(_global);
}

MozJSImplScope::~MozJSImplScope() {
    for (auto&& x : _funcs) {
        x.reset();
    }

    unregisterOperation();
}

bool MozJSImplScope::hasOutOfMemoryException() {
    return _hasOutOfMemoryException;
}

void MozJSImplScope::init(const BSONObj* data) {
    if (!data)
        return;

    BSONObjIterator i(*data);
    while (i.more()) {
        BSONElement e = i.next();
        setElement(e.fieldName(), e, *data);
    }
}

void MozJSImplScope::setNumber(const char* field, double val) {
    MozJSEntry entry(this);

    ObjectWrapper(_context, _global).setNumber(field, val);
}

void MozJSImplScope::setString(const char* field, StringData val) {
    MozJSEntry entry(this);

    ObjectWrapper(_context, _global).setString(field, val);
}

void MozJSImplScope::setBoolean(const char* field, bool val) {
    MozJSEntry entry(this);

    ObjectWrapper(_context, _global).setBoolean(field, val);
}

void MozJSImplScope::setElement(const char* field, const BSONElement& e, const BSONObj& parent) {
    MozJSEntry entry(this);

    ObjectWrapper(_context, _global).setBSONElement(field, e, parent, false);
}

void MozJSImplScope::setObject(const char* field, const BSONObj& obj, bool readOnly) {
    MozJSEntry entry(this);

    ObjectWrapper(_context, _global).setBSON(field, obj, readOnly);
}

int MozJSImplScope::type(const char* field) {
    MozJSEntry entry(this);

    return ObjectWrapper(_context, _global).type(field);
}

double MozJSImplScope::getNumber(const char* field) {
    MozJSEntry entry(this);

    return ObjectWrapper(_context, _global).getNumber(field);
}

int MozJSImplScope::getNumberInt(const char* field) {
    MozJSEntry entry(this);

    return ObjectWrapper(_context, _global).getNumberInt(field);
}

long long MozJSImplScope::getNumberLongLong(const char* field) {
    MozJSEntry entry(this);

    return ObjectWrapper(_context, _global).getNumberLongLong(field);
}

Decimal128 MozJSImplScope::getNumberDecimal(const char* field) {
    MozJSEntry entry(this);

    return ObjectWrapper(_context, _global).getNumberDecimal(field);
}

std::string MozJSImplScope::getString(const char* field) {
    MozJSEntry entry(this);

    return ObjectWrapper(_context, _global).getString(field);
}

bool MozJSImplScope::getBoolean(const char* field) {
    MozJSEntry entry(this);

    return ObjectWrapper(_context, _global).getBoolean(field);
}

BSONObj MozJSImplScope::getObject(const char* field) {
    MozJSEntry entry(this);

    return ObjectWrapper(_context, _global).getObject(field);
}

void MozJSImplScope::newFunction(StringData raw, JS::MutableHandleValue out) {
    MozJSEntry entry(this);

    std::string code = str::stream() << "____MongoToSM_newFunction_temp = " << raw;

    JS::CompileOptions co(_context);
    setCompileOptions(&co);
    _checkErrorState(JS::Evaluate(_context, _global, co, code.c_str(), code.length(), out));
}

BSONObj MozJSImplScope::callThreadArgs(const BSONObj& args) {
    MozJSEntry entry(this);

    JS::RootedValue function(_context);
    ValueReader(_context, &function).fromBSONElement(args.firstElement(), args, true);

    int argc = args.nFields() - 1;

    JS::AutoValueVector argv(_context);
    BSONObjIterator it(args);
    it.next();
    JS::RootedValue value(_context);

    for (int i = 0; i < argc; ++i) {
        ValueReader(_context, &value).fromBSONElement(*it, args, true);
        argv.append(value);
        it.next();
    }

    JS::RootedValue out(_context);
    JS::RootedObject thisv(_context);

    _checkErrorState(JS::Call(_context, thisv, function, argv, &out), false, true);

    JS::RootedObject rout(_context, JS_NewPlainObject(_context));
    ObjectWrapper wout(_context, rout);
    wout.setValue("ret", out);

    return wout.toBSON();
}

bool hasFunctionIdentifier(StringData code) {
    if (code.size() < 9 || code.find("function") != 0)
        return false;

    return code[8] == ' ' || code[8] == '(';
}

void MozJSImplScope::_MozJSCreateFunction(const char* raw,
                                          ScriptingFunction functionNumber,
                                          JS::MutableHandleValue fun) {
    std::string code = str::stream() << "_funcs" << functionNumber << " = "
                                     << parseJSFunctionOrExpression(_context, StringData(raw));

    JS::CompileOptions co(_context);
    setCompileOptions(&co);

    _checkErrorState(JS::Evaluate(_context, _global, co, code.c_str(), code.length(), fun));
    uassert(10232,
            "not a function",
            fun.isObject() && JS_ObjectIsFunction(_context, fun.toObjectOrNull()));
}

ScriptingFunction MozJSImplScope::_createFunction(const char* raw,
                                                  ScriptingFunction functionNumber) {
    MozJSEntry entry(this);

    JS::RootedValue fun(_context);
    _MozJSCreateFunction(raw, functionNumber, &fun);
    _funcs.emplace_back(_context, fun.get());

    return functionNumber;
}

void MozJSImplScope::setFunction(const char* field, const char* code) {
    MozJSEntry entry(this);

    JS::RootedValue fun(_context);

    _MozJSCreateFunction(code, getFunctionCache().size() + 1, &fun);

    ObjectWrapper(_context, _global).setValue(field, fun);
}

void MozJSImplScope::rename(const char* from, const char* to) {
    MozJSEntry entry(this);

    ObjectWrapper(_context, _global).rename(from, to);
}

int MozJSImplScope::invoke(ScriptingFunction func,
                           const BSONObj* argsObject,
                           const BSONObj* recv,
                           int timeoutMs,
                           bool ignoreReturn,
                           bool readOnlyArgs,
                           bool readOnlyRecv) {
    MozJSEntry entry(this);

    auto funcValue = _funcs[func - 1];
    JS::RootedValue result(_context);

    const int nargs = argsObject ? argsObject->nFields() : 0;

    JS::AutoValueVector args(_context);

    if (nargs) {
        BSONObjIterator it(*argsObject);
        for (int i = 0; i < nargs; i++) {
            BSONElement next = it.next();

            JS::RootedValue value(_context);
            ValueReader(_context, &value).fromBSONElement(next, *argsObject, readOnlyArgs);

            args.append(value);
        }
    }

    JS::RootedValue smrecv(_context);
    if (recv)
        ValueReader(_context, &smrecv).fromBSON(*recv, nullptr, readOnlyRecv);
    else
        smrecv.setObjectOrNull(_global);

    if (timeoutMs)
        _engine->getDeadlineMonitor().startDeadline(this, timeoutMs);

    JS::RootedValue out(_context);
    JS::RootedObject obj(_context, smrecv.toObjectOrNull());

    bool success = JS::Call(_context, obj, funcValue, args, &out);

    if (timeoutMs)
        _engine->getDeadlineMonitor().stopDeadline(this);

    _checkErrorState(success);

    if (!ignoreReturn) {
        // must validate the handle because TerminateExecution may have
        // been thrown after the above checks
        if (out.isObject() && _nativeFunctionProto.instanceOf(out)) {
            warning() << "storing native function as return value";
            _lastRetIsNativeCode = true;
        } else {
            _lastRetIsNativeCode = false;
        }

        ObjectWrapper(_context, _global).setValue(kInvokeResult, out);
    }

    return 0;
}

bool MozJSImplScope::exec(StringData code,
                          const std::string& name,
                          bool printResult,
                          bool reportError,
                          bool assertOnError,
                          int timeoutMs) {
    MozJSEntry entry(this);

    JS::CompileOptions co(_context);
    setCompileOptions(&co);
    co.setFile(name.c_str());
    JS::RootedScript script(_context);

    bool success = JS::Compile(_context, _global, co, code.rawData(), code.size(), &script);

    if (_checkErrorState(success, reportError, assertOnError))
        return false;

    if (timeoutMs)
        _engine->getDeadlineMonitor().startDeadline(this, timeoutMs);

    JS::RootedValue out(_context);

    success = JS_ExecuteScript(_context, _global, script, &out);

    if (timeoutMs)
        _engine->getDeadlineMonitor().stopDeadline(this);

    if (_checkErrorState(success, reportError, assertOnError))
        return false;

    ObjectWrapper(_context, _global).setValue(kExecResult, out);

    if (printResult && !out.isUndefined()) {
        // appears to only be used by shell
        std::cout << ValueWriter(_context, out).toString() << std::endl;
    }

    return true;
}

void MozJSImplScope::injectNative(const char* field, NativeFunction func, void* data) {
    MozJSEntry entry(this);

    JS::RootedObject obj(_context);

    NativeFunctionInfo::make(_context, &obj, func, data);

    JS::RootedValue value(_context);
    value.setObjectOrNull(obj);
    ObjectWrapper(_context, _global).setValue(field, value);
}

void MozJSImplScope::gc() {
    _pendingGC.store(true);
    JS_RequestInterruptCallback(_runtime);
}

void MozJSImplScope::localConnectForDbEval(OperationContext* txn, const char* dbName) {
    MozJSEntry entry(this);

    invariant(_opCtx == NULL);
    _opCtx = txn;

    if (_connectState == ConnectState::External)
        uasserted(12510, "externalSetup already called, can't call localConnect");
    if (_connectState == ConnectState::Local) {
        if (_localDBName == dbName)
            return;
        uasserted(12511,
                  str::stream() << "localConnect previously called with name " << _localDBName);
    }

    // NOTE: order is important here.  the following methods must be called after
    //       the above conditional statements.

    // install db access functions in the global object
    installDBAccess();

    // install the Mongo function object and instantiate the 'db' global
    _mongoLocalProto.install(_global);
    execCoreFiles();

    const char* const makeMongo = "_mongo = new Mongo()";
    exec(makeMongo, "local connect 2", false, true, true, 0);

    std::string makeDB = str::stream() << "db = _mongo.getDB(\"" << dbName << "\");";
    exec(makeDB, "local connect 3", false, true, true, 0);

    _connectState = ConnectState::Local;
    _localDBName = dbName;

    loadStored(txn);
}

void MozJSImplScope::externalSetup() {
    MozJSEntry entry(this);

    if (_connectState == ConnectState::External)
        return;
    if (_connectState == ConnectState::Local)
        uasserted(12512, "localConnect already called, can't call externalSetup");

    mongo::sm::reset(0);

    // install db access functions in the global object
    installDBAccess();

    // install thread-related functions (e.g. _threadInject)
    installFork();

    // install the Mongo function object
    _mongoExternalProto.install(_global);
    execCoreFiles();
    _connectState = ConnectState::External;
}

void MozJSImplScope::reset() {
    unregisterOperation();
    _pendingKill.store(false);
    _pendingGC.store(false);
    advanceGeneration();
}

void MozJSImplScope::installBSONTypes() {
    _binDataProto.install(_global);
    _bsonProto.install(_global);
    _dbPointerProto.install(_global);
    _dbRefProto.install(_global);
    _errorProto.install(_global);
    _maxKeyProto.install(_global);
    _minKeyProto.install(_global);
    _nativeFunctionProto.install(_global);
    _numberIntProto.install(_global);
    _numberLongProto.install(_global);
    if (Decimal128::enabled) {
        _numberDecimalProto.install(_global);
    }
    _objectProto.install(_global);
    _oidProto.install(_global);
    _regExpProto.install(_global);
    _timestampProto.install(_global);

    // This builtin map is a javascript 6 thing.  We want our version.  so
    // take theirs out
    ObjectWrapper(_context, _global).deleteProperty("Map");
}

void MozJSImplScope::installDBAccess() {
    _cursorProto.install(_global);
    _cursorHandleProto.install(_global);
    _dbProto.install(_global);
    _dbQueryProto.install(_global);
    _dbCollectionProto.install(_global);
}

void MozJSImplScope::installFork() {
    _countDownLatchProto.install(_global);
    _jsThreadProto.install(_global);
}

bool MozJSImplScope::_checkErrorState(bool success, bool reportError, bool assertOnError) {
    if (success)
        return false;

    if (_quickExit)
        return false;

    if (_status.isOK()) {
        JS::RootedValue excn(_context);
        if (JS_GetPendingException(_context, &excn) && excn.isObject()) {
            str::stream ss;

            JS::RootedValue stack(_context);

            ObjectWrapper(_context, excn).getValue("stack", &stack);

            ss << ValueWriter(_context, excn).toString() << " :\n"
               << ValueWriter(_context, stack).toString();
            _status = Status(ErrorCodes::JSInterpreterFailure, ss);
        } else {
            _status = Status(ErrorCodes::UnknownError, "Unknown Failure from JSInterpreter");
        }
    }

    _error = _status.reason();

    if (reportError)
        error() << _error << std::endl;

    // Clear the status state
    auto status = std::move(_status);

    if (assertOnError) {
        // Throw if necessary
        uassertStatusOK(status);
    }

    return true;
}


void MozJSImplScope::setCompileOptions(JS::CompileOptions* co) {
    co->setUTF8(true);
}

MozJSImplScope* MozJSImplScope::getThreadScope() {
    return kCurrentScope;
}

void MozJSImplScope::setQuickExit(int exitCode) {
    _quickExit = true;
    _exitCode = exitCode;
}

bool MozJSImplScope::getQuickExit(int* exitCode) {
    if (_quickExit) {
        *exitCode = _exitCode;
    }

    return _quickExit;
}

void MozJSImplScope::setOOM() {
    _hasOutOfMemoryException = true;
    JS_RequestInterruptCallback(_runtime);
}

void MozJSImplScope::setParentStack(std::string parentStack) {
    _parentStack = std::move(parentStack);
}

std::size_t MozJSImplScope::getGeneration() const {
    return _generation;
}

void MozJSImplScope::advanceGeneration() {
    _generation++;
}

const std::string& MozJSImplScope::getParentStack() const {
    return _parentStack;
}

}  // namespace mozjs
}  // namespace mongo
