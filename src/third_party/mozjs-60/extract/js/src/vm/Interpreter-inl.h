/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Interpreter_inl_h
#define vm_Interpreter_inl_h

#include "vm/Interpreter.h"

#include "jsnum.h"

#include "builtin/String.h"
#include "jit/Ion.h"
#include "vm/ArgumentsObject.h"
#include "vm/JSCompartment.h"

#include "vm/EnvironmentObject-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Stack-inl.h"
#include "vm/StringType-inl.h"
#include "vm/UnboxedObject-inl.h"

namespace js {

/*
 * Every possible consumer of MagicValue(JS_OPTIMIZED_ARGUMENTS) (as determined
 * by ScriptAnalysis::needsArgsObj) must check for these magic values and, when
 * one is received, act as if the value were the function's ArgumentsObject.
 * Additionally, it is possible that, after 'arguments' was copied into a
 * temporary, the arguments object has been created a some other failed guard
 * that called JSScript::argumentsOptimizationFailed. In this case, it is
 * always valid (and necessary) to replace JS_OPTIMIZED_ARGUMENTS with the real
 * arguments object.
 */
static inline bool
IsOptimizedArguments(AbstractFramePtr frame, MutableHandleValue vp)
{
    if (vp.isMagic(JS_OPTIMIZED_ARGUMENTS) && frame.script()->needsArgsObj())
        vp.setObject(frame.argsObj());
    return vp.isMagic(JS_OPTIMIZED_ARGUMENTS);
}

/*
 * One optimized consumer of MagicValue(JS_OPTIMIZED_ARGUMENTS) is f.apply.
 * However, this speculation must be guarded before calling 'apply' in case it
 * is not the builtin Function.prototype.apply.
 */
static inline bool
GuardFunApplyArgumentsOptimization(JSContext* cx, AbstractFramePtr frame, CallArgs& args)
{
    if (args.length() == 2 && IsOptimizedArguments(frame, args[1])) {
        if (!IsNativeFunction(args.calleev(), js::fun_apply)) {
            RootedScript script(cx, frame.script());
            if (!JSScript::argumentsOptimizationFailed(cx, script))
                return false;
            args[1].setObject(frame.argsObj());
        }
    }

    return true;
}

/*
 * Per ES6, lexical declarations may not be accessed in any fashion until they
 * are initialized (i.e., until the actual declaring statement is
 * executed). The various LEXICAL opcodes need to check if the slot is an
 * uninitialized let declaration, represented by the magic value
 * JS_UNINITIALIZED_LEXICAL.
 */
static inline bool
IsUninitializedLexical(const Value& val)
{
    // Use whyMagic here because JS_OPTIMIZED_ARGUMENTS could flow into here.
    return val.isMagic() && val.whyMagic() == JS_UNINITIALIZED_LEXICAL;
}

static inline bool
IsUninitializedLexicalSlot(HandleObject obj, Handle<PropertyResult> prop)
{
    MOZ_ASSERT(prop);
    if (obj->is<WithEnvironmentObject>())
        return false;

    // Proxy hooks may return a non-native property.
    if (prop.isNonNativeProperty())
        return false;

    Shape* shape = prop.shape();
    if (!shape->isDataProperty())
        return false;

    MOZ_ASSERT(obj->as<NativeObject>().containsPure(shape));
    return IsUninitializedLexical(obj->as<NativeObject>().getSlot(shape->slot()));
}

static inline void
ReportUninitializedLexical(JSContext* cx, HandlePropertyName name)
{
    ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, name);
}

static inline void
ReportUninitializedLexical(JSContext* cx, HandleScript script, jsbytecode* pc)
{
    ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, script, pc);
}

static inline bool
CheckUninitializedLexical(JSContext* cx, PropertyName* name_, HandleValue val)
{
    if (IsUninitializedLexical(val)) {
        RootedPropertyName name(cx, name_);
        ReportUninitializedLexical(cx, name);
        return false;
    }
    return true;
}

static inline bool
CheckUninitializedLexical(JSContext* cx, HandleScript script, jsbytecode* pc, HandleValue val)
{
    if (IsUninitializedLexical(val)) {
        ReportUninitializedLexical(cx, script, pc);
        return false;
    }
    return true;
}

static inline void
ReportRuntimeConstAssignment(JSContext* cx, HandlePropertyName name)
{
    ReportRuntimeLexicalError(cx, JSMSG_BAD_CONST_ASSIGN, name);
}

static inline void
ReportRuntimeConstAssignment(JSContext* cx, HandleScript script, jsbytecode* pc)
{
    ReportRuntimeLexicalError(cx, JSMSG_BAD_CONST_ASSIGN, script, pc);
}

inline bool
GetLengthProperty(const Value& lval, MutableHandleValue vp)
{
    /* Optimize length accesses on strings, arrays, and arguments. */
    if (lval.isString()) {
        vp.setInt32(lval.toString()->length());
        return true;
    }
    if (lval.isObject()) {
        JSObject* obj = &lval.toObject();
        if (obj->is<ArrayObject>()) {
            vp.setNumber(obj->as<ArrayObject>().length());
            return true;
        }

        if (obj->is<ArgumentsObject>()) {
            ArgumentsObject* argsobj = &obj->as<ArgumentsObject>();
            if (!argsobj->hasOverriddenLength()) {
                uint32_t length = argsobj->initialLength();
                MOZ_ASSERT(length < INT32_MAX);
                vp.setInt32(int32_t(length));
                return true;
            }
        }
    }

    return false;
}

enum class GetNameMode { Normal, TypeOf };

template <GetNameMode mode>
inline bool
FetchName(JSContext* cx, HandleObject receiver, HandleObject holder, HandlePropertyName name,
          Handle<PropertyResult> prop, MutableHandleValue vp)
{
    if (!prop) {
        switch (mode) {
          case GetNameMode::Normal:
            return ReportIsNotDefined(cx, name);
          case GetNameMode::TypeOf:
            vp.setUndefined();
            return true;
        }
    }

    /* Take the slow path if shape was not found in a native object. */
    if (!receiver->isNative() || !holder->isNative()) {
        Rooted<jsid> id(cx, NameToId(name));
        if (!GetProperty(cx, receiver, receiver, id, vp))
            return false;
    } else {
        RootedShape shape(cx, prop.shape());
        if (shape->isDataDescriptor() && shape->hasDefaultGetter()) {
            /* Fast path for Object instance properties. */
            MOZ_ASSERT(shape->isDataProperty());
            vp.set(holder->as<NativeObject>().getSlot(shape->slot()));
        } else {
            // Unwrap 'with' environments for reasons given in
            // GetNameBoundInEnvironment.
            RootedObject normalized(cx, MaybeUnwrapWithEnvironment(receiver));
            if (!NativeGetExistingProperty(cx, normalized, holder.as<NativeObject>(), shape, vp))
                return false;
        }
    }

    // We do our own explicit checking for |this|
    if (name == cx->names().dotThis)
        return true;

    // NAME operations are the slow paths already, so unconditionally check
    // for uninitialized lets.
    return CheckUninitializedLexical(cx, name, vp);
}

inline bool
FetchNameNoGC(JSObject* pobj, PropertyResult prop, MutableHandleValue vp)
{
    if (!prop || !pobj->isNative())
        return false;

    Shape* shape = prop.shape();
    if (!shape->isDataDescriptor() || !shape->hasDefaultGetter())
        return false;

    vp.set(pobj->as<NativeObject>().getSlot(shape->slot()));
    return !IsUninitializedLexical(vp);
}

template <js::GetNameMode mode>
inline bool
GetEnvironmentName(JSContext* cx, HandleObject envChain, HandlePropertyName name,
                   MutableHandleValue vp)
{
    {
        PropertyResult prop;
        JSObject* obj = nullptr;
        JSObject* pobj = nullptr;
        if (LookupNameNoGC(cx, name, envChain, &obj, &pobj, &prop)) {
            if (FetchNameNoGC(pobj, prop, vp))
                return true;
        }
    }

    Rooted<PropertyResult> prop(cx);
    RootedObject obj(cx), pobj(cx);
    if (!LookupName(cx, name, envChain, &obj, &pobj, &prop))
        return false;

    return FetchName<mode>(cx, obj, pobj, name, prop, vp);
}

inline bool
HasOwnProperty(JSContext* cx, HandleValue val, HandleValue idValue, bool* result)
{

    // As an optimization, provide a fast path when rooting is not necessary and
    // we can safely retrieve the object's shape.
    jsid id;
    if (val.isObject() && ValueToId<NoGC>(cx, idValue, &id)) {
        JSObject* obj = &val.toObject();
        PropertyResult prop;
        if (obj->isNative() &&
            NativeLookupOwnProperty<NoGC>(cx, &obj->as<NativeObject>(), id, &prop))
        {
            *result = prop.isFound();
            return true;
        }
    }

    // Step 1.
    RootedId key(cx);
    if (!ToPropertyKey(cx, idValue, &key))
        return false;

    // Step 2.
    RootedObject obj(cx, ToObject(cx, val));
    if (!obj)
        return false;

    // Step 3.
    return HasOwnProperty(cx, obj, key, result);
}


inline bool
GetIntrinsicOperation(JSContext* cx, jsbytecode* pc, MutableHandleValue vp)
{
    RootedPropertyName name(cx, cx->currentScript()->getName(pc));
    return GlobalObject::getIntrinsicValue(cx, cx->global(), name, vp);
}

inline bool
SetIntrinsicOperation(JSContext* cx, JSScript* script, jsbytecode* pc, HandleValue val)
{
    RootedPropertyName name(cx, script->getName(pc));
    return GlobalObject::setIntrinsicValue(cx, cx->global(), name, val);
}

inline void
SetAliasedVarOperation(JSContext* cx, JSScript* script, jsbytecode* pc,
                       EnvironmentObject& obj, EnvironmentCoordinate ec, const Value& val,
                       MaybeCheckTDZ checkTDZ)
{
    MOZ_ASSERT_IF(checkTDZ, !IsUninitializedLexical(obj.aliasedBinding(ec)));

    // Avoid computing the name if no type updates are needed, as this may be
    // expensive on scopes with large numbers of variables.
    PropertyName* name = obj.isSingleton()
                         ? EnvironmentCoordinateName(cx->caches().envCoordinateNameCache, script, pc)
                         : nullptr;

    obj.setAliasedBinding(cx, ec, name, val);
}

inline bool
SetNameOperation(JSContext* cx, JSScript* script, jsbytecode* pc, HandleObject env,
                 HandleValue val)
{
    MOZ_ASSERT(*pc == JSOP_SETNAME ||
               *pc == JSOP_STRICTSETNAME ||
               *pc == JSOP_SETGNAME ||
               *pc == JSOP_STRICTSETGNAME);
    MOZ_ASSERT_IF((*pc == JSOP_SETGNAME || *pc == JSOP_STRICTSETGNAME) &&
                  !script->hasNonSyntacticScope(),
                  env == cx->global() ||
                  env == &cx->global()->lexicalEnvironment() ||
                  env->is<RuntimeLexicalErrorObject>());

    bool strict = *pc == JSOP_STRICTSETNAME || *pc == JSOP_STRICTSETGNAME;
    RootedPropertyName name(cx, script->getName(pc));

    // In strict mode, assigning to an undeclared global variable is an
    // error. To detect this, we call NativeSetProperty directly and pass
    // Unqualified. It stores the error, if any, in |result|.
    bool ok;
    ObjectOpResult result;
    RootedId id(cx, NameToId(name));
    RootedValue receiver(cx, ObjectValue(*env));
    if (env->isUnqualifiedVarObj()) {
        RootedNativeObject varobj(cx);
        if (env->is<DebugEnvironmentProxy>())
            varobj = &env->as<DebugEnvironmentProxy>().environment().as<NativeObject>();
        else
            varobj = &env->as<NativeObject>();
        MOZ_ASSERT(!varobj->getOpsSetProperty());
        ok = NativeSetProperty<Unqualified>(cx, varobj, id, val, receiver, result);
    } else {
        ok = SetProperty(cx, env, id, val, receiver, result);
    }
    return ok && result.checkStrictErrorOrWarning(cx, env, id, strict);
}

inline bool
DefLexicalOperation(JSContext* cx, Handle<LexicalEnvironmentObject*> lexicalEnv,
                    HandleObject varObj, HandlePropertyName name, unsigned attrs)
{
    // Redeclaration checks should have already been done.
    MOZ_ASSERT(CheckLexicalNameConflict(cx, lexicalEnv, varObj, name));
    RootedId id(cx, NameToId(name));
    RootedValue uninitialized(cx, MagicValue(JS_UNINITIALIZED_LEXICAL));
    return NativeDefineDataProperty(cx, lexicalEnv, id, uninitialized, attrs);
}

inline bool
DefLexicalOperation(JSContext* cx, LexicalEnvironmentObject* lexicalEnvArg,
                    JSObject* varObjArg, JSScript* script, jsbytecode* pc)
{
    MOZ_ASSERT(*pc == JSOP_DEFLET || *pc == JSOP_DEFCONST);
    RootedPropertyName name(cx, script->getName(pc));

    unsigned attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT;
    if (*pc == JSOP_DEFCONST)
        attrs |= JSPROP_READONLY;

    Rooted<LexicalEnvironmentObject*> lexicalEnv(cx, lexicalEnvArg);
    RootedObject varObj(cx, varObjArg);
    MOZ_ASSERT_IF(!script->hasNonSyntacticScope(),
                  lexicalEnv == &cx->global()->lexicalEnvironment() && varObj == cx->global());

    return DefLexicalOperation(cx, lexicalEnv, varObj, name, attrs);
}

inline void
InitGlobalLexicalOperation(JSContext* cx, LexicalEnvironmentObject* lexicalEnvArg,
                           JSScript* script, jsbytecode* pc, HandleValue value)
{
    MOZ_ASSERT_IF(!script->hasNonSyntacticScope(),
                  lexicalEnvArg == &cx->global()->lexicalEnvironment());
    MOZ_ASSERT(*pc == JSOP_INITGLEXICAL);
    Rooted<LexicalEnvironmentObject*> lexicalEnv(cx, lexicalEnvArg);
    RootedShape shape(cx, lexicalEnv->lookup(cx, script->getName(pc)));
    MOZ_ASSERT(shape);
    lexicalEnv->setSlotWithType(cx, shape, value);
}

inline bool
InitPropertyOperation(JSContext* cx, JSOp op, HandleObject obj, HandlePropertyName name,
                      HandleValue rhs)
{
    if (obj->is<PlainObject>() || obj->is<JSFunction>()) {
        unsigned propAttrs = GetInitDataPropAttrs(op);
        return NativeDefineDataProperty(cx, obj.as<NativeObject>(), name, rhs, propAttrs);
    }

    MOZ_ASSERT(obj->as<UnboxedPlainObject>().layout().lookup(name));
    RootedId id(cx, NameToId(name));
    return PutProperty(cx, obj, id, rhs, false);
}

inline bool
DefVarOperation(JSContext* cx, HandleObject varobj, HandlePropertyName dn, unsigned attrs)
{
    MOZ_ASSERT(varobj->isQualifiedVarObj());

#ifdef DEBUG
    // Per spec, it is an error to redeclare a lexical binding. This should
    // have already been checked.
    if (JS_HasExtensibleLexicalEnvironment(varobj)) {
        Rooted<LexicalEnvironmentObject*> lexicalEnv(cx);
        lexicalEnv = &JS_ExtensibleLexicalEnvironment(varobj)->as<LexicalEnvironmentObject>();
        MOZ_ASSERT(CheckVarNameConflict(cx, lexicalEnv, dn));
    }
#endif

    Rooted<PropertyResult> prop(cx);
    RootedObject obj2(cx);
    if (!LookupProperty(cx, varobj, dn, &obj2, &prop))
        return false;

    /* Steps 8c, 8d. */
    if (!prop || (obj2 != varobj && varobj->is<GlobalObject>())) {
        if (!DefineDataProperty(cx, varobj, dn, UndefinedHandleValue, attrs))
            return false;
    }

    if (varobj->is<GlobalObject>()) {
        if (!varobj->compartment()->addToVarNames(cx, dn))
            return false;
    }

    return true;
}

static MOZ_ALWAYS_INLINE bool
NegOperation(JSContext* cx, HandleValue val, MutableHandleValue res)
{
    /*
     * When the operand is int jsval, INT32_FITS_IN_JSVAL(i) implies
     * INT32_FITS_IN_JSVAL(-i) unless i is 0 or INT32_MIN when the
     * results, -0.0 or INT32_MAX + 1, are double values.
     */
    int32_t i;
    if (val.isInt32() && (i = val.toInt32()) != 0 && i != INT32_MIN) {
        res.setInt32(-i);
    } else {
        double d;
        if (!ToNumber(cx, val, &d))
            return false;
        res.setNumber(-d);
    }

    return true;
}

static MOZ_ALWAYS_INLINE bool
ToIdOperation(JSContext* cx, HandleValue idval, MutableHandleValue res)
{
    if (idval.isInt32()) {
        res.set(idval);
        return true;
    }

    RootedId id(cx);
    if (!ToPropertyKey(cx, idval, &id))
        return false;

    res.set(IdToValue(id));
    return true;
}

static MOZ_ALWAYS_INLINE bool
GetObjectElementOperation(JSContext* cx, JSOp op, JS::HandleObject obj, JS::HandleValue receiver,
                          HandleValue key, MutableHandleValue res)
{
    MOZ_ASSERT(op == JSOP_GETELEM || op == JSOP_CALLELEM || op == JSOP_GETELEM_SUPER);
    MOZ_ASSERT_IF(op == JSOP_GETELEM || op == JSOP_CALLELEM, obj == &receiver.toObject());

    do {
        uint32_t index;
        if (IsDefinitelyIndex(key, &index)) {
            if (GetElementNoGC(cx, obj, receiver, index, res.address()))
                break;

            if (!GetElement(cx, obj, receiver, index, res))
                return false;
            break;
        }

        if (key.isString()) {
            JSString* str = key.toString();
            JSAtom* name = str->isAtom() ? &str->asAtom() : AtomizeString(cx, str);
            if (!name)
                return false;
            if (name->isIndex(&index)) {
                if (GetElementNoGC(cx, obj, receiver, index, res.address()))
                    break;
            } else {
                if (GetPropertyNoGC(cx, obj, receiver, name->asPropertyName(), res.address()))
                    break;
            }
        }

        RootedId id(cx);
        if (!ToPropertyKey(cx, key, &id))
            return false;
        if (!GetProperty(cx, obj, receiver, id, res))
            return false;
    } while (false);

    assertSameCompartmentDebugOnly(cx, res);
    return true;
}

static MOZ_ALWAYS_INLINE bool
GetPrimitiveElementOperation(JSContext* cx, JSOp op, JS::HandleValue receiver,
                             HandleValue key, MutableHandleValue res)
{
    MOZ_ASSERT(op == JSOP_GETELEM || op == JSOP_CALLELEM);

    // FIXME: Bug 1234324 We shouldn't be boxing here.
    RootedObject boxed(cx, ToObjectFromStack(cx, receiver));
    if (!boxed)
        return false;

    do {
        uint32_t index;
        if (IsDefinitelyIndex(key, &index)) {
            if (GetElementNoGC(cx, boxed, receiver, index, res.address()))
                break;

            if (!GetElement(cx, boxed, receiver, index, res))
                return false;
            break;
        }

        if (key.isString()) {
            JSString* str = key.toString();
            JSAtom* name = str->isAtom() ? &str->asAtom() : AtomizeString(cx, str);
            if (!name)
                return false;
            if (name->isIndex(&index)) {
                if (GetElementNoGC(cx, boxed, receiver, index, res.address()))
                    break;
            } else {
                if (GetPropertyNoGC(cx, boxed, receiver, name->asPropertyName(), res.address()))
                    break;
            }
        }

        RootedId id(cx);
        if (!ToPropertyKey(cx, key, &id))
            return false;
        if (!GetProperty(cx, boxed, receiver, id, res))
            return false;
    } while (false);

    assertSameCompartmentDebugOnly(cx, res);
    return true;
}

static MOZ_ALWAYS_INLINE bool
GetElemOptimizedArguments(JSContext* cx, AbstractFramePtr frame, MutableHandleValue lref,
                          HandleValue rref, MutableHandleValue res, bool* done)
{
    MOZ_ASSERT(!*done);

    if (IsOptimizedArguments(frame, lref)) {
        if (rref.isInt32()) {
            int32_t i = rref.toInt32();
            if (i >= 0 && uint32_t(i) < frame.numActualArgs()) {
                res.set(frame.unaliasedActual(i));
                *done = true;
                return true;
            }
        }

        RootedScript script(cx, frame.script());
        if (!JSScript::argumentsOptimizationFailed(cx, script))
            return false;

        lref.set(ObjectValue(frame.argsObj()));
    }

    return true;
}

static MOZ_ALWAYS_INLINE bool
GetElementOperation(JSContext* cx, JSOp op, HandleValue lref, HandleValue rref,
                    MutableHandleValue res)
{
    MOZ_ASSERT(op == JSOP_GETELEM || op == JSOP_CALLELEM);

    uint32_t index;
    if (lref.isString() && IsDefinitelyIndex(rref, &index)) {
        JSString* str = lref.toString();
        if (index < str->length()) {
            str = cx->staticStrings().getUnitStringForElement(cx, str, index);
            if (!str)
                return false;
            res.setString(str);
            return true;
        }
    }

    if (lref.isPrimitive()) {
        RootedValue thisv(cx, lref);
        return GetPrimitiveElementOperation(cx, op, thisv, rref, res);
    }

    RootedObject obj(cx, &lref.toObject());
    RootedValue thisv(cx, lref);
    return GetObjectElementOperation(cx, op, obj, thisv, rref, res);
}

static MOZ_ALWAYS_INLINE JSString*
TypeOfOperation(const Value& v, JSRuntime* rt)
{
    JSType type = js::TypeOfValue(v);
    return TypeName(type, *rt->commonNames);
}

static MOZ_ALWAYS_INLINE bool
InitElemOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, HandleValue idval, HandleValue val)
{
    MOZ_ASSERT(!val.isMagic(JS_ELEMENTS_HOLE));

    RootedId id(cx);
    if (!ToPropertyKey(cx, idval, &id))
        return false;

    unsigned flags = GetInitDataPropAttrs(JSOp(*pc));
    return DefineDataProperty(cx, obj, id, val, flags);
}

static MOZ_ALWAYS_INLINE bool
InitArrayElemOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, uint32_t index, HandleValue val)
{
    JSOp op = JSOp(*pc);
    MOZ_ASSERT(op == JSOP_INITELEM_ARRAY || op == JSOP_INITELEM_INC);

    MOZ_ASSERT(obj->is<ArrayObject>());

    if (op == JSOP_INITELEM_INC && index == INT32_MAX) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_SPREAD_TOO_LARGE);
        return false;
    }

    /*
     * If val is a hole, do not call DefineElement.
     *
     * Furthermore, if the current op is JSOP_INITELEM_INC, always call
     * SetLengthProperty even if it is not the last element initialiser,
     * because it may be followed by JSOP_SPREAD, which will not set the array
     * length if nothing is spread.
     *
     * Alternatively, if the current op is JSOP_INITELEM_ARRAY, the length will
     * have already been set by the earlier JSOP_NEWARRAY; JSOP_INITELEM_ARRAY
     * cannot follow JSOP_SPREAD.
     */
    if (val.isMagic(JS_ELEMENTS_HOLE)) {
        if (op == JSOP_INITELEM_INC) {
            if (!SetLengthProperty(cx, obj, index + 1))
                return false;
        }
    } else {
        if (!DefineDataElement(cx, obj, index, val, JSPROP_ENUMERATE))
            return false;
    }

    return true;
}

static MOZ_ALWAYS_INLINE bool
ProcessCallSiteObjOperation(JSContext* cx, HandleObject cso, HandleObject raw)
{
    MOZ_ASSERT(cso->is<ArrayObject>());
    MOZ_ASSERT(raw->is<ArrayObject>());

    if (cso->nonProxyIsExtensible()) {
        RootedValue rawValue(cx, ObjectValue(*raw));
        if (!DefineDataProperty(cx, cso, cx->names().raw, rawValue, 0))
            return false;
        if (!FreezeObject(cx, raw))
            return false;
        if (!FreezeObject(cx, cso))
            return false;
    }
    return true;
}

#define RELATIONAL_OP(OP)                                                     \
    JS_BEGIN_MACRO                                                            \
        /* Optimize for two int-tagged operands (typical loop control). */    \
        if (lhs.isInt32() && rhs.isInt32()) {                                 \
            *res = lhs.toInt32() OP rhs.toInt32();                            \
        } else {                                                              \
            if (!ToPrimitive(cx, JSTYPE_NUMBER, lhs))                         \
                return false;                                                 \
            if (!ToPrimitive(cx, JSTYPE_NUMBER, rhs))                         \
                return false;                                                 \
            if (lhs.isString() && rhs.isString()) {                           \
                JSString* l = lhs.toString();                                 \
                JSString* r = rhs.toString();                                 \
                int32_t result;                                               \
                if (!CompareStrings(cx, l, r, &result))                       \
                    return false;                                             \
                *res = result OP 0;                                           \
            } else {                                                          \
                double l, r;                                                  \
                if (!ToNumber(cx, lhs, &l) || !ToNumber(cx, rhs, &r))         \
                    return false;                                             \
                *res = (l OP r);                                              \
            }                                                                 \
        }                                                                     \
        return true;                                                          \
    JS_END_MACRO

static MOZ_ALWAYS_INLINE bool
LessThanOperation(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res) {
    RELATIONAL_OP(<);
}

static MOZ_ALWAYS_INLINE bool
LessThanOrEqualOperation(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res) {
    RELATIONAL_OP(<=);
}

static MOZ_ALWAYS_INLINE bool
GreaterThanOperation(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res) {
    RELATIONAL_OP(>);
}

static MOZ_ALWAYS_INLINE bool
GreaterThanOrEqualOperation(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res) {
    RELATIONAL_OP(>=);
}

static MOZ_ALWAYS_INLINE bool
BitNot(JSContext* cx, HandleValue in, int* out)
{
    int i;
    if (!ToInt32(cx, in, &i))
        return false;
    *out = ~i;
    return true;
}

static MOZ_ALWAYS_INLINE bool
BitXor(JSContext* cx, HandleValue lhs, HandleValue rhs, int* out)
{
    int left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = left ^ right;
    return true;
}

static MOZ_ALWAYS_INLINE bool
BitOr(JSContext* cx, HandleValue lhs, HandleValue rhs, int* out)
{
    int left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = left | right;
    return true;
}

static MOZ_ALWAYS_INLINE bool
BitAnd(JSContext* cx, HandleValue lhs, HandleValue rhs, int* out)
{
    int left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = left & right;
    return true;
}

static MOZ_ALWAYS_INLINE bool
BitLsh(JSContext* cx, HandleValue lhs, HandleValue rhs, int* out)
{
    int32_t left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = uint32_t(left) << (right & 31);
    return true;
}

static MOZ_ALWAYS_INLINE bool
BitRsh(JSContext* cx, HandleValue lhs, HandleValue rhs, int* out)
{
    int32_t left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = left >> (right & 31);
    return true;
}

static MOZ_ALWAYS_INLINE bool
UrshOperation(JSContext* cx, HandleValue lhs, HandleValue rhs, MutableHandleValue out)
{
    uint32_t left;
    int32_t  right;
    if (!ToUint32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    left >>= right & 31;
    out.setNumber(uint32_t(left));
    return true;
}

template <typename T>
static MOZ_ALWAYS_INLINE bool
SignExtendOperation(JSContext* cx, HandleValue in, int* out)
{
    int32_t i;
    if (!ToInt32(cx, in, &i))
        return false;
    *out = (T)i;
    return true;
}

#undef RELATIONAL_OP

inline JSFunction*
ReportIfNotFunction(JSContext* cx, HandleValue v, MaybeConstruct construct = NO_CONSTRUCT)
{
    if (v.isObject() && v.toObject().is<JSFunction>())
        return &v.toObject().as<JSFunction>();

    ReportIsNotFunction(cx, v, -1, construct);
    return nullptr;
}

}  /* namespace js */

#endif /* vm_Interpreter_inl_h */
