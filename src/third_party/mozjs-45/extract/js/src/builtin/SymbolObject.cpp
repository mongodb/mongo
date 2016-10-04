/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/SymbolObject.h"

#include "vm/StringBuffer.h"
#include "vm/Symbol.h"

#include "jsobjinlines.h"

#include "vm/NativeObject-inl.h"

using JS::Symbol;
using namespace js;

const Class SymbolObject::class_ = {
    "Symbol",
    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) | JSCLASS_HAS_CACHED_PROTO(JSProto_Symbol)
};

SymbolObject*
SymbolObject::create(JSContext* cx, JS::HandleSymbol symbol)
{
    JSObject* obj = NewBuiltinClassInstance(cx, &class_);
    if (!obj)
        return nullptr;
    SymbolObject& symobj = obj->as<SymbolObject>();
    symobj.setPrimitiveValue(symbol);
    return &symobj;
}

const JSPropertySpec SymbolObject::properties[] = {
    JS_PS_END
};

const JSFunctionSpec SymbolObject::methods[] = {
    JS_FN(js_toString_str, toString, 0, 0),
    JS_FN(js_valueOf_str, valueOf, 0, 0),
    JS_SYM_FN(toPrimitive, toPrimitive, 1, JSPROP_READONLY),
    JS_FS_END
};

const JSFunctionSpec SymbolObject::staticMethods[] = {
    JS_FN("for", for_, 1, 0),
    JS_FN("keyFor", keyFor, 1, 0),
    JS_FS_END
};

JSObject*
SymbolObject::initClass(JSContext* cx, HandleObject obj)
{
    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());

    // This uses &JSObject::class_ because: "The Symbol prototype object is an
    // ordinary object. It is not a Symbol instance and does not have a
    // [[SymbolData]] internal slot." (ES6 rev 24, 19.4.3)
    RootedObject proto(cx, global->createBlankPrototype<PlainObject>(cx));
    if (!proto)
        return nullptr;

    RootedFunction ctor(cx, global->createConstructor(cx, construct,
                                                      ClassName(JSProto_Symbol, cx), 0));
    if (!ctor)
        return nullptr;

    // Define the well-known symbol properties, such as Symbol.iterator.
    ImmutablePropertyNamePtr* names = &cx->names().iterator;
    RootedValue value(cx);
    unsigned attrs = JSPROP_READONLY | JSPROP_PERMANENT;
    WellKnownSymbols* wks = cx->runtime()->wellKnownSymbols;
    for (size_t i = 0; i < JS::WellKnownSymbolLimit; i++) {
        value.setSymbol(wks->get(i));
        if (!NativeDefineProperty(cx, ctor, names[i], value, nullptr, nullptr, attrs))
            return nullptr;
    }

    if (!LinkConstructorAndPrototype(cx, ctor, proto) ||
        !DefinePropertiesAndFunctions(cx, proto, properties, methods) ||
        !DefinePropertiesAndFunctions(cx, ctor, nullptr, staticMethods) ||
        !GlobalObject::initBuiltinConstructor(cx, global, JSProto_Symbol, ctor, proto))
    {
        return nullptr;
    }
    return proto;
}

// ES6 rev 24 (2014 Apr 27) 19.4.1.1 and 19.4.1.2
bool
SymbolObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    // According to a note in the draft standard, "Symbol has ordinary
    // [[Construct]] behaviour but the definition of its @@create method causes
    // `new Symbol` to throw a TypeError exception." We do not support @@create
    // yet, so just throw a TypeError.
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.isConstructing()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_CONSTRUCTOR, "Symbol");
        return false;
    }

    // steps 1-3
    RootedString desc(cx);
    if (!args.get(0).isUndefined()) {
        desc = ToString(cx, args.get(0));
        if (!desc)
            return false;
    }

    // step 4
    RootedSymbol symbol(cx, JS::Symbol::new_(cx, JS::SymbolCode::UniqueSymbol, desc));
    if (!symbol)
        return false;
    args.rval().setSymbol(symbol);
    return true;
}

// ES6 rev 24 (2014 Apr 27) 19.4.2.2
bool
SymbolObject::for_(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // steps 1-2
    RootedString stringKey(cx, ToString(cx, args.get(0)));
    if (!stringKey)
        return false;

    // steps 3-7
    JS::Symbol* symbol = JS::Symbol::for_(cx, stringKey);
    if (!symbol)
        return false;
    args.rval().setSymbol(symbol);
    return true;
}

// ES6 rev 25 (2014 May 22) 19.4.2.7
bool
SymbolObject::keyFor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // step 1
    HandleValue arg = args.get(0);
    if (!arg.isSymbol()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK,
                              arg, nullptr, "not a symbol", nullptr);
        return false;
    }

    // step 2
    if (arg.toSymbol()->code() == JS::SymbolCode::InSymbolRegistry) {
#ifdef DEBUG
        RootedString desc(cx, arg.toSymbol()->description());
        MOZ_ASSERT(Symbol::for_(cx, desc) == arg.toSymbol());
#endif
        args.rval().setString(arg.toSymbol()->description());
        return true;
    }

    // step 3: omitted
    // step 4
    args.rval().setUndefined();
    return true;
}

MOZ_ALWAYS_INLINE bool
IsSymbol(HandleValue v)
{
    return v.isSymbol() || (v.isObject() && v.toObject().is<SymbolObject>());
}

// ES6 rev 27 (2014 Aug 24) 19.4.3.2
bool
SymbolObject::toString_impl(JSContext* cx, const CallArgs& args)
{
    // steps 1-3
    HandleValue thisv = args.thisv();
    MOZ_ASSERT(IsSymbol(thisv));
    Rooted<Symbol*> sym(cx, thisv.isSymbol()
                            ? thisv.toSymbol()
                            : thisv.toObject().as<SymbolObject>().unbox());

    // step 4
    return SymbolDescriptiveString(cx, sym, args.rval());
}

bool
SymbolObject::toString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsSymbol, toString_impl>(cx, args);
}

//ES6 rev 24 (2014 Apr 27) 19.4.3.3
bool
SymbolObject::valueOf_impl(JSContext* cx, const CallArgs& args)
{
    // Step 3, the error case, is handled by CallNonGenericMethod.
    HandleValue thisv = args.thisv();
    MOZ_ASSERT(IsSymbol(thisv));
    if (thisv.isSymbol())
        args.rval().set(thisv);
    else
        args.rval().setSymbol(thisv.toObject().as<SymbolObject>().unbox());
    return true;
}

bool
SymbolObject::valueOf(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsSymbol, valueOf_impl>(cx, args);
}

// ES6 19.4.3.4
bool
SymbolObject::toPrimitive(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // The specification gives exactly the same algorithm for @@toPrimitive as
    // for valueOf, so reuse the valueOf implementation.
    return CallNonGenericMethod<IsSymbol, valueOf_impl>(cx, args);
}

JSObject*
js::InitSymbolClass(JSContext* cx, HandleObject obj)
{
    return SymbolObject::initClass(cx, obj);
}
