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

#include <js/CharacterEncoding.h>
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
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

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
 * The threshold (as a fraction of the max) after which garbage collection will be run during
 * interrupts.
 */
const double kInterruptGCThreshold = 0.8;

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

bool closeToMaxMemory() {
    return mongo::sm::get_total_bytes() > (kInterruptGCThreshold * mongo::sm::get_max_bytes());
}
}  // namespace

thread_local MozJSImplScope::ASANHandles* kCurrentASANHandles = nullptr;
thread_local MozJSImplScope* kCurrentScope = nullptr;

struct MozJSImplScope::MozJSEntry {
    MozJSEntry(MozJSImplScope* scope)
        : ar(scope->_context), ac(scope->_context, scope->_global), _scope(scope) {
        ++_scope->_inOp;
    }

    ~MozJSEntry() {
        --_scope->_inOp;
    }

    JSAutoRequest ar;
    JSAutoCompartment ac;
    MozJSImplScope* _scope;
};

void MozJSImplScope::_reportError(JSContext* cx, const char* message, JSErrorReport* report) {
    auto scope = getScope(cx);

    if (!JSREPORT_IS_WARNING(report->flags)) {

        std::string exceptionMsg;

        try {
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

            exceptionMsg = ss;
        } catch (const DBException& dbe) {
            exceptionMsg = "Unknown error occured while processing exception";
            log() << exceptionMsg << ":" << dbe.toString() << ":" << message;
        }

        scope->_status = Status(
            JSErrorReportToStatus(cx, report, ErrorCodes::JSInterpreterFailure, message).code(),
            exceptionMsg);
    }
}

std::string MozJSImplScope::getError() {
    return "";
}

void MozJSImplScope::registerOperation(OperationContext* opCtx) {
    invariant(_opCtx == nullptr);

    // getPooledScope may call registerOperation with a nullptr, so we have to
    // check for that here.
    if (!opCtx)
        return;

    _opCtx = opCtx;
    _opId = opCtx->getOpID();

    _engine->registerOperation(opCtx, this);
}

void MozJSImplScope::unregisterOperation() {
    if (_opCtx) {
        _engine->unregisterOperation(_opId);
        _opCtx = nullptr;
    }
}

void MozJSImplScope::kill() {
    _pendingKill.store(true);
    JS_RequestInterruptCallback(_runtime);
}

void MozJSImplScope::interrupt() {
    JS_RequestInterruptCallback(_runtime);
}

bool MozJSImplScope::isKillPending() const {
    return _pendingKill.load();
}

OperationContext* MozJSImplScope::getOpContext() const {
    return _opCtx;
}

bool MozJSImplScope::isJavaScriptProtectionEnabled() const {
    return _engine->isJavaScriptProtectionEnabled();
}

bool MozJSImplScope::_interruptCallback(JSContext* cx) {
    auto scope = getScope(cx);

    JS_SetInterruptCallback(scope->_runtime, nullptr);
    auto guard = MakeGuard([&]() { JS_SetInterruptCallback(scope->_runtime, _interruptCallback); });

    if (scope->_pendingGC.load() || closeToMaxMemory()) {
        scope->_pendingGC.store(false);
        JS_GC(scope->_runtime);
    } else {
        JS_MaybeGC(cx);
    }

    if (scope->_hasOutOfMemoryException) {
        scope->_status = Status(ErrorCodes::JSInterpreterFailure, "Out of memory");
    } else if (scope->isKillPending()) {
        scope->_status = Status(ErrorCodes::Interrupted, "JavaScript execution interrupted");
    }
    // If we are on the right thread, in the middle of an operation, and we have a registered opCtx,
    // then we should check the opCtx for interrupts.
    if ((scope->_mr._thread.get() == PR_GetCurrentThread()) && (scope->_inOp > 0) &&
        scope->_opCtx) {
        auto status = scope->_opCtx->checkForInterruptNoAssert();
        if (!status.isOK()) {
            scope->_status = status;
        }
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
          << " total: " << mongo::sm::get_total_bytes() << " limit: " << mongo::sm::get_max_bytes();
}

#if __has_feature(address_sanitizer)

MozJSImplScope::ASANHandles::ASANHandles() {
    kCurrentASANHandles = this;
}

MozJSImplScope::ASANHandles::~ASANHandles() {
    invariant(kCurrentASANHandles == this);
    kCurrentASANHandles = nullptr;
}

void MozJSImplScope::ASANHandles::addPointer(void* ptr) {
    bool inserted;
    std::tie(std::ignore, inserted) = _handles.insert(ptr);
    invariant(inserted);
}

void MozJSImplScope::ASANHandles::removePointer(void* ptr) {
    invariant(_handles.erase(ptr));
}

#else

MozJSImplScope::ASANHandles::ASANHandles() {}

MozJSImplScope::ASANHandles::~ASANHandles() {}

void MozJSImplScope::ASANHandles::addPointer(void* ptr) {}

void MozJSImplScope::ASANHandles::removePointer(void* ptr) {}

#endif


MozJSImplScope::MozRuntime::MozRuntime(const MozJSScriptEngine* engine) {
    /**
     * The maximum amount of memory to be given out per thread to mozilla. We
     * manage this by trapping all calls to malloc, free, etc. and keeping track of
     * counts in some thread locals
     */

    const auto jsHeapLimit = engine->getJSHeapLimitMB();
    if (jsHeapLimit != 0 && jsHeapLimit < 10) {
        warning() << "JavaScript may not be able to initialize with a heap limit less than 10MB.";
    }
    size_t mallocMemoryLimit = 1024ul * 1024 * jsHeapLimit;
    mongo::sm::reset(mallocMemoryLimit);

    // If this runtime isn't running on an NSPR thread, then it is
    // running on a mongo thread. In that case, we need to insert a
    // fake NSPR thread so that the SM runtime can call PR functions
    // without falling over.
    auto thread = PR_GetCurrentThread();
    if (!thread) {
        _thread = std::unique_ptr<PRThread, std::function<void(PRThread*)>>(
            PR_CreateFakeThread(), [](PRThread* ptr) {
                if (ptr) {
                    invariant(PR_GetCurrentThread() == ptr);
                    PR_DestroyFakeThread(ptr);
                    PR_BindThread(nullptr);
                }
            });
        PR_BindThread(_thread.get());
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

        _runtime = std::unique_ptr<JSRuntime, std::function<void(JSRuntime*)>>(
            JS_NewRuntime(kMaxBytesBeforeGC), [](JSRuntime* ptr) { JS_DestroyRuntime(ptr); });
        uassert(ErrorCodes::JSInterpreterFailure, "Failed to initialize JSRuntime", _runtime);

        // We turn on a variety of optimizations if the jit is enabled
        if (engine->isJITEnabled()) {
            JS::RuntimeOptionsRef(_runtime.get())
                .setAsmJS(true)
                .setThrowOnAsmJSValidationFailure(true)
                .setBaseline(true)
                .setIon(true)
                .setAsyncStack(false)
                .setNativeRegExp(true);
        } else {
            JS::RuntimeOptionsRef(_runtime.get())
                .setAsmJS(false)
                .setThrowOnAsmJSValidationFailure(false)
                .setBaseline(false)
                .setIon(false)
                .setAsyncStack(false)
                .setNativeRegExp(false);
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
            const auto available_stack_space = available.get();

#if defined(__powerpc64__) && defined(MONGO_CONFIG_DEBUG_BUILD)
            // From experimentation, we need a larger reservation of 96k since debug ppc64le code
            // needs more stack space to process stack overflow. In debug builds, more variables are
            // stored on the stack which increases the stack pressure. It does not affects non-debug
            // builds.
            const decltype(available_stack_space) reserve_stack_space = 96 * 1024;
#else
            const decltype(available_stack_space) reserve_stack_space = 64 * 1024;
#endif

            JS_SetNativeStackQuota(_runtime.get(), available_stack_space - reserve_stack_space);
        }

        // The memory limit is in megabytes
        JS_SetGCParametersBasedOnAvailableMemory(_runtime.get(), engine->getJSHeapLimitMB());
    }

    _context = std::unique_ptr<JSContext, std::function<void(JSContext*)>>(
        JS_NewContext(_runtime.get(), kStackChunkSize),
        [](JSContext* ptr) { JS_DestroyContext(ptr); });
    uassert(ErrorCodes::JSInterpreterFailure, "Failed to initialize JSContext", _context);
    uassert(ErrorCodes::ExceededMemoryLimit,
            "Out of memory while trying to initialize javascript scope",
            mallocMemoryLimit == 0 || mongo::sm::get_total_bytes() < mallocMemoryLimit);
}

MozJSImplScope::MozJSImplScope(MozJSScriptEngine* engine)
    : _engine(engine),
      _mr(engine),
      _runtime(_mr._runtime.get()),
      _context(_mr._context.get()),
      _globalProto(_context),
      _global(_globalProto.getProto()),
      _funcs(),
      _internedStrings(_context),
      _pendingKill(false),
      _opId(0),
      _opCtx(nullptr),
      _inOp(0),
      _pendingGC(false),
      _connectState(ConnectState::Not),
      _status(Status::OK()),
      _generation(0),
      _requireOwnedObjects(false),
      _hasOutOfMemoryException(false),
      _binDataProto(_context),
      _bsonProto(_context),
      _codeProto(_context),
      _countDownLatchProto(_context),
      _cursorHandleProto(_context),
      _cursorProto(_context),
      _dbCollectionProto(_context),
      _dbProto(_context),
      _dbPointerProto(_context),
      _dbQueryProto(_context),
      _dbRefProto(_context),
      _errorProto(_context),
      _jsThreadProto(_context),
      _maxKeyProto(_context),
      _minKeyProto(_context),
      _mongoExternalProto(_context),
      _mongoHelpersProto(_context),
      _mongoLocalProto(_context),
      _nativeFunctionProto(_context),
      _numberDecimalProto(_context),
      _numberIntProto(_context),
      _numberLongProto(_context),
      _objectProto(_context),
      _oidProto(_context),
      _regExpProto(_context),
      _sessionProto(_context),
      _timestampProto(_context),
      _uriProto(_context) {
    kCurrentScope = this;

    // The default is quite low and doesn't seem to directly correlate with
    // malloc'd bytes.  Set it to MAX_INT here and catching things in the
    // jscustomallocator.cpp
    JS_SetGCParameter(_runtime, JSGC_MAX_BYTES, 0xffffffff);

    JS_SetInterruptCallback(_runtime, _interruptCallback);
    JS_SetGCCallback(_runtime, _gcCallback, this);
    JS_SetContextPrivate(_context, this);
    JS_SetRuntimePrivate(_runtime, this);
    JSAutoRequest ar(_context);

    JS_SetErrorReporter(_runtime, _reportError);

    JSAutoCompartment ac(_context, _global);

    _checkErrorState(JS_InitStandardClasses(_context, _global));

    installBSONTypes();

    JS_FireOnNewGlobalObject(_context, _global);

    execSetup(JSFiles::assert);
    execSetup(JSFiles::types);

    // install global utility functions
    installGlobalUtils(*this);
    _mongoHelpersProto.install(_global);

    // install process-specific utilities in the global scope (dependancy: types.js, assert.js)
    if (_engine->getScopeInitCallback())
        _engine->getScopeInitCallback()(*this);
}

MozJSImplScope::~MozJSImplScope() {
    for (auto&& x : _funcs) {
        x.reset();
    }

    unregisterOperation();

    kCurrentScope = nullptr;
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

template <typename ImplScopeFunction>
auto MozJSImplScope::_runSafely(ImplScopeFunction&& functionToRun) -> decltype(functionToRun()) {
    try {
        MozJSEntry entry(this);
        return functionToRun();
    } catch (...) {
        _error = _status.reason();

        // Clear the status state
        auto status = std::move(_status);
        uassertStatusOK(status);
        throw;
    }
}

void MozJSImplScope::setNumber(const char* field, double val) {
    _runSafely([this, &field, &val] { ObjectWrapper(_context, _global).setNumber(field, val); });
}

void MozJSImplScope::setString(const char* field, StringData val) {
    _runSafely([this, &field, &val] { ObjectWrapper(_context, _global).setString(field, val); });
}

void MozJSImplScope::setBoolean(const char* field, bool val) {
    _runSafely([this, &field, &val] { ObjectWrapper(_context, _global).setBoolean(field, val); });
}

void MozJSImplScope::setElement(const char* field, const BSONElement& e, const BSONObj& parent) {
    _runSafely([this, &field, &e, &parent] {

        ObjectWrapper(_context, _global).setBSONElement(field, e, parent, false);
    });
}

void MozJSImplScope::setObject(const char* field, const BSONObj& obj, bool readOnly) {
    _runSafely([this, &field, &obj, &readOnly] {

        ObjectWrapper(_context, _global).setBSON(field, obj, readOnly);
    });
}

int MozJSImplScope::type(const char* field) {
    return _runSafely([this, &field] { return ObjectWrapper(_context, _global).type(field); });
}

double MozJSImplScope::getNumber(const char* field) {
    return _runSafely([this, &field] { return ObjectWrapper(_context, _global).getNumber(field); });
}

int MozJSImplScope::getNumberInt(const char* field) {
    return _runSafely(
        [this, &field] { return ObjectWrapper(_context, _global).getNumberInt(field); });
}

long long MozJSImplScope::getNumberLongLong(const char* field) {
    return _runSafely(
        [this, &field] { return ObjectWrapper(_context, _global).getNumberLongLong(field); });
}

Decimal128 MozJSImplScope::getNumberDecimal(const char* field) {
    return _runSafely(
        [this, &field] { return ObjectWrapper(_context, _global).getNumberDecimal(field); });
}

std::string MozJSImplScope::getString(const char* field) {
    return _runSafely([this, &field] { return ObjectWrapper(_context, _global).getString(field); });
}

bool MozJSImplScope::getBoolean(const char* field) {
    return _runSafely(
        [this, &field] { return ObjectWrapper(_context, _global).getBoolean(field); });
}

BSONObj MozJSImplScope::getObject(const char* field) {
    return _runSafely([this, &field] { return ObjectWrapper(_context, _global).getObject(field); });
}

void MozJSImplScope::newFunction(StringData raw, JS::MutableHandleValue out) {
    MozJSEntry entry(this);

    _MozJSCreateFunction(raw, std::move(out));
}

void MozJSImplScope::_MozJSCreateFunction(StringData raw, JS::MutableHandleValue fun) {
    std::string code = str::stream()
        << "(" << parseJSFunctionOrExpression(_context, StringData(raw)) << ")";

    JS::CompileOptions co(_context);
    setCompileOptions(&co);

    _checkErrorState(JS::Evaluate(_context, co, code.c_str(), code.length(), fun));
    uassert(10232,
            "not a function",
            fun.isObject() && JS_ObjectIsFunction(_context, fun.toObjectOrNull()));
}

BSONObj MozJSImplScope::callThreadArgs(const BSONObj& args) {
    MozJSEntry entry(this);

    JS::RootedValue function(_context);
    auto firstElem = args.firstElement();

    // The first argument must be the thread start function
    if (firstElem.type() != mongo::Code)
        uasserted(ErrorCodes::BadValue, "first thread argument must be a function");

    getScope(_context)->newFunction(firstElem.valueStringData(), &function);

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

ScriptingFunction MozJSImplScope::_createFunction(const char* raw) {
    MozJSEntry entry(this);

    JS::RootedValue fun(_context);
    _MozJSCreateFunction(raw, &fun);
    _funcs.emplace_back(_context, fun.get());
    return _funcs.size();
}

void MozJSImplScope::setFunction(const char* field, const char* code) {
    _runSafely([this, &field, &code] {
        JS::RootedValue fun(_context);
        _MozJSCreateFunction(code, &fun);
        ObjectWrapper(_context, _global).setValue(field, fun);
    });
}

void MozJSImplScope::rename(const char* from, const char* to) {
    _runSafely([this, &from, &to] { ObjectWrapper(_context, _global).rename(from, to); });
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
    else {
        _engine->getDeadlineMonitor().startDeadline(this, -1);
    }

    JS::RootedValue out(_context);
    JS::RootedObject obj(_context, smrecv.toObjectOrNull());

    bool success = JS::Call(_context, obj, funcValue, args, &out);

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
    co.setFileAndLine(name.c_str(), 1);
    JS::RootedScript script(_context);

    bool success = JS::Compile(_context, co, code.rawData(), code.size(), &script);

    if (_checkErrorState(success, reportError, assertOnError))
        return false;

    if (timeoutMs) {
        _engine->getDeadlineMonitor().startDeadline(this, timeoutMs);
    } else {
        _engine->getDeadlineMonitor().startDeadline(this, -1);
    }

    JS::RootedValue out(_context);

    success = JS_ExecuteScript(_context, script, &out);

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
    _runSafely([this, &field, &func, &data] {
        JS::RootedObject obj(_context);

        NativeFunctionInfo::make(_context, &obj, func, data);

        JS::RootedValue value(_context);
        value.setObjectOrNull(obj);
        ObjectWrapper(_context, _global).setValue(field, value);
    });
}

void MozJSImplScope::gc() {
    _pendingGC.store(true);
    JS_RequestInterruptCallback(_runtime);
}

void MozJSImplScope::localConnectForDbEval(OperationContext* opCtx, const char* dbName) {

    _runSafely([this, &opCtx, &dbName] {
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

        _connectState = ConnectState::Local;
        _localDBName = dbName;

        loadStored(opCtx);

        // install db access functions in the global object
        installDBAccess();

        // install the Mongo function object and instantiate the 'db' global
        _mongoLocalProto.install(_global);
        execCoreFiles();

        const char* const makeMongo = "const _mongo = new Mongo()";
        exec(makeMongo, "local connect 2", false, true, true, 0);

        std::string makeDB = str::stream() << "const db = _mongo.getDB(\"" << dbName << "\");";
        exec(makeDB, "local connect 3", false, true, true, 0);
    });
}

void MozJSImplScope::externalSetup() {

    _runSafely([&] {
        if (_connectState == ConnectState::External)
            return;
        if (_connectState == ConnectState::Local)
            uasserted(12512, "localConnect already called, can't call externalSetup");

        // install db access functions in the global object
        installDBAccess();

        // install thread-related functions (e.g. _threadInject)
        installFork();

        // install the Mongo function object
        _mongoExternalProto.install(_global);
        execCoreFiles();
        _connectState = ConnectState::External;
    });
}

void MozJSImplScope::reset() {
    unregisterOperation();
    _pendingKill.store(false);
    _pendingGC.store(false);
    _requireOwnedObjects = false;
    advanceGeneration();
}

void MozJSImplScope::installBSONTypes() {
    _binDataProto.install(_global);
    _bsonProto.install(_global);
    _codeProto.install(_global);
    _dbPointerProto.install(_global);
    _dbRefProto.install(_global);
    _errorProto.install(_global);
    _maxKeyProto.install(_global);
    _minKeyProto.install(_global);
    _nativeFunctionProto.install(_global);
    _numberIntProto.install(_global);
    _numberLongProto.install(_global);
    _numberDecimalProto.install(_global);
    _objectProto.install(_global);
    _oidProto.install(_global);
    _regExpProto.install(_global);
    _timestampProto.install(_global);
    _uriProto.install(_global);

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
    _sessionProto.install(_global);
}

void MozJSImplScope::installFork() {
    _countDownLatchProto.install(_global);
    _jsThreadProto.install(_global);
}

bool MozJSImplScope::_checkErrorState(bool success, bool reportError, bool assertOnError) {
    if (success)
        return false;

    if (_status.isOK()) {
        JS::RootedValue excn(_context);
        if (JS_GetPendingException(_context, &excn) && excn.isObject()) {
            str::stream ss;

            auto stackStr = ObjectWrapper(_context, excn).getString(InternedString::stack);
            auto fnameStr = ObjectWrapper(_context, excn).getString(InternedString::fileName);
            auto lineNum = ObjectWrapper(_context, excn).getNumberInt(InternedString::lineNumber);
            auto colNum = ObjectWrapper(_context, excn).getNumberInt(InternedString::columnNumber);

            if (fnameStr != "") {
                ss << "[" << fnameStr << ":" << lineNum << ":" << colNum << "] ";
            }
            ss << ValueWriter(_context, excn).toString();
            if (stackStr != "") {
                ss << "\nStack trace:\n" << stackStr << "----------\n";
            }
            _status = Status(ErrorCodes::JSInterpreterFailure, ss);
        } else {
            _status = Status(ErrorCodes::UnknownError, "Unknown Failure from JSInterpreter");
        }
    }

    _error = _status.reason();

    if (reportError)
        error() << redact(_error);

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

auto MozJSImplScope::ASANHandles::getThreadASANHandles() -> ASANHandles* {
    return kCurrentASANHandles;
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

void MozJSImplScope::requireOwnedObjects() {
    _requireOwnedObjects = true;
}

bool MozJSImplScope::requiresOwnedObjects() const {
    return _requireOwnedObjects;
}

const std::string& MozJSImplScope::getParentStack() const {
    return _parentStack;
}

std::string MozJSImplScope::buildStackString() {
    JS::RootedObject stack(_context);

    if (!JS::CaptureCurrentStack(_context, &stack)) {
        return {};
    }

    JS::RootedString out(_context);
    if (JS::BuildStackString(_context, stack, &out)) {
        return JSStringWrapper(_context, out.get()).toString();
    } else {
        return {};
    }
}

}  // namespace mozjs
}  // namespace mongo
