/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS boolean implementation.
 */

#include "jsboolinlines.h"

#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsobj.h"
#include "jstypes.h"

#include "vm/GlobalObject.h"
#include "vm/ProxyObject.h"
#include "vm/StringBuffer.h"

#include "vm/BooleanObject-inl.h"

using namespace js;

const Class BooleanObject::class_ = {
    "Boolean",
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_HAS_CACHED_PROTO(JSProto_Boolean)
};

MOZ_ALWAYS_INLINE bool
IsBoolean(HandleValue v)
{
    return v.isBoolean() || (v.isObject() && v.toObject().is<BooleanObject>());
}

#if JS_HAS_TOSOURCE
MOZ_ALWAYS_INLINE bool
bool_toSource_impl(JSContext* cx, const CallArgs& args)
{
    HandleValue thisv = args.thisv();
    MOZ_ASSERT(IsBoolean(thisv));

    bool b = thisv.isBoolean() ? thisv.toBoolean() : thisv.toObject().as<BooleanObject>().unbox();

    StringBuffer sb(cx);
    if (!sb.append("(new Boolean(") || !BooleanToStringBuffer(b, sb) || !sb.append("))"))
        return false;

    JSString* str = sb.finishString();
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static bool
bool_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsBoolean, bool_toSource_impl>(cx, args);
}
#endif

MOZ_ALWAYS_INLINE bool
bool_toString_impl(JSContext* cx, const CallArgs& args)
{
    HandleValue thisv = args.thisv();
    MOZ_ASSERT(IsBoolean(thisv));

    bool b = thisv.isBoolean() ? thisv.toBoolean() : thisv.toObject().as<BooleanObject>().unbox();
    args.rval().setString(BooleanToString(cx, b));
    return true;
}

static bool
bool_toString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsBoolean, bool_toString_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
bool_valueOf_impl(JSContext* cx, const CallArgs& args)
{
    HandleValue thisv = args.thisv();
    MOZ_ASSERT(IsBoolean(thisv));

    bool b = thisv.isBoolean() ? thisv.toBoolean() : thisv.toObject().as<BooleanObject>().unbox();
    args.rval().setBoolean(b);
    return true;
}

static bool
bool_valueOf(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsBoolean, bool_valueOf_impl>(cx, args);
}

static const JSFunctionSpec boolean_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,  bool_toSource,  0, 0),
#endif
    JS_FN(js_toString_str,  bool_toString,  0, 0),
    JS_FN(js_valueOf_str,   bool_valueOf,   0, 0),
    JS_FS_END
};

static bool
Boolean(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool b = args.length() != 0 ? JS::ToBoolean(args[0]) : false;

    if (args.isConstructing()) {
        RootedObject newTarget (cx, &args.newTarget().toObject());
        RootedObject proto(cx);

        if (!GetPrototypeFromConstructor(cx, newTarget, &proto))
            return false;

        JSObject* obj = BooleanObject::create(cx, b, proto);
        if (!obj)
            return false;
        args.rval().setObject(*obj);
    } else {
        args.rval().setBoolean(b);
    }
    return true;
}

JSObject*
js::InitBooleanClass(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->isNative());

    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());

    Rooted<BooleanObject*> booleanProto(cx, global->createBlankPrototype<BooleanObject>(cx));
    if (!booleanProto)
        return nullptr;
    booleanProto->setFixedSlot(BooleanObject::PRIMITIVE_VALUE_SLOT, BooleanValue(false));

    RootedFunction ctor(cx, global->createConstructor(cx, Boolean, cx->names().Boolean, 1));
    if (!ctor)
        return nullptr;

    if (!LinkConstructorAndPrototype(cx, ctor, booleanProto))
        return nullptr;

    if (!DefinePropertiesAndFunctions(cx, booleanProto, nullptr, boolean_methods))
        return nullptr;

    if (!GlobalObject::initBuiltinConstructor(cx, global, JSProto_Boolean, ctor, booleanProto))
        return nullptr;

    return booleanProto;
}

JSString*
js::BooleanToString(ExclusiveContext* cx, bool b)
{
    return b ? cx->names().true_ : cx->names().false_;
}

JS_PUBLIC_API(bool)
js::ToBooleanSlow(HandleValue v)
{
    if (v.isString())
        return v.toString()->length() != 0;

    MOZ_ASSERT(v.isObject());
    return !EmulatesUndefined(&v.toObject());
}
