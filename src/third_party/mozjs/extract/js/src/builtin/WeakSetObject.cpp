/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/WeakSetObject.h"

#include "jsapi.h"

#include "builtin/MapObject.h"
#include "vm/GlobalObject.h"
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/SelfHosting.h"

#include "builtin/WeakMapObject-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

MOZ_ALWAYS_INLINE bool
IsWeakSet(HandleValue v)
{
    return v.isObject() && v.toObject().is<WeakSetObject>();
}

// ES2018 draft rev 7a2d3f053ecc2336fc19f377c55d52d78b11b296
// 23.4.3.1 WeakSet.prototype.add ( value )
MOZ_ALWAYS_INLINE bool
WeakSet_add_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsWeakSet(args.thisv()));

    // Step 4.
    if (!args.get(0).isObject()) {
        ReportNotObjectWithName(cx, "WeakSet value", args.get(0));
        return false;
    }

    // Steps 5-7.
    RootedObject value(cx, &args[0].toObject());
    Rooted<WeakSetObject*> map(cx, &args.thisv().toObject().as<WeakSetObject>());
    if (!WeakCollectionPutEntryInternal(cx, map, value, TrueHandleValue))
        return false;

    // Steps 6.a.i, 8.
    args.rval().set(args.thisv());
    return true;
}

static bool
WeakSet_add(JSContext* cx, unsigned argc, Value* vp)
{
    // Steps 1-3.
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsWeakSet, WeakSet_add_impl>(cx, args);
}

// ES2018 draft rev 7a2d3f053ecc2336fc19f377c55d52d78b11b296
// 23.4.3.3 WeakSet.prototype.delete ( value )
MOZ_ALWAYS_INLINE bool
WeakSet_delete_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsWeakSet(args.thisv()));

    // Step 4.
    if (!args.get(0).isObject()) {
        args.rval().setBoolean(false);
        return true;
    }

    // Steps 5-6.
    if (ObjectValueMap* map = args.thisv().toObject().as<WeakSetObject>().getMap()) {
        JSObject* value = &args[0].toObject();
        if (ObjectValueMap::Ptr ptr = map->lookup(value)) {
            map->remove(ptr);
            args.rval().setBoolean(true);
            return true;
        }
    }

    // Step 7.
    args.rval().setBoolean(false);
    return true;
}

static bool
WeakSet_delete(JSContext* cx, unsigned argc, Value* vp)
{
    // Steps 1-3.
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsWeakSet, WeakSet_delete_impl>(cx, args);
}

// ES2018 draft rev 7a2d3f053ecc2336fc19f377c55d52d78b11b296
// 23.4.3.4 WeakSet.prototype.has ( value )
MOZ_ALWAYS_INLINE bool
WeakSet_has_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsWeakSet(args.thisv()));

    // Step 5.
    if (!args.get(0).isObject()) {
        args.rval().setBoolean(false);
        return true;
    }

    // Steps 4, 6.
    if (ObjectValueMap* map = args.thisv().toObject().as<WeakSetObject>().getMap()) {
        JSObject* value = &args[0].toObject();
        if (map->has(value)) {
            args.rval().setBoolean(true);
            return true;
        }
    }

    // Step 7.
    args.rval().setBoolean(false);
    return true;
}

static bool
WeakSet_has(JSContext* cx, unsigned argc, Value* vp)
{
    // Steps 1-3.
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsWeakSet, WeakSet_has_impl>(cx, args);
}

const Class WeakSetObject::class_ = {
    "WeakSet",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_CACHED_PROTO(JSProto_WeakSet) |
    JSCLASS_BACKGROUND_FINALIZE,
    &WeakCollectionObject::classOps_
};

const JSPropertySpec WeakSetObject::properties[] = {
    JS_PS_END
};

const JSFunctionSpec WeakSetObject::methods[] = {
    JS_FN("add",    WeakSet_add,    1, 0),
    JS_FN("delete", WeakSet_delete, 1, 0),
    JS_FN("has",    WeakSet_has,    1, 0),
    JS_FS_END
};

JSObject*
WeakSetObject::initClass(JSContext* cx, HandleObject obj)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    RootedPlainObject proto(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!proto)
        return nullptr;

    Rooted<JSFunction*> ctor(cx, GlobalObject::createConstructor(cx, construct,
                                                                 ClassName(JSProto_WeakSet, cx), 0));
    if (!ctor ||
        !LinkConstructorAndPrototype(cx, ctor, proto) ||
        !DefinePropertiesAndFunctions(cx, proto, properties, methods) ||
        !DefineToStringTag(cx, proto, cx->names().WeakSet) ||
        !GlobalObject::initBuiltinConstructor(cx, global, JSProto_WeakSet, ctor, proto))
    {
        return nullptr;
    }
    return proto;
}

WeakSetObject*
WeakSetObject::create(JSContext* cx, HandleObject proto /* = nullptr */)
{
    return NewObjectWithClassProto<WeakSetObject>(cx, proto);
}

bool
WeakSetObject::isBuiltinAdd(HandleValue add)
{
    return IsNativeFunction(add, WeakSet_add);
}

bool
WeakSetObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    // Based on our "Set" implementation instead of the more general ES6 steps.
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "WeakSet"))
        return false;

    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
        return false;

    Rooted<WeakSetObject*> obj(cx, WeakSetObject::create(cx, proto));
    if (!obj)
        return false;

    if (!args.get(0).isNullOrUndefined()) {
        RootedValue iterable(cx, args[0]);
        bool optimized = false;
        if (!IsOptimizableInitForSet<GlobalObject::getOrCreateWeakSetPrototype, isBuiltinAdd>(cx, obj, iterable, &optimized))
            return false;

        if (optimized) {
            RootedValue keyVal(cx);
            RootedObject keyObject(cx);
            RootedArrayObject array(cx, &iterable.toObject().as<ArrayObject>());
            for (uint32_t index = 0; index < array->getDenseInitializedLength(); ++index) {
                keyVal.set(array->getDenseElement(index));
                MOZ_ASSERT(!keyVal.isMagic(JS_ELEMENTS_HOLE));

                if (keyVal.isPrimitive()) {
                    ReportNotObjectWithName(cx, "WeakSet value", keyVal);
                    return false;
                }

                keyObject = &keyVal.toObject();
                if (!WeakCollectionPutEntryInternal(cx, obj, keyObject, TrueHandleValue))
                    return false;
            }
        } else {
            FixedInvokeArgs<1> args2(cx);
            args2[0].set(args[0]);

            RootedValue thisv(cx, ObjectValue(*obj));
            if (!CallSelfHostedFunction(cx, cx->names().WeakSetConstructorInit, thisv, args2, args2.rval()))
                return false;
        }
    }

    args.rval().setObject(*obj);
    return true;
}


JSObject*
js::InitWeakSetClass(JSContext* cx, HandleObject obj)
{
    return WeakSetObject::initClass(cx, obj);
}

JS_FRIEND_API(bool)
JS_NondeterministicGetWeakSetKeys(JSContext* cx, HandleObject objArg, MutableHandleObject ret)
{
    RootedObject obj(cx, UncheckedUnwrap(objArg));
    if (!obj || !obj->is<WeakSetObject>()) {
        ret.set(nullptr);
        return true;
    }
    return WeakCollectionObject::nondeterministicGetKeys(cx, obj.as<WeakCollectionObject>(), ret);
}
