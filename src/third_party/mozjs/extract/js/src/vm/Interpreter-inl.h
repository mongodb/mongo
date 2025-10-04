/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Interpreter_inl_h
#define vm_Interpreter_inl_h

#include "vm/Interpreter.h"

#include "jslibmath.h"
#include "jsmath.h"
#include "jsnum.h"

#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "util/CheckedArithmetic.h"
#include "vm/ArgumentsObject.h"
#include "vm/BigIntType.h"
#include "vm/BytecodeUtil.h"  // JSDVG_SEARCH_STACK
#include "vm/JSAtomUtils.h"   // AtomizeString
#include "vm/Realm.h"
#include "vm/SharedStencil.h"  // GCThingIndex
#include "vm/StaticStrings.h"
#include "vm/ThrowMsgKind.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "vm/RecordTupleShared.h"
#endif

#include "vm/GlobalObject-inl.h"
#include "vm/JSAtomUtils-inl.h"  // PrimitiveValueToId, TypeName
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"
#include "vm/StringType-inl.h"

namespace js {

/*
 * Per ES6, lexical declarations may not be accessed in any fashion until they
 * are initialized (i.e., until the actual declaring statement is
 * executed). The various LEXICAL opcodes need to check if the slot is an
 * uninitialized let declaration, represented by the magic value
 * JS_UNINITIALIZED_LEXICAL.
 */
static inline bool IsUninitializedLexical(const Value& val) {
  // Use whyMagic here because JS_OPTIMIZED_OUT could flow into here.
  return val.isMagic() && val.whyMagic() == JS_UNINITIALIZED_LEXICAL;
}

static inline bool IsUninitializedLexicalSlot(HandleObject obj,
                                              const PropertyResult& prop) {
  MOZ_ASSERT(prop.isFound());
  if (obj->is<WithEnvironmentObject>()) {
    return false;
  }

  // Proxy hooks may return a non-native property.
  if (prop.isNonNativeProperty()) {
    return false;
  }

  PropertyInfo propInfo = prop.propertyInfo();
  if (!propInfo.isDataProperty()) {
    return false;
  }

  return IsUninitializedLexical(
      obj->as<NativeObject>().getSlot(propInfo.slot()));
}

static inline bool CheckUninitializedLexical(JSContext* cx, PropertyName* name_,
                                             HandleValue val) {
  if (IsUninitializedLexical(val)) {
    Rooted<PropertyName*> name(cx, name_);
    ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, name);
    return false;
  }
  return true;
}

inline bool GetLengthProperty(const Value& lval, MutableHandleValue vp) {
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
inline bool FetchName(JSContext* cx, HandleObject receiver, HandleObject holder,
                      Handle<PropertyName*> name, const PropertyResult& prop,
                      MutableHandleValue vp) {
  if (prop.isNotFound()) {
    switch (mode) {
      case GetNameMode::Normal:
        ReportIsNotDefined(cx, name);
        return false;
      case GetNameMode::TypeOf:
        vp.setUndefined();
        return true;
    }
  }

  /* Take the slow path if shape was not found in a native object. */
  if (!receiver->is<NativeObject>() || !holder->is<NativeObject>() ||
      receiver->is<WithEnvironmentObject>()) {
    Rooted<jsid> id(cx, NameToId(name));
    if (!GetProperty(cx, receiver, receiver, id, vp)) {
      return false;
    }
  } else {
    PropertyInfo propInfo = prop.propertyInfo();
    if (propInfo.isDataProperty()) {
      /* Fast path for Object instance properties. */
      vp.set(holder->as<NativeObject>().getSlot(propInfo.slot()));
    } else {
      RootedId id(cx, NameToId(name));
      if (!NativeGetExistingProperty(cx, receiver, holder.as<NativeObject>(),
                                     id, propInfo, vp)) {
        return false;
      }
    }
  }

  // We do our own explicit checking for |this|
  if (name == cx->names().dot_this_) {
    return true;
  }

  // NAME operations are the slow paths already, so unconditionally check
  // for uninitialized lets.
  return CheckUninitializedLexical(cx, name, vp);
}

inline bool FetchNameNoGC(NativeObject* pobj, PropertyResult prop, Value* vp) {
  if (prop.isNotFound()) {
    return false;
  }

  PropertyInfo propInfo = prop.propertyInfo();
  if (!propInfo.isDataProperty()) {
    return false;
  }

  *vp = pobj->getSlot(propInfo.slot());
  return !IsUninitializedLexical(*vp);
}

template <js::GetNameMode mode>
inline bool GetEnvironmentName(JSContext* cx, HandleObject envChain,
                               Handle<PropertyName*> name,
                               MutableHandleValue vp) {
  {
    PropertyResult prop;
    JSObject* obj = nullptr;
    NativeObject* pobj = nullptr;
    if (LookupNameNoGC(cx, name, envChain, &obj, &pobj, &prop)) {
      if (FetchNameNoGC(pobj, prop, vp.address())) {
        return true;
      }
    }
  }

  PropertyResult prop;
  RootedObject obj(cx), pobj(cx);
  if (!LookupName(cx, name, envChain, &obj, &pobj, &prop)) {
    return false;
  }

  return FetchName<mode>(cx, obj, pobj, name, prop, vp);
}

inline bool HasOwnProperty(JSContext* cx, HandleValue val, HandleValue idValue,
                           bool* result) {
  // As an optimization, provide a fast path when rooting is not necessary and
  // we can safely retrieve the object's shape.
  jsid id;
  if (val.isObject() && idValue.isPrimitive() &&
      PrimitiveValueToId<NoGC>(cx, idValue, &id)) {
    JSObject* obj = &val.toObject();
    PropertyResult prop;
    if (obj->is<NativeObject>() &&
        NativeLookupOwnProperty<NoGC>(cx, &obj->as<NativeObject>(), id,
                                      &prop)) {
      *result = prop.isFound();
      return true;
    }
  }

  // Step 1.
  RootedId key(cx);
  if (!ToPropertyKey(cx, idValue, &key)) {
    return false;
  }

  // Step 2.
  RootedObject obj(cx, ToObject(cx, val));
  if (!obj) {
    return false;
  }

  // Step 3.
  return HasOwnProperty(cx, obj, key, result);
}

inline bool GetIntrinsicOperation(JSContext* cx, HandleScript script,
                                  jsbytecode* pc, MutableHandleValue vp) {
  Rooted<PropertyName*> name(cx, script->getName(pc));
  return GlobalObject::getIntrinsicValue(cx, cx->global(), name, vp);
}

inline bool SetIntrinsicOperation(JSContext* cx, JSScript* script,
                                  jsbytecode* pc, HandleValue val) {
  Rooted<PropertyName*> name(cx, script->getName(pc));
  return GlobalObject::setIntrinsicValue(cx, cx->global(), name, val);
}

inline bool SetNameOperation(JSContext* cx, JSScript* script, jsbytecode* pc,
                             HandleObject env, HandleValue val) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::SetName || JSOp(*pc) == JSOp::StrictSetName ||
             JSOp(*pc) == JSOp::SetGName || JSOp(*pc) == JSOp::StrictSetGName);
  MOZ_ASSERT_IF(
      JSOp(*pc) == JSOp::SetGName || JSOp(*pc) == JSOp::StrictSetGName,
      !script->hasNonSyntacticScope());
  MOZ_ASSERT_IF(
      JSOp(*pc) == JSOp::SetGName || JSOp(*pc) == JSOp::StrictSetGName,
      env == cx->global() || env == &cx->global()->lexicalEnvironment() ||
          env->is<RuntimeLexicalErrorObject>());

  bool strict =
      JSOp(*pc) == JSOp::StrictSetName || JSOp(*pc) == JSOp::StrictSetGName;
  Rooted<PropertyName*> name(cx, script->getName(pc));

  // In strict mode, assigning to an undeclared global variable is an
  // error. To detect this, we call NativeSetProperty directly and pass
  // Unqualified. It stores the error, if any, in |result|.
  bool ok;
  ObjectOpResult result;
  RootedId id(cx, NameToId(name));
  RootedValue receiver(cx, ObjectValue(*env));
  if (env->isUnqualifiedVarObj()) {
    Rooted<NativeObject*> varobj(cx);
    if (env->is<DebugEnvironmentProxy>()) {
      varobj =
          &env->as<DebugEnvironmentProxy>().environment().as<NativeObject>();
    } else {
      varobj = &env->as<NativeObject>();
    }
    MOZ_ASSERT(!varobj->getOpsSetProperty());
    ok = NativeSetProperty<Unqualified>(cx, varobj, id, val, receiver, result);
  } else {
    ok = SetProperty(cx, env, id, val, receiver, result);
  }
  return ok && result.checkStrictModeError(cx, env, id, strict);
}

inline void InitGlobalLexicalOperation(
    JSContext* cx, ExtensibleLexicalEnvironmentObject* lexicalEnv,
    JSScript* script, jsbytecode* pc, HandleValue value) {
  MOZ_ASSERT_IF(!script->hasNonSyntacticScope(),
                lexicalEnv == &cx->global()->lexicalEnvironment());
  MOZ_ASSERT(JSOp(*pc) == JSOp::InitGLexical);

  mozilla::Maybe<PropertyInfo> prop =
      lexicalEnv->lookup(cx, script->getName(pc));
  MOZ_ASSERT(prop.isSome());
  MOZ_ASSERT(IsUninitializedLexical(lexicalEnv->getSlot(prop->slot())));

  lexicalEnv->setSlot(prop->slot(), value);
}

inline bool InitPropertyOperation(JSContext* cx, jsbytecode* pc,
                                  HandleObject obj, Handle<PropertyName*> name,
                                  HandleValue rhs) {
  unsigned propAttrs = GetInitDataPropAttrs(JSOp(*pc));
  return DefineDataProperty(cx, obj, name, rhs, propAttrs);
}

static MOZ_ALWAYS_INLINE bool NegOperation(JSContext* cx,
                                           MutableHandleValue val,
                                           MutableHandleValue res) {
  /*
   * When the operand is int jsval, INT32_FITS_IN_JSVAL(i) implies
   * INT32_FITS_IN_JSVAL(-i) unless i is 0 or INT32_MIN when the
   * results, -0.0 or INT32_MAX + 1, are double values.
   */
  int32_t i;
  if (val.isInt32() && (i = val.toInt32()) != 0 && i != INT32_MIN) {
    res.setInt32(-i);
    return true;
  }

  if (!ToNumeric(cx, val)) {
    return false;
  }

  if (val.isBigInt()) {
    return BigInt::negValue(cx, val, res);
  }

  res.setNumber(-val.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool IncOperation(JSContext* cx, HandleValue val,
                                           MutableHandleValue res) {
  int32_t i;
  if (val.isInt32() && (i = val.toInt32()) != INT32_MAX) {
    res.setInt32(i + 1);
    return true;
  }

  if (val.isNumber()) {
    res.setNumber(val.toNumber() + 1);
    return true;
  }

  MOZ_ASSERT(val.isBigInt(), "+1 only callable on result of JSOp::ToNumeric");
  return BigInt::incValue(cx, val, res);
}

static MOZ_ALWAYS_INLINE bool DecOperation(JSContext* cx, HandleValue val,
                                           MutableHandleValue res) {
  int32_t i;
  if (val.isInt32() && (i = val.toInt32()) != INT32_MIN) {
    res.setInt32(i - 1);
    return true;
  }

  if (val.isNumber()) {
    res.setNumber(val.toNumber() - 1);
    return true;
  }

  MOZ_ASSERT(val.isBigInt(), "-1 only callable on result of JSOp::ToNumeric");
  return BigInt::decValue(cx, val, res);
}

static MOZ_ALWAYS_INLINE bool ToPropertyKeyOperation(JSContext* cx,
                                                     HandleValue idval,
                                                     MutableHandleValue res) {
  if (idval.isInt32()) {
    res.set(idval);
    return true;
  }

  RootedId id(cx);
  if (!ToPropertyKey(cx, idval, &id)) {
    return false;
  }

  res.set(IdToValue(id));
  return true;
}

static MOZ_ALWAYS_INLINE bool GetObjectElementOperation(
    JSContext* cx, JSOp op, JS::HandleObject obj, JS::HandleValue receiver,
    HandleValue key, MutableHandleValue res) {
  MOZ_ASSERT(op == JSOp::GetElem || op == JSOp::GetElemSuper);
  MOZ_ASSERT_IF(op == JSOp::GetElem, obj == &receiver.toObject());

  do {
    uint32_t index;
    if (IsDefinitelyIndex(key, &index)) {
      if (GetElementNoGC(cx, obj, receiver, index, res.address())) {
        break;
      }

      if (!GetElement(cx, obj, receiver, index, res)) {
        return false;
      }
      break;
    }

    if (key.isString()) {
      JSString* str = key.toString();
      JSAtom* name = str->isAtom() ? &str->asAtom() : AtomizeString(cx, str);
      if (!name) {
        return false;
      }
      if (name->isIndex(&index)) {
        if (GetElementNoGC(cx, obj, receiver, index, res.address())) {
          break;
        }
      } else {
        if (GetPropertyNoGC(cx, obj, receiver, name->asPropertyName(),
                            res.address())) {
          break;
        }
      }
    }

    RootedId id(cx);
    if (!ToPropertyKey(cx, key, &id)) {
      return false;
    }
    if (!GetProperty(cx, obj, receiver, id, res)) {
      return false;
    }
  } while (false);

  cx->debugOnlyCheck(res);
  return true;
}

static MOZ_ALWAYS_INLINE bool GetPrimitiveElementOperation(
    JSContext* cx, JS::HandleValue receiver, int receiverIndex, HandleValue key,
    MutableHandleValue res) {
#ifdef ENABLE_RECORD_TUPLE
  if (receiver.isExtendedPrimitive()) {
    RootedId id(cx);
    if (!ToPropertyKey(cx, key, &id)) {
      return false;
    }
    RootedObject obj(cx, &receiver.toExtendedPrimitive());
    if (!ExtendedPrimitiveGetProperty(cx, obj, receiver, id, res)) {
      return false;
    }
  }
#endif

  // FIXME: Bug 1234324 We shouldn't be boxing here.
  RootedObject boxed(
      cx, ToObjectFromStackForPropertyAccess(cx, receiver, receiverIndex, key));
  if (!boxed) {
    return false;
  }

  do {
    uint32_t index;
    if (IsDefinitelyIndex(key, &index)) {
      if (GetElementNoGC(cx, boxed, receiver, index, res.address())) {
        break;
      }

      if (!GetElement(cx, boxed, receiver, index, res)) {
        return false;
      }
      break;
    }

    if (key.isString()) {
      JSString* str = key.toString();
      JSAtom* name = str->isAtom() ? &str->asAtom() : AtomizeString(cx, str);
      if (!name) {
        return false;
      }
      if (name->isIndex(&index)) {
        if (GetElementNoGC(cx, boxed, receiver, index, res.address())) {
          break;
        }
      } else {
        if (GetPropertyNoGC(cx, boxed, receiver, name->asPropertyName(),
                            res.address())) {
          break;
        }
      }
    }

    RootedId id(cx);
    if (!ToPropertyKey(cx, key, &id)) {
      return false;
    }
    if (!GetProperty(cx, boxed, receiver, id, res)) {
      return false;
    }
  } while (false);

  cx->debugOnlyCheck(res);
  return true;
}

static MOZ_ALWAYS_INLINE bool GetElementOperationWithStackIndex(
    JSContext* cx, HandleValue lref, int lrefIndex, HandleValue rref,
    MutableHandleValue res) {
  uint32_t index;
  if (lref.isString() && IsDefinitelyIndex(rref, &index)) {
    JSString* str = lref.toString();
    if (index < str->length()) {
      str = cx->staticStrings().getUnitStringForElement(cx, str, index);
      if (!str) {
        return false;
      }
      res.setString(str);
      return true;
    }
  }

  if (lref.isPrimitive()) {
    return GetPrimitiveElementOperation(cx, lref, lrefIndex, rref, res);
  }

  RootedObject obj(cx, &lref.toObject());
  return GetObjectElementOperation(cx, JSOp::GetElem, obj, lref, rref, res);
}

// Wrapper for callVM from JIT.
static MOZ_ALWAYS_INLINE bool GetElementOperation(JSContext* cx,
                                                  HandleValue lref,
                                                  HandleValue rref,
                                                  MutableHandleValue res) {
  return GetElementOperationWithStackIndex(cx, lref, JSDVG_SEARCH_STACK, rref,
                                           res);
}

static MOZ_ALWAYS_INLINE JSString* TypeOfOperation(const Value& v,
                                                   JSRuntime* rt) {
  JSType type = js::TypeOfValue(v);
  return TypeName(type, *rt->commonNames);
}

static MOZ_ALWAYS_INLINE bool InitElemOperation(JSContext* cx, jsbytecode* pc,
                                                HandleObject obj,
                                                HandleValue idval,
                                                HandleValue val) {
  MOZ_ASSERT(!val.isMagic(JS_ELEMENTS_HOLE));

  RootedId id(cx);
  if (!ToPropertyKey(cx, idval, &id)) {
    return false;
  }

  unsigned flags = GetInitDataPropAttrs(JSOp(*pc));
  if (id.isPrivateName()) {
    // Clear enumerate flag off of private names.
    flags &= ~JSPROP_ENUMERATE;
  }
  return DefineDataProperty(cx, obj, id, val, flags);
}

static MOZ_ALWAYS_INLINE bool CheckPrivateFieldOperation(JSContext* cx,
                                                         jsbytecode* pc,
                                                         HandleValue val,
                                                         HandleValue idval,
                                                         bool* result) {
  MOZ_ASSERT(idval.isSymbol());
  MOZ_ASSERT(idval.toSymbol()->isPrivateName());

  // Result had better not be a nullptr.
  MOZ_ASSERT(result);

  ThrowCondition condition;
  ThrowMsgKind msgKind;
  GetCheckPrivateFieldOperands(pc, &condition, &msgKind);

  // When we are using OnlyCheckRhs, we are implementing PrivateInExpr
  // This requires we throw if the rhs is not an object;
  //
  // The InlineCache for CheckPrivateField already checks for a
  // non-object rhs and refuses to attach in that circumstance.
  if (condition == ThrowCondition::OnlyCheckRhs) {
    if (!val.isObject()) {
      ReportInNotObjectError(cx, idval, val);
      return false;
    }
  }

  // Invoke the HostEnsureCanAddPrivateElement ( O ) host hook here
  // if the code is attempting to attach a new private element (which
  // corresponds to the ThrowHas Throw Condition).
  if (condition == ThrowCondition::ThrowHas) {
    if (JS::EnsureCanAddPrivateElementOp op =
            cx->runtime()->canAddPrivateElement) {
      if (!op(cx, val)) {
        return false;
      }
    }
  }

  if (!HasOwnProperty(cx, val, idval, result)) {
    return false;
  }

  if (!CheckPrivateFieldWillThrow(condition, *result)) {
    return true;
  }

  // Throw!
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            ThrowMsgKindToErrNum(msgKind));
  return false;
}

static inline JS::Symbol* NewPrivateName(JSContext* cx, Handle<JSAtom*> name) {
  return JS::Symbol::new_(cx, JS::SymbolCode::PrivateNameSymbol, name);
}

inline bool InitElemIncOperation(JSContext* cx, Handle<ArrayObject*> arr,
                                 uint32_t index, HandleValue val) {
  if (index == INT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SPREAD_TOO_LARGE);
    return false;
  }

  // If val is a hole, do not call DefineDataElement.
  if (val.isMagic(JS_ELEMENTS_HOLE)) {
    // Always call SetLengthProperty even if this is not the last element
    // initialiser, because this may be followed by a SpreadElement loop,
    // which will not set the array length if nothing is spread.
    return SetLengthProperty(cx, arr, index + 1);
  }

  return DefineDataElement(cx, arr, index, val, JSPROP_ENUMERATE);
}

inline JSFunction* ReportIfNotFunction(
    JSContext* cx, HandleValue v, MaybeConstruct construct = NO_CONSTRUCT) {
  if (v.isObject() && v.toObject().is<JSFunction>()) {
    return &v.toObject().as<JSFunction>();
  }

  ReportIsNotFunction(cx, v, -1, construct);
  return nullptr;
}

static inline JSObject* SuperFunOperation(JSObject* callee) {
  MOZ_ASSERT(callee->as<JSFunction>().isClassConstructor());
  MOZ_ASSERT(
      callee->as<JSFunction>().baseScript()->isDerivedClassConstructor());

  return callee->as<JSFunction>().staticPrototype();
}

static inline JSObject* HomeObjectSuperBase(JSObject* homeObj) {
  MOZ_ASSERT(homeObj->is<PlainObject>() || homeObj->is<JSFunction>());

  return homeObj->staticPrototype();
}

static MOZ_ALWAYS_INLINE bool AddOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (lhs.isInt32() && rhs.isInt32()) {
    int32_t l = lhs.toInt32(), r = rhs.toInt32();
    int32_t t;
    if (MOZ_LIKELY(SafeAdd(l, r, &t))) {
      res.setInt32(t);
      return true;
    }
  }

  if (!ToPrimitive(cx, lhs)) {
    return false;
  }
  if (!ToPrimitive(cx, rhs)) {
    return false;
  }

  bool lIsString = lhs.isString();
  bool rIsString = rhs.isString();
  if (lIsString || rIsString) {
    JSString* lstr;
    if (lIsString) {
      lstr = lhs.toString();
    } else {
      lstr = ToString<CanGC>(cx, lhs);
      if (!lstr) {
        return false;
      }
    }

    JSString* rstr;
    if (rIsString) {
      rstr = rhs.toString();
    } else {
      // Save/restore lstr in case of GC activity under ToString.
      lhs.setString(lstr);
      rstr = ToString<CanGC>(cx, rhs);
      if (!rstr) {
        return false;
      }
      lstr = lhs.toString();
    }
    JSString* str = ConcatStrings<NoGC>(cx, lstr, rstr);
    if (!str) {
      RootedString nlstr(cx, lstr), nrstr(cx, rstr);
      str = ConcatStrings<CanGC>(cx, nlstr, nrstr);
      if (!str) {
        return false;
      }
    }
    res.setString(str);
    return true;
  }

  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::addValue(cx, lhs, rhs, res);
  }

  res.setNumber(lhs.toNumber() + rhs.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool SubOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::subValue(cx, lhs, rhs, res);
  }

  res.setNumber(lhs.toNumber() - rhs.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool MulOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::mulValue(cx, lhs, rhs, res);
  }

  res.setNumber(lhs.toNumber() * rhs.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool DivOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::divValue(cx, lhs, rhs, res);
  }

  res.setNumber(NumberDiv(lhs.toNumber(), rhs.toNumber()));
  return true;
}

static MOZ_ALWAYS_INLINE bool ModOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  int32_t l, r;
  if (lhs.isInt32() && rhs.isInt32() && (l = lhs.toInt32()) >= 0 &&
      (r = rhs.toInt32()) > 0) {
    int32_t mod = l % r;
    res.setInt32(mod);
    return true;
  }

  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::modValue(cx, lhs, rhs, res);
  }

  res.setNumber(NumberMod(lhs.toNumber(), rhs.toNumber()));
  return true;
}

static MOZ_ALWAYS_INLINE bool PowOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::powValue(cx, lhs, rhs, res);
  }

  res.setNumber(ecmaPow(lhs.toNumber(), rhs.toNumber()));
  return true;
}

static MOZ_ALWAYS_INLINE bool BitNotOperation(JSContext* cx,
                                              MutableHandleValue in,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, in)) {
    return false;
  }

  if (in.isBigInt()) {
    return BigInt::bitNotValue(cx, in, out);
  }

  out.setInt32(~in.toInt32());
  return true;
}

static MOZ_ALWAYS_INLINE bool BitXorOperation(JSContext* cx,
                                              MutableHandleValue lhs,
                                              MutableHandleValue rhs,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::bitXorValue(cx, lhs, rhs, out);
  }

  out.setInt32(lhs.toInt32() ^ rhs.toInt32());
  return true;
}

static MOZ_ALWAYS_INLINE bool BitOrOperation(JSContext* cx,
                                             MutableHandleValue lhs,
                                             MutableHandleValue rhs,
                                             MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::bitOrValue(cx, lhs, rhs, out);
  }

  out.setInt32(lhs.toInt32() | rhs.toInt32());
  return true;
}

static MOZ_ALWAYS_INLINE bool BitAndOperation(JSContext* cx,
                                              MutableHandleValue lhs,
                                              MutableHandleValue rhs,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::bitAndValue(cx, lhs, rhs, out);
  }

  out.setInt32(lhs.toInt32() & rhs.toInt32());
  return true;
}

static MOZ_ALWAYS_INLINE bool BitLshOperation(JSContext* cx,
                                              MutableHandleValue lhs,
                                              MutableHandleValue rhs,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::lshValue(cx, lhs, rhs, out);
  }

  // Signed left-shift is undefined on overflow, so |lhs << (rhs & 31)| won't
  // work.  Instead, convert to unsigned space (where overflow is treated
  // modularly), perform the operation there, then convert back.
  uint32_t left = static_cast<uint32_t>(lhs.toInt32());
  uint8_t right = rhs.toInt32() & 31;
  out.setInt32(mozilla::WrapToSigned(left << right));
  return true;
}

static MOZ_ALWAYS_INLINE bool BitRshOperation(JSContext* cx,
                                              MutableHandleValue lhs,
                                              MutableHandleValue rhs,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::rshValue(cx, lhs, rhs, out);
  }

  out.setInt32(lhs.toInt32() >> (rhs.toInt32() & 31));
  return true;
}

static MOZ_ALWAYS_INLINE bool UrshOperation(JSContext* cx,
                                            MutableHandleValue lhs,
                                            MutableHandleValue rhs,
                                            MutableHandleValue out) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_TO_NUMBER);
    return false;
  }

  uint32_t left;
  int32_t right;
  if (!ToUint32(cx, lhs, &left) || !ToInt32(cx, rhs, &right)) {
    return false;
  }
  left >>= right & 31;
  out.setNumber(uint32_t(left));
  return true;
}

static MOZ_ALWAYS_INLINE void InitElemArrayOperation(JSContext* cx,
                                                     jsbytecode* pc,
                                                     Handle<ArrayObject*> arr,
                                                     HandleValue val) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::InitElemArray);

  // The dense elements must have been initialized up to this index. The JIT
  // implementation also depends on this.
  uint32_t index = GET_UINT32(pc);
  MOZ_ASSERT(index < arr->getDenseCapacity());
  MOZ_ASSERT(index == arr->getDenseInitializedLength());

  // Bump the initialized length even for hole values to ensure the
  // index == initLength invariant holds for later InitElemArray ops.
  arr->setDenseInitializedLength(index + 1);

  if (val.isMagic(JS_ELEMENTS_HOLE)) {
    arr->initDenseElementHole(index);
  } else {
    arr->initDenseElement(index, val);
  }
}

/*
 * As an optimization, the interpreter creates a handful of reserved Rooted<T>
 * variables at the beginning, thus inserting them into the Rooted list once
 * upon entry. ReservedRooted "borrows" a reserved Rooted variable and uses it
 * within a local scope, resetting the value to nullptr (or the appropriate
 * equivalent for T) at scope end. This avoids inserting/removing the Rooted
 * from the rooter list, while preventing stale values from being kept alive
 * unnecessarily.
 */

template <typename T>
class ReservedRooted : public RootedOperations<T, ReservedRooted<T>> {
  Rooted<T>* savedRoot;

 public:
  ReservedRooted(Rooted<T>* root, const T& ptr) : savedRoot(root) {
    *root = ptr;
  }

  explicit ReservedRooted(Rooted<T>* root) : savedRoot(root) {
    *root = JS::SafelyInitialized<T>::create();
  }

  ~ReservedRooted() { *savedRoot = JS::SafelyInitialized<T>::create(); }

  void set(const T& p) const { *savedRoot = p; }
  operator Handle<T>() { return *savedRoot; }
  operator Rooted<T>&() { return *savedRoot; }
  MutableHandle<T> operator&() { return &*savedRoot; }

  DECLARE_NONPOINTER_ACCESSOR_METHODS(savedRoot->get())
  DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(savedRoot->get())
  DECLARE_POINTER_CONSTREF_OPS(T)
  DECLARE_POINTER_ASSIGN_OPS(ReservedRooted, T)
};

} /* namespace js */

#endif /* vm_Interpreter_inl_h */
