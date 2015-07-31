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

#include "mongo/scripting/mozjs/jsthread.h"

#include <cstdio>

#include "mongo/db/jsobj.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec JSThreadInfo::threadMethods[6] = {
    MONGO_ATTACH_JS_FUNCTION(init),
    MONGO_ATTACH_JS_FUNCTION(start),
    MONGO_ATTACH_JS_FUNCTION(join),
    MONGO_ATTACH_JS_FUNCTION(hasFailed),
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
 * thread, have that thread do the work and join() it before checking it's
 * result (serialized through bson). We can check errors at any time by
 * checking a mutex guarded hasError().
 */
class JSThreadConfig {
public:
    JSThreadConfig(JSContext* cx, JS::CallArgs args)
        : _started(false), _done(false), _sharedData(new SharedData()) {
        uassert(ErrorCodes::JSInterpreterFailure, "need at least one argument", args.length() > 0);
        uassert(ErrorCodes::JSInterpreterFailure,
                "first argument must be a function",
                args.get(0).isObject() && JS_ObjectIsFunction(cx, args.get(0).toObjectOrNull()));

        BSONObjBuilder b;
        for (unsigned i = 0; i < args.length(); ++i) {
            // 10 decimal digits for a 32 bit unsigned, then 1 for the null
            char buf[11];
            std::sprintf(buf, "%i", i);

            ValueWriter(cx, args.get(i)).writeThis(&b, buf);
        }

        _sharedData->_args = b.obj();
    }

    void start() {
        uassert(ErrorCodes::JSInterpreterFailure, "Thread already started", !_started);

        _thread = stdx::thread(JSThread(*this));
        _started = true;
    }

    void join() {
        uassert(ErrorCodes::JSInterpreterFailure, "Thread not running", _started && !_done);

        _thread.join();
        _done = true;
    }

    /**
     * Returns true if the JSThread terminated as a result of an error
     * during its execution, and false otherwise. This operation does
     * not block, nor does it require join() to have been called.
     */
    bool hasFailed() const {
        uassert(ErrorCodes::JSInterpreterFailure, "Thread not started", _started);

        return _sharedData->getErrored();
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
        SharedData() : _errored(false) {}

        void setErrored(bool value) {
            stdx::lock_guard<stdx::mutex> lck(_erroredMutex);
            _errored = value;
        }

        bool getErrored() {
            stdx::lock_guard<stdx::mutex> lck(_erroredMutex);
            return _errored;
        }

        /**
         * These two members aren't protected in any way, so you have to be
         * mindful about how they're used. I.e. _args needs to be set before
         * start() and _returnData can't be touched until after join().
         */
        BSONObj _args;
        BSONObj _returnData;

    private:
        stdx::mutex _erroredMutex;
        bool _errored;
    };

    /**
     * The callable object used by stdx::thread
     */
    class JSThread {
    public:
        JSThread(JSThreadConfig& config) : _sharedData(config._sharedData) {}

        void operator()() {
            try {
                MozJSImplScope scope(static_cast<MozJSScriptEngine*>(globalScriptEngine));

                _sharedData->_returnData = scope.callThreadArgs(_sharedData->_args);
            } catch (...) {
                auto status = exceptionToStatus();

                log() << "js thread raised js exception: " << status.reason();
                _sharedData->setErrored(true);
                _sharedData->_returnData = BSON("ret" << BSONUndefined);
            }
        }

    private:
        std::shared_ptr<SharedData> _sharedData;
    };

    bool _started;
    bool _done;
    stdx::thread _thread;
    std::shared_ptr<SharedData> _sharedData;
};

namespace {

JSThreadConfig* getConfig(JSContext* cx, JS::CallArgs args) {
    JS::RootedValue value(cx);
    ObjectWrapper(cx, args.thisv()).getValue("_JSThreadConfig", &value);

    if (!value.isObject())
        uasserted(ErrorCodes::InternalError, "_JSThreadConfig not an object");

    return static_cast<JSThreadConfig*>(JS_GetPrivate(value.toObjectOrNull()));
}

}  // namespace

void JSThreadInfo::finalize(JSFreeOp* fop, JSObject* obj) {
    auto config = static_cast<JSThreadConfig*>(JS_GetPrivate(obj));

    if (!config)
        return;

    delete config;
}

void JSThreadInfo::Functions::init(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    JS::RootedObject obj(cx);
    scope->getJSThreadProto().newObject(&obj);
    JSThreadConfig* config = new JSThreadConfig(cx, args);
    JS_SetPrivate(obj, config);

    ObjectWrapper(cx, args.thisv()).setObject("_JSThreadConfig", obj);

    args.rval().setUndefined();
}

void JSThreadInfo::Functions::start(JSContext* cx, JS::CallArgs args) {
    getConfig(cx, args)->start();

    args.rval().setUndefined();
}

void JSThreadInfo::Functions::join(JSContext* cx, JS::CallArgs args) {
    getConfig(cx, args)->join();

    args.rval().setUndefined();
}

void JSThreadInfo::Functions::hasFailed(JSContext* cx, JS::CallArgs args) {
    args.rval().setBoolean(getConfig(cx, args)->hasFailed());
}

void JSThreadInfo::Functions::returnData(JSContext* cx, JS::CallArgs args) {
    ValueReader(cx, args.rval())
        .fromBSONElement(getConfig(cx, args)->returnData().firstElement(), true);
}

void JSThreadInfo::Functions::_threadInject(JSContext* cx, JS::CallArgs args) {
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

void JSThreadInfo::Functions::_scopedThreadInject(JSContext* cx, JS::CallArgs args) {
    _threadInject(cx, args);
}

}  // namespace mozjs
}  // namespace mongo
