/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Interpreter_inl_h
#define vm_Interpreter_inl_h

#include "vm/Interpreter.h"

#include "jscompartment.h"
#include "jsnum.h"
#include "jsstr.h"

#include "jit/Ion.h"
#include "vm/ArgumentsObject.h"

#include "jsatominlines.h"
#include "jsobjinlines.h"

#include "vm/ScopeObject-inl.h"
#include "vm/Stack-inl.h"
#include "vm/String-inl.h"
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
IsUninitializedLexicalSlot(HandleObject obj, HandleShape shape)
{
    if (obj->is<DynamicWithObject>())
        return false;
    // We check for IsImplicitDenseOrTypedArrayElement even though the shape
    // is always a non-indexed property because proxy hooks may return a
    // "non-native property found" shape, which happens to be encoded in the
    // same way as the "dense element" shape. See MarkNonNativePropertyFound.
    if (!shape ||
        IsImplicitDenseOrTypedArrayElement(shape) ||
        !shape->hasSlot() ||
        !shape->hasDefaultGetter() ||
        !shape->hasDefaultSetter())
    {
        return false;
    }
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

template <bool TypeOf> inline bool
FetchName(JSContext* cx, HandleObject obj, HandleObject obj2, HandlePropertyName name,
          HandleShape shape, MutableHandleValue vp)
{
    if (!shape) {
        if (TypeOf) {
            vp.setUndefined();
            return true;
        }
        return ReportIsNotDefined(cx, name);
    }

    /* Take the slow path if shape was not found in a native object. */
    if (!obj->isNative() || !obj2->isNative()) {
        Rooted<jsid> id(cx, NameToId(name));
        if (!GetProperty(cx, obj, obj, id, vp))
            return false;
    } else {
        RootedObject normalized(cx, obj);
        if (normalized->is<DynamicWithObject>() && !shape->hasDefaultGetter())
            normalized = &normalized->as<DynamicWithObject>().object();
        if (shape->isDataDescriptor() && shape->hasDefaultGetter()) {
            /* Fast path for Object instance properties. */
            MOZ_ASSERT(shape->hasSlot());
            vp.set(obj2->as<NativeObject>().getSlot(shape->slot()));
        } else {
            if (!NativeGetExistingProperty(cx, normalized, obj2.as<NativeObject>(), shape, vp))
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
FetchNameNoGC(JSObject* pobj, Shape* shape, MutableHandleValue vp)
{
    if (!shape || !pobj->isNative() || !shape->isDataDescriptor() || !shape->hasDefaultGetter())
        return false;

    vp.set(pobj->as<NativeObject>().getSlot(shape->slot()));
    return !IsUninitializedLexical(vp);
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
                       ScopeObject& obj, ScopeCoordinate sc, const Value& val,
                       MaybeCheckLexical checkLexical)
{
    MOZ_ASSERT_IF(checkLexical, !IsUninitializedLexical(obj.aliasedVar(sc)));

    // Avoid computing the name if no type updates are needed, as this may be
    // expensive on scopes with large numbers of variables.
    PropertyName* name = obj.isSingleton()
                         ? ScopeCoordinateName(cx->runtime()->scopeCoordinateNameCache, script, pc)
                         : nullptr;

    obj.setAliasedVar(cx, sc, name, val);
}

inline bool
SetNameOperation(JSContext* cx, JSScript* script, jsbytecode* pc, HandleObject scope,
                 HandleValue val)
{
    MOZ_ASSERT(*pc == JSOP_SETNAME ||
               *pc == JSOP_STRICTSETNAME ||
               *pc == JSOP_SETGNAME ||
               *pc == JSOP_STRICTSETGNAME);
    MOZ_ASSERT_IF((*pc == JSOP_SETGNAME || *pc == JSOP_STRICTSETGNAME) &&
                  !script->hasNonSyntacticScope(),
                  scope == cx->global() ||
                  scope == &cx->global()->lexicalScope() ||
                  scope->is<RuntimeLexicalErrorObject>());

    bool strict = *pc == JSOP_STRICTSETNAME || *pc == JSOP_STRICTSETGNAME;
    RootedPropertyName name(cx, script->getName(pc));

    // In strict mode, assigning to an undeclared global variable is an
    // error. To detect this, we call NativeSetProperty directly and pass
    // Unqualified. It stores the error, if any, in |result|.
    bool ok;
    ObjectOpResult result;
    RootedId id(cx, NameToId(name));
    RootedValue receiver(cx, ObjectValue(*scope));
    if (scope->isUnqualifiedVarObj()) {
        MOZ_ASSERT(!scope->getOps()->setProperty);
        ok = NativeSetProperty(cx, scope.as<NativeObject>(), id, val, receiver, Unqualified,
                               result);
    } else {
        ok = SetProperty(cx, scope, id, val, receiver, result);
    }
    return ok && result.checkStrictErrorOrWarning(cx, scope, id, strict);
}

inline bool
DefLexicalOperation(JSContext* cx, Handle<ClonedBlockObject*> lexicalScope,
                    HandleObject varObj, HandlePropertyName name, unsigned attrs)
{
    // Redeclaration checks should have already been done.
    MOZ_ASSERT(CheckLexicalNameConflict(cx, lexicalScope, varObj, name));
    RootedId id(cx, NameToId(name));
    RootedValue uninitialized(cx, MagicValue(JS_UNINITIALIZED_LEXICAL));
    return NativeDefineProperty(cx, lexicalScope, id, uninitialized, nullptr, nullptr, attrs);
}

inline bool
DefLexicalOperation(JSContext* cx, ClonedBlockObject* lexicalScopeArg,
                    JSObject* varObjArg, JSScript* script, jsbytecode* pc)
{
    MOZ_ASSERT(*pc == JSOP_DEFLET || *pc == JSOP_DEFCONST);
    RootedPropertyName name(cx, script->getName(pc));

    unsigned attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT;
    if (*pc == JSOP_DEFCONST)
        attrs |= JSPROP_READONLY;

    Rooted<ClonedBlockObject*> lexicalScope(cx, lexicalScopeArg);
    RootedObject varObj(cx, varObjArg);
    MOZ_ASSERT_IF(!script->hasNonSyntacticScope(),
                  lexicalScope == &cx->global()->lexicalScope() && varObj == cx->global());

    return DefLexicalOperation(cx, lexicalScope, varObj, name, attrs);
}

inline void
InitGlobalLexicalOperation(JSContext* cx, ClonedBlockObject* lexicalScopeArg,
                           JSScript* script, jsbytecode* pc, HandleValue value)
{
    MOZ_ASSERT_IF(!script->hasNonSyntacticScope(),
                  lexicalScopeArg == &cx->global()->lexicalScope());
    MOZ_ASSERT(*pc == JSOP_INITGLEXICAL);
    Rooted<ClonedBlockObject*> lexicalScope(cx, lexicalScopeArg);
    RootedShape shape(cx, lexicalScope->lookup(cx, script->getName(pc)));
    MOZ_ASSERT(shape);
    lexicalScope->setSlot(shape->slot(), value);
}

inline bool
InitPropertyOperation(JSContext* cx, JSOp op, HandleObject obj, HandleId id, HandleValue rhs)
{
    if (obj->is<PlainObject>() || obj->is<JSFunction>()) {
        unsigned propAttrs = GetInitDataPropAttrs(op);
        return NativeDefineProperty(cx, obj.as<NativeObject>(), id, rhs, nullptr, nullptr,
                                    propAttrs);
    }

    MOZ_ASSERT(obj->as<UnboxedPlainObject>().layout().lookup(id));
    return PutProperty(cx, obj, id, rhs, false);
}

inline bool
DefVarOperation(JSContext* cx, HandleObject varobj, HandlePropertyName dn, unsigned attrs)
{
    MOZ_ASSERT(varobj->isQualifiedVarObj());

#ifdef DEBUG
    // Per spec, it is an error to redeclare a lexical binding. This should
    // have already been checked.
    if (JS_HasExtensibleLexicalScope(varobj)) {
        Rooted<ClonedBlockObject*> lexicalScope(cx);
        lexicalScope = &JS_ExtensibleLexicalScope(varobj)->as<ClonedBlockObject>();
        MOZ_ASSERT(CheckVarNameConflict(cx, lexicalScope, dn));
    }
#endif

    RootedShape prop(cx);
    RootedObject obj2(cx);
    if (!LookupProperty(cx, varobj, dn, &obj2, &prop))
        return false;

    /* Steps 8c, 8d. */
    if (!prop || (obj2 != varobj && varobj->is<GlobalObject>())) {
        if (!DefineProperty(cx, varobj, dn, UndefinedHandleValue, nullptr, nullptr, attrs))
            return false;
    }

    return true;
}

static MOZ_ALWAYS_INLINE bool
NegOperation(JSContext* cx, HandleScript script, jsbytecode* pc, HandleValue val,
             MutableHandleValue res)
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
ToIdOperation(JSContext* cx, HandleScript script, jsbytecode* pc, HandleValue idval,
              MutableHandleValue res)
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
GetObjectElementOperation(JSContext* cx, JSOp op, JS::HandleObject obj, JS::HandleObject receiver,
                          HandleValue key, MutableHandleValue res)
{
    MOZ_ASSERT(op == JSOP_GETELEM || op == JSOP_CALLELEM || op == JSOP_GETELEM_SUPER);
    MOZ_ASSERT_IF(op == JSOP_GETELEM || op == JSOP_CALLELEM, obj == receiver);

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
GetPrimitiveElementOperation(JSContext* cx, JSOp op, JS::HandleValue receiver_,
                             HandleValue key, MutableHandleValue res)
{
    MOZ_ASSERT(op == JSOP_GETELEM || op == JSOP_CALLELEM);

    // FIXME: We shouldn't be boxing here or exposing the boxed object as
    //        receiver anywhere below (bug 603201).
    RootedObject boxed(cx, ToObjectFromStack(cx, receiver_));
    if (!boxed)
        return false;
    RootedValue receiver(cx, ObjectValue(*boxed));

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
        if (!GetProperty(cx, boxed, boxed, id, res))
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
GetElementOperation(JSContext* cx, JSOp op, MutableHandleValue lref, HandleValue rref,
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

    RootedObject thisv(cx, &lref.toObject());
    return GetObjectElementOperation(cx, op, thisv, thisv, rref, res);
}

static MOZ_ALWAYS_INLINE JSString*
TypeOfOperation(const Value& v, JSRuntime* rt)
{
    JSType type = js::TypeOfValue(v);
    return TypeName(type, *rt->commonNames);
}

static inline JSString*
TypeOfObjectOperation(JSObject* obj, JSRuntime* rt)
{
    JSType type = js::TypeOfObject(obj);
    return TypeName(type, *rt->commonNames);
}

static MOZ_ALWAYS_INLINE bool
InitElemOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, HandleValue idval, HandleValue val)
{
    MOZ_ASSERT(!val.isMagic(JS_ELEMENTS_HOLE));
    MOZ_ASSERT(!obj->getClass()->getProperty);
    MOZ_ASSERT(!obj->getClass()->setProperty);

    RootedId id(cx);
    if (!ToPropertyKey(cx, idval, &id))
        return false;

    unsigned flags = JSOp(*pc) == JSOP_INITHIDDENELEM ? 0 : JSPROP_ENUMERATE;
    return DefineProperty(cx, obj, id, val, nullptr, nullptr, flags);
}

static MOZ_ALWAYS_INLINE bool
InitArrayElemOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, uint32_t index, HandleValue val)
{
    JSOp op = JSOp(*pc);
    MOZ_ASSERT(op == JSOP_INITELEM_ARRAY || op == JSOP_INITELEM_INC);

    MOZ_ASSERT(obj->is<ArrayObject>() || obj->is<UnboxedArrayObject>());

    if (op == JSOP_INITELEM_INC && index == INT32_MAX) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_SPREAD_TOO_LARGE);
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
        if (!DefineElement(cx, obj, index, val, nullptr, nullptr, JSPROP_ENUMERATE))
            return false;
    }

    return true;
}

static MOZ_ALWAYS_INLINE bool
ProcessCallSiteObjOperation(JSContext* cx, RootedObject& cso, RootedObject& raw,
                            RootedValue& rawValue)
{
    bool extensible;
    if (!IsExtensible(cx, cso, &extensible))
        return false;
    if (extensible) {
        JSAtom* name = cx->names().raw;
        if (!DefineProperty(cx, cso, name->asPropertyName(), rawValue, nullptr, nullptr, 0))
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

#undef RELATIONAL_OP

inline JSFunction*
ReportIfNotFunction(JSContext* cx, HandleValue v, MaybeConstruct construct = NO_CONSTRUCT)
{
    if (v.isObject() && v.toObject().is<JSFunction>())
        return &v.toObject().as<JSFunction>();

    ReportIsNotFunction(cx, v, -1, construct);
    return nullptr;
}

/*
 * FastInvokeGuard is used to optimize calls to JS functions from natives written
 * in C++, for instance Array.map. If the callee is not Ion-compiled, this will
 * just call Invoke. If the callee has a valid IonScript, however, it will enter
 * Ion directly.
 */
class FastInvokeGuard
{
    InvokeArgs args_;
    RootedFunction fun_;
    RootedScript script_;

    // Constructing a JitContext is pretty expensive due to the TLS access,
    // so only do this if we have to.
    bool useIon_;

  public:
    FastInvokeGuard(JSContext* cx, const Value& fval)
      : args_(cx)
      , fun_(cx)
      , script_(cx)
      , useIon_(jit::IsIonEnabled(cx))
    {
        initFunction(fval);
    }

    void initFunction(const Value& fval) {
        if (fval.isObject() && fval.toObject().is<JSFunction>()) {
            JSFunction* fun = &fval.toObject().as<JSFunction>();
            if (fun->isInterpreted())
                fun_ = fun;
        }
    }

    InvokeArgs& args() {
        return args_;
    }

    bool invoke(JSContext* cx) {
        if (useIon_ && fun_) {
            if (!script_) {
                script_ = fun_->getOrCreateScript(cx);
                if (!script_)
                    return false;
            }
            MOZ_ASSERT(fun_->nonLazyScript() == script_);

            jit::MethodStatus status = jit::CanEnterUsingFastInvoke(cx, script_, args_.length());
            if (status == jit::Method_Error)
                return false;
            if (status == jit::Method_Compiled) {
                jit::JitExecStatus result = jit::FastInvoke(cx, fun_, args_);
                if (IsErrorStatus(result))
                    return false;

                MOZ_ASSERT(result == jit::JitExec_Ok);
                return true;
            }

            MOZ_ASSERT(status == jit::Method_Skipped);

            if (script_->canIonCompile()) {
                // This script is not yet hot. Since calling into Ion is much
                // faster here, bump the warm-up counter a bit to account for this.
                script_->incWarmUpCounter(5);
            }
        }

        return Invoke(cx, args_);
    }

  private:
    FastInvokeGuard(const FastInvokeGuard& other) = delete;
    const FastInvokeGuard& operator=(const FastInvokeGuard& other) = delete;
};

}  /* namespace js */

#endif /* vm_Interpreter_inl_h */
