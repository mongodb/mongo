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


#include "mongo/scripting/mozjs/jsthread.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/engine.h"
#include "mongo/scripting/mozjs/exception.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <jsapi.h>
#include <jsfriendapi.h>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <js/Array.h>
#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/PropertyAndElement.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>
#include <js/ValueArray.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace mozjs {

// These are all executed on some object that owns a js thread, rather than a
// jsthread itself, so CONSTRAINED_METHOD doesn't do the job here.
const JSFunctionSpec JSThreadInfo::threadMethods[7] = {
    MONGO_ATTACH_JS_FUNCTION(init),
    MONGO_ATTACH_JS_FUNCTION(start),
    MONGO_ATTACH_JS_FUNCTION(join),
    MONGO_ATTACH_JS_FUNCTION(hasFailed),
    MONGO_ATTACH_JS_FUNCTION(currentStatus),
    MONGO_ATTACH_JS_FUNCTION(returnData),
    JS_FS_END,
};

const JSFunctionSpec JSThreadInfo::freeFunctions[3] = {
    MONGO_ATTACH_JS_FUNCTION(_threadInject),
    MONGO_ATTACH_JS_FUNCTION(_scopedThreadInject),
    JS_FS_END,
};

const char* const JSThreadInfo::className = "JSThread";

/**
 * Holder for JSThreads as exposed by fork() in the shell.
 *
 * The idea here is that we create a jsthread by taking a js function and its
 * parameters and encoding them into a single bson object. Then we spawn a
 * thread, have that thread do the work and join() it before checking its
 * result (serialized through bson). We can check errors at any time by
 * checking a mutex guarded hasError().
 */
class JSThreadConfig {
public:
    JSThreadConfig(JSContext* cx, JS::CallArgs args)
        : _started(false),
          _done(false),
          _sharedData(std::make_shared<SharedData>()),
          _jsthread(*this) {
        auto scope = getScope(cx);

        uassert(ErrorCodes::JSInterpreterFailure, "need at least one argument", args.length() > 0);
        uassert(ErrorCodes::JSInterpreterFailure,
                "first argument must be a function",
                args.get(0).isObject() && js::IsFunctionObject(args.get(0).toObjectOrNull()));

        JS::RootedObject robj(cx, JS::NewArrayObject(cx, args));
        if (!robj) {
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS::NewArrayObject");
        }

        _sharedData->_args = ObjectWrapper(cx, robj).toBSON();

        _sharedData->_stack = currentJSStackToString(cx);

        if (!scope->getParentStack().empty()) {
            _sharedData->_stack = _sharedData->_stack + scope->getParentStack();
        }
    }

    void start() {
        uassert(ErrorCodes::JSInterpreterFailure, "Thread already started", !_started);

        _thread = stdx::thread(JSThread::run, &_jsthread);
        _started = true;
    }

    void join() {
        uassert(ErrorCodes::JSInterpreterFailure, "Thread not running", _started && !_done);

        _thread.join();
        _done = true;

        uassertStatusOK(_sharedData->getErrorStatus());
    }

    /**
     * Returns true if the JSThread terminated as a result of an error
     * during its execution, and false otherwise. This operation does
     * not wait for the thread to terminate, nor does it require join()
     * to have been called.
     */
    bool hasFailed() const {
        return !currentStatus().isOK();
    }

    /**
     * Returns the current error status of the thread, which may
     * change as it is running. This operation does not wait for the
     * thread to terminate, nor does it require join() to have been
     * called.
     */
    Status currentStatus() const {
        uassert(ErrorCodes::JSInterpreterFailure, "Thread not started", _started);

        return _sharedData->getErrorStatus();
    }

    BSONObj returnData() {
        if (!_done)
            join();

        return _sharedData->_returnData;
    }

private:
    /**
     * SharedData between the calling thread and the callee
     *
     * JSThreadConfig doesn't always outlive its JSThread (for example, if the parent thread
     * garbage collects the JSThreadConfig before the JSThread has finished running), so any
     * data shared between them has to go in a shared_ptr.
     */
    class SharedData {
    public:
        SharedData() = default;

        void setErrorStatus(Status status) {
            stdx::lock_guard<stdx::mutex> lck(_statusMutex);
            _status = std::move(status);
        }

        Status getErrorStatus() {
            stdx::lock_guard<stdx::mutex> lck(_statusMutex);
            return _status;
        }

        /**
         * These three members aren't protected in any way, so you have to be
         * mindful about how they're used. I.e. _args/_stack need to be set
         * before start() and _returnData can't be touched until after join().
         */
        BSONObj _args;
        BSONObj _returnData;
        std::string _stack;

    private:
        stdx::mutex _statusMutex;
        Status _status = Status::OK();
    };

    /**
     * The callable object used by stdx::thread
     */
    class JSThread {
    public:
        JSThread(JSThreadConfig& config) : _sharedData(config._sharedData) {}

        static void run(void* priv) {
            auto thisv = static_cast<JSThread*>(priv);

            try {
                MozJSImplScope scope(static_cast<MozJSScriptEngine*>(getGlobalScriptEngine()),
                                     boost::none /* Don't override global jsHeapLimitMB */);
                Client::initThread("js", getGlobalServiceContext()->getService());
                scope.setParentStack(thisv->_sharedData->_stack);
                thisv->_sharedData->_returnData = scope.callThreadArgs(thisv->_sharedData->_args);
            } catch (...) {
                auto status = exceptionToStatus();
                LOGV2_WARNING(4988200,
                              "JS Thread exiting after catching unhandled exception",
                              "error"_attr = redact(status));
                thisv->_sharedData->setErrorStatus(status);
                thisv->_sharedData->_returnData = BSON("ret" << BSONUndefined);
            }
        }

    private:
        std::shared_ptr<SharedData> _sharedData;
    };

    bool _started;
    bool _done;
    stdx::thread _thread;
    std::shared_ptr<SharedData> _sharedData;
    JSThread _jsthread;
};

namespace {

JSThreadConfig* getConfig(JSContext* cx, JS::CallArgs args) {
    JS::RootedValue value(cx);
    ObjectWrapper(cx, args.thisv()).getValue(InternedString::_JSThreadConfig, &value);

    if (!value.isObject())
        uasserted(ErrorCodes::BadValue, "_JSThreadConfig not an object");

    if (!getScope(cx)->getProto<JSThreadInfo>().instanceOf(value))
        uasserted(ErrorCodes::BadValue, "_JSThreadConfig is not a JSThread");

    return JS::GetMaybePtrFromReservedSlot<JSThreadConfig>(value.toObjectOrNull(),
                                                           JSThreadInfo::JSThreadConfigSlot);
}

}  // namespace

void JSThreadInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto config = JS::GetMaybePtrFromReservedSlot<JSThreadConfig>(obj, JSThreadConfigSlot);

    if (!config)
        return;

    getScope(gcCtx)->trackedDelete(config);
}

void JSThreadInfo::Functions::init::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    JS::RootedObject obj(cx);
    scope->getProto<JSThreadInfo>().newObject(&obj);
    JSThreadConfig* config = scope->trackedNew<JSThreadConfig>(cx, args);
    JS::SetReservedSlot(obj, JSThreadConfigSlot, JS::PrivateValue(config));

    ObjectWrapper(cx, args.thisv()).setObject(InternedString::_JSThreadConfig, obj);

    args.rval().setUndefined();
}

void JSThreadInfo::Functions::start::call(JSContext* cx, JS::CallArgs args) {
    getConfig(cx, args)->start();

    args.rval().setUndefined();
}

void JSThreadInfo::Functions::join::call(JSContext* cx, JS::CallArgs args) {
    getConfig(cx, args)->join();

    args.rval().setUndefined();
}

void JSThreadInfo::Functions::hasFailed::call(JSContext* cx, JS::CallArgs args) {
    args.rval().setBoolean(getConfig(cx, args)->hasFailed());
}

void JSThreadInfo::Functions::currentStatus::call(JSContext* cx, JS::CallArgs args) {
    BSONObjBuilder bob;
    getConfig(cx, args)->currentStatus().serialize(&bob);
    const BSONObj* parent = nullptr;
    const bool readOnly = true;
    ValueReader(cx, args.rval()).fromBSON(bob.obj(), parent, readOnly);
}

void JSThreadInfo::Functions::returnData::call(JSContext* cx, JS::CallArgs args) {
    auto obj = getConfig(cx, args)->returnData();
    ValueReader(cx, args.rval()).fromBSONElement(obj.firstElement(), obj, true);
}

void JSThreadInfo::Functions::_threadInject::call(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::JSInterpreterFailure,
            "threadInject takes exactly 1 argument",
            args.length() == 1);
    uassert(ErrorCodes::JSInterpreterFailure,
            "threadInject needs to be passed a prototype",
            args.get(0).isObject());

    JS::RootedObject o(cx, args.get(0).toObjectOrNull());

    if (!JS_DefineFunctions(cx, o, JSThreadInfo::threadMethods))
        throwCurrentJSException(cx, ErrorCodes::JSInterpreterFailure, "Failed to define functions");

    args.rval().setUndefined();
}

void JSThreadInfo::Functions::_scopedThreadInject::call(JSContext* cx, JS::CallArgs args) {
    _threadInject::call(cx, args);
}

}  // namespace mozjs
}  // namespace mongo
