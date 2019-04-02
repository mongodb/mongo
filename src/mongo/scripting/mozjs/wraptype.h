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

#include <cstddef>
#include <jsapi.h>
#include <type_traits>

#include "mongo/scripting/mozjs/base.h"
#include "mongo/scripting/mozjs/exception.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/util/assert_util.h"

// The purpose of this class is to take in specially crafted types and generate
// a wrapper which installs the type, along with any useful life cycle methods
// and free functions that might be associated with it. The template magic in
// here, along with some useful macros, hides a lot of the implementation
// complexity of exposing C++ code into javascript. Most prominently, we have
// to wrap every function that can be called from javascript to prevent any C++
// exceptions from leaking out. We do this, with template and macro based
// codegen, and turn mongo exceptions into instances of Status, then convert
// those into javascript exceptions before returning. That allows all consumers
// of this library to throw exceptions freely, with the understanding that
// they'll be visible in javascript. Javascript exceptions are trapped at the
// top level and converted back to mongo exceptions by an error handler on
// ImplScope.

// MONGO_*_JS_FUNCTION_* macros are public and allow wrapped types to install
// their own functions on types and into the global scope
#define MONGO_DECLARE_JS_FUNCTION(function)                 \
    struct function {                                       \
        static const char* name() {                         \
            return #function;                               \
        }                                                   \
        static void call(JSContext* cx, JS::CallArgs args); \
    };

#define MONGO_ATTACH_JS_FUNCTION_WITH_FLAGS(name, flags) \
    JS_FN(#name, smUtils::wrapFunction<Functions::name>, 0, flags)

#define MONGO_ATTACH_JS_FUNCTION(name) MONGO_ATTACH_JS_FUNCTION_WITH_FLAGS(name, 0)

#define MONGO_ATTACH_JS_CONSTRAINED_METHOD(name, ...)                                              \
    {                                                                                              \
        #name, {smUtils::wrapConstrainedMethod < Functions::name, false, __VA_ARGS__ >, nullptr }, \
                0,                                                                                 \
                0,                                                                                 \
                nullptr                                                                            \
    }

#define MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(name, ...)                                    \
    {                                                                                             \
        #name, {smUtils::wrapConstrainedMethod < Functions::name, true, __VA_ARGS__ >, nullptr }, \
                0,                                                                                \
                0,                                                                                \
                nullptr                                                                           \
    }

namespace mongo {
namespace mozjs {

namespace smUtils {

template <typename T>
bool wrapFunction(JSContext* cx, unsigned argc, JS::Value* vp) {
    try {
        JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
        T::call(cx, args);
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
}

// Now all the spidermonkey type methods
template <typename T>
bool addProperty(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue v) {
    try {
        T::addProperty(cx, obj, id, v);
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
};

template <typename T>
bool call(JSContext* cx, unsigned argc, JS::Value* vp) {
    try {
        T::call(cx, JS::CallArgsFromVp(argc, vp));
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
};

template <typename T>
bool construct(JSContext* cx, unsigned argc, JS::Value* vp) {
    try {
        T::construct(cx, JS::CallArgsFromVp(argc, vp));
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
};

template <typename T>
bool delProperty(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::ObjectOpResult& result) {
    try {
        T::delProperty(cx, obj, id, result);
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
};

template <typename T>
bool enumerate(JSContext* cx,
               JS::HandleObject obj,
               JS::AutoIdVector& properties,
               bool enumerableOnly) {
    try {
        T::enumerate(cx, obj, properties, enumerableOnly);
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
};

template <typename T>
bool getProperty(JSContext* cx,
                 JS::HandleObject obj,
                 JS::HandleValue receiver,
                 JS::HandleId id,
                 JS::MutableHandleValue vp) {
    if (JSID_IS_SYMBOL(id)) {
        // Just default to the SpiderMonkey's standard implementations for Symbol methods
        vp.setUndefined();
        return true;
    }

    try {
        T::getProperty(cx, obj, id, receiver, vp);
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
};

template <typename T>
bool hasInstance(JSContext* cx, JS::HandleObject obj, JS::MutableHandleValue vp, bool* bp) {
    try {
        T::hasInstance(cx, obj, vp, bp);
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
};

template <typename T>
bool setProperty(JSContext* cx,
                 JS::HandleObject obj,
                 JS::HandleId id,
                 JS::HandleValue vp,
                 JS::HandleValue receiver,
                 JS::ObjectOpResult& result) {
    try {
        T::setProperty(cx, obj, id, vp, receiver, result);
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
};

template <typename T>
bool resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp) {
    if (JSID_IS_SYMBOL(id)) {
        // Just default to the SpiderMonkey's standard implementations for Symbol methods
        *resolvedp = false;
        return true;
    }

    try {
        T::resolve(cx, obj, id, resolvedp);
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
};

template <typename T>
void trace(JSTracer* trc, JSObject* obj) {
    try {
        T::trace(trc, obj);
    } catch (...) {
        invariant(false);
    }
};

}  // namespace smUtils

template <typename T>
class WrapType : public T {
public:
    WrapType(JSContext* context)
        : _context(context),
          _proto(),
          _constructor(),
          _jsclass({T::className,
                    T::classFlags,
                    &_jsclassOps,
                    JS_NULL_CLASS_SPEC,
                    JS_NULL_CLASS_EXT,
                    &_jsoOps}),
          _jsclassOps({T::addProperty != BaseInfo::addProperty ? smUtils::addProperty<T> : nullptr,
                       T::delProperty != BaseInfo::delProperty ? smUtils::delProperty<T> : nullptr,
                       nullptr,  // enumerate
                       T::enumerate != BaseInfo::enumerate ? smUtils::enumerate<T>
                                                           : nullptr,  // newEnumerate
                       T::resolve != BaseInfo::resolve ? smUtils::resolve<T> : nullptr,
                       T::mayResolve != BaseInfo::mayResolve ? T::mayResolve : nullptr,
                       T::finalize != BaseInfo::finalize ? T::finalize : nullptr,
                       T::call != BaseInfo::call ? smUtils::call<T> : nullptr,
                       T::hasInstance != BaseInfo::hasInstance ? smUtils::hasInstance<T> : nullptr,
                       T::construct != BaseInfo::construct ? smUtils::construct<T> : nullptr,
                       T::trace != BaseInfo::trace ? smUtils::trace<T> : nullptr}),
          _jsoOps({
              nullptr,  // lookupProperty
              nullptr,  // defineProperty
              nullptr,  // hasProperty
              T::getProperty != BaseInfo::getProperty ? smUtils::getProperty<T>
                                                      : nullptr,  // getProperty
              T::setProperty != BaseInfo::setProperty ? smUtils::setProperty<T>
                                                      : nullptr,  // setProperty
              nullptr,                                            // getOwnPropertyDescriptor
              nullptr,                                            // deleteProperty
              nullptr,                                            // getElements
              nullptr                                             // funToString
          }) {

        // The global object is different.  We need it for basic setup
        // before the other types are installed.  Might as well just do it
        // in the constructor.
        if (T::classFlags & JSCLASS_GLOBAL_FLAGS) {
            _jsclassOps.trace = JS_GlobalObjectTraceHook;

            JS::RootedObject proto(_context);

            JSAutoRequest ar(_context);

            JS::CompartmentOptions options;
            _proto.init(_context,
                        _assertPtr(JS_NewGlobalObject(_context,
                                                      js::Jsvalify(&_jsclass),
                                                      nullptr,
                                                      JS::DontFireOnNewGlobalHook,
                                                      options)));

            JSAutoCompartment ac(_context, _proto);
            _installFunctions(_proto, T::freeFunctions);
        }
    }

    ~WrapType() {
        // Persistent globals don't RAII, you have to reset() them manually
        _proto.reset();
        _constructor.reset();
    }

    void install(JS::HandleObject global) {
        switch (static_cast<InstallType>(T::installType)) {
            case InstallType::Global:
                _installGlobal(global);
                break;
            case InstallType::Private:
                _installPrivate(global);
                break;
            case InstallType::OverNative:
                _installOverNative(global);
                break;
        }
    }

    /**
     * newObject methods don't invoke the constructor.  So they're good for
     * types without a constructor or inside the constructor
     */
    void newObject(JS::MutableHandleObject out) {
        out.set(_assertPtr(JS_NewObjectWithGivenProto(_context, js::Jsvalify(&_jsclass), _proto)));
    }

    void newObject(JS::MutableHandleValue out) {
        JS::RootedObject obj(_context);
        newObject(&obj);

        out.setObjectOrNull(obj);
    }

    void newObjectWithProto(JS::MutableHandleObject out, JS::HandleObject proto) {
        out.set(_assertPtr(JS_NewObjectWithGivenProto(_context, js::Jsvalify(&_jsclass), proto)));
    }

    void newObjectWithProto(JS::MutableHandleValue out, JS::HandleObject proto) {
        JS::RootedObject obj(_context);
        newObjectWithProto(&obj, proto);

        out.setObjectOrNull(obj);
    }

    /**
     * newInstance calls the constructor, a la new Type() in js
     */
    void newInstance(JS::MutableHandleObject out) {
        dassert(T::installType == InstallType::OverNative || T::construct != BaseInfo::construct);

        JS::AutoValueVector args(_context);

        newInstance(args, out);
    }

    void newInstance(const JS::HandleValueArray& args, JS::MutableHandleObject out) {
        dassert(T::installType == InstallType::OverNative || T::construct != BaseInfo::construct);

        out.set(_assertPtr(JS_New(
            _context, T::installType == InstallType::OverNative ? _constructor : _proto, args)));
    }

    void newInstance(JS::MutableHandleValue out) {
        dassert(T::installType == InstallType::OverNative || T::construct != BaseInfo::construct);

        JS::AutoValueVector args(_context);

        newInstance(args, out);
    }

    void newInstance(const JS::HandleValueArray& args, JS::MutableHandleValue out) {
        dassert(T::installType == InstallType::OverNative || T::construct != BaseInfo::construct);

        out.setObjectOrNull(_assertPtr(JS_New(
            _context, T::installType == InstallType::OverNative ? _constructor : _proto, args)));
    }

    // instanceOf doesn't go up the prototype tree.  It's a lower level more specific match
    bool instanceOf(JS::HandleObject obj) {
        return JS_InstanceOf(_context, obj, js::Jsvalify(&_jsclass), nullptr);
    }

    bool instanceOf(JS::HandleValue value) {
        if (!value.isObject())
            return false;

        JS::RootedObject obj(_context, value.toObjectOrNull());

        return instanceOf(obj);
    }

    const JSClass* getJSClass() const {
        return js::Jsvalify(&_jsclass);
    }

    JS::HandleObject getProto() const {
        return _proto;
    }

    JS::HandleObject getCtor() const {
        return _constructor;
    }

private:
    /**
     * Use this if you want your types installed visibly in the global scope
     */
    void _installGlobal(JS::HandleObject global) {
        JS::RootedObject parent(_context);
        _inheritFrom(T::inheritFrom, global, &parent);

        _proto.init(_context,
                    _assertPtr(JS_InitClass(
                        _context,
                        global,
                        parent,
                        js::Jsvalify(&_jsclass),
                        T::construct != BaseInfo::construct ? smUtils::construct<T> : nullptr,
                        0,
                        nullptr,
                        T::methods,
                        nullptr,
                        nullptr)));

        _installFunctions(global, T::freeFunctions);
        _postInstall(global, T::postInstall);
    }

    // Use this if you want your types installed, but not visible in the
    // global scope
    void _installPrivate(JS::HandleObject global) {
        dassert(T::construct == BaseInfo::construct);

        JS::RootedObject parent(_context);
        _inheritFrom(T::inheritFrom, global, &parent);

        // See newObject() for why we have to do this dance with the explicit
        // SetPrototype
        _proto.init(_context, _assertPtr(JS_NewObject(_context, js::Jsvalify(&_jsclass))));
        if (parent.get() && !JS_SetPrototype(_context, _proto, parent))
            throwCurrentJSException(
                _context, ErrorCodes::JSInterpreterFailure, "Failed to set prototype");

        _installFunctions(_proto, T::methods);
        _installFunctions(global, T::freeFunctions);

        _installConstructor(T::construct != BaseInfo::construct ? smUtils::construct<T> : nullptr);

        _postInstall(global, T::postInstall);
    }

    // Use this to attach things to types that we don't provide like
    // Object, or Array
    void _installOverNative(JS::HandleObject global) {
        dassert(T::addProperty == BaseInfo::addProperty);
        dassert(T::call == BaseInfo::call);
        dassert(T::construct == BaseInfo::construct);
        dassert(T::delProperty == BaseInfo::delProperty);
        dassert(T::enumerate == BaseInfo::enumerate);
        dassert(T::finalize == BaseInfo::finalize);
        dassert(T::getProperty == BaseInfo::getProperty);
        dassert(T::hasInstance == BaseInfo::hasInstance);
        dassert(T::resolve == BaseInfo::resolve);
        dassert(T::setProperty == BaseInfo::setProperty);

        JS::RootedValue value(_context);
        if (!JS_GetProperty(_context, global, T::className, &value))
            throwCurrentJSException(
                _context, ErrorCodes::JSInterpreterFailure, "Couldn't get className property");

        if (!value.isObject())
            uasserted(ErrorCodes::BadValue, "className isn't object");

        JS::RootedObject classNameObject(_context);
        if (!JS_ValueToObject(_context, value, &classNameObject))
            throwCurrentJSException(_context,
                                    ErrorCodes::JSInterpreterFailure,
                                    "Couldn't convert className property into an object.");

        JS::RootedValue protoValue(_context);
        if (!JS_GetPropertyById(_context,
                                classNameObject,
                                InternedStringId(_context, InternedString::prototype),
                                &protoValue))
            throwCurrentJSException(
                _context, ErrorCodes::JSInterpreterFailure, "Couldn't get className prototype");

        if (!protoValue.isObject())
            uasserted(ErrorCodes::BadValue, "className's prototype isn't object");

        _constructor.init(_context, value.toObjectOrNull());
        _proto.init(_context, protoValue.toObjectOrNull());

        _installFunctions(_proto, T::methods);
        _installFunctions(global, T::freeFunctions);
        _postInstall(global, T::postInstall);
    }

    void _installFunctions(JS::HandleObject global, const JSFunctionSpec* fs) {
        if (!fs)
            return;
        if (JS_DefineFunctions(_context, global, fs))
            return;

        throwCurrentJSException(
            _context, ErrorCodes::JSInterpreterFailure, "Failed to define functions");
    }

    // This is for inheriting from something other than Object
    void _inheritFrom(const char* name, JS::HandleObject global, JS::MutableHandleObject out) {
        if (!name)
            return;

        JS::RootedValue val(_context);

        if (!JS_GetProperty(_context, global, name, &val)) {
            throwCurrentJSException(
                _context, ErrorCodes::JSInterpreterFailure, "Failed to get parent");
        }

        if (!val.isObject()) {
            uasserted(ErrorCodes::JSInterpreterFailure, "Parent is not an object");
        }

        out.set(val.toObjectOrNull());
    }

    using postInstallT = void (*)(JSContext*, JS::HandleObject, JS::HandleObject);
    void _postInstall(JS::HandleObject global, postInstallT postInstall) {
        if (!postInstall)
            return;

        postInstall(_context, global, _proto);
    }

    void _installConstructor(JSNative ctor) {
        if (!ctor)
            return;

        auto ptr = JS_NewFunction(_context, ctor, 0, JSFUN_CONSTRUCTOR, nullptr);
        if (!ptr) {
            throwCurrentJSException(
                _context, ErrorCodes::JSInterpreterFailure, "Failed to install constructor");
        }

        JS::RootedObject ctorObj(_context, JS_GetFunctionObject(ptr));

        if (!JS_LinkConstructorAndPrototype(_context, ctorObj, _proto))
            throwCurrentJSException(_context,
                                    ErrorCodes::JSInterpreterFailure,
                                    "Failed to link constructor and prototype");
    }

    JSObject* _assertPtr(JSObject* ptr) {
        if (!ptr)
            throwCurrentJSException(
                _context, ErrorCodes::JSInterpreterFailure, "Failed to JS_NewX");

        return ptr;
    }

    JSContext* _context;
    JS::PersistentRootedObject _proto;
    JS::PersistentRootedObject _constructor;
    js::Class _jsclass;
    js::ClassOps _jsclassOps;
    js::ObjectOps _jsoOps;
};

}  // namespace mozjs
}  // namespace mongo
