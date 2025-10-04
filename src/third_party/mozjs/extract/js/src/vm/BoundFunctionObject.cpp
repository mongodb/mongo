/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/BoundFunctionObject.h"

#include <string_view>

#include "util/StringBuffer.h"
#include "vm/Interpreter.h"
#include "vm/Shape.h"
#include "vm/Stack.h"

#include "gc/ObjectKind-inl.h"
#include "vm/JSFunction-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

// Helper function to initialize `args` with all bound arguments + the arguments
// supplied in `callArgs`.
template <typename Args>
static MOZ_ALWAYS_INLINE void FillArguments(Args& args,
                                            BoundFunctionObject* bound,
                                            size_t numBoundArgs,
                                            const CallArgs& callArgs) {
  MOZ_ASSERT(args.length() == numBoundArgs + callArgs.length());

  if (numBoundArgs <= BoundFunctionObject::MaxInlineBoundArgs) {
    for (size_t i = 0; i < numBoundArgs; i++) {
      args[i].set(bound->getInlineBoundArg(i));
    }
  } else {
    ArrayObject* boundArgs = bound->getBoundArgsArray();
    for (size_t i = 0; i < numBoundArgs; i++) {
      args[i].set(boundArgs->getDenseElement(i));
    }
  }

  for (size_t i = 0; i < callArgs.length(); i++) {
    args[numBoundArgs + i].set(callArgs[i]);
  }
}

// ES2023 10.4.1.1 [[Call]]
// https://tc39.es/ecma262/#sec-bound-function-exotic-objects-call-thisargument-argumentslist
// static
bool BoundFunctionObject::call(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<BoundFunctionObject*> bound(cx,
                                     &args.callee().as<BoundFunctionObject>());

  // Step 1.
  Rooted<Value> target(cx, bound->getTargetVal());

  // Step 2.
  Rooted<Value> boundThis(cx, bound->getBoundThis());

  // Steps 3-4.
  size_t numBoundArgs = bound->numBoundArgs();
  InvokeArgs args2(cx);
  if (!args2.init(cx, uint64_t(numBoundArgs) + args.length())) {
    return false;
  }
  FillArguments(args2, bound, numBoundArgs, args);

  // Step 5.
  return Call(cx, target, boundThis, args2, args.rval());
}

// ES2023 10.4.1.2 [[Construct]]
// https://tc39.es/ecma262/#sec-bound-function-exotic-objects-construct-argumentslist-newtarget
// static
bool BoundFunctionObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<BoundFunctionObject*> bound(cx,
                                     &args.callee().as<BoundFunctionObject>());

  MOZ_ASSERT(bound->isConstructor(),
             "shouldn't have called this hook if not a constructor");

  // Step 1.
  Rooted<Value> target(cx, bound->getTargetVal());

  // Step 2.
  MOZ_ASSERT(IsConstructor(target));

  // Steps 3-4.
  size_t numBoundArgs = bound->numBoundArgs();
  ConstructArgs args2(cx);
  if (!args2.init(cx, uint64_t(numBoundArgs) + args.length())) {
    return false;
  }
  FillArguments(args2, bound, numBoundArgs, args);

  // Step 5.
  Rooted<Value> newTarget(cx, args.newTarget());
  if (newTarget == ObjectValue(*bound)) {
    newTarget = target;
  }

  // Step 6.
  Rooted<JSObject*> res(cx);
  if (!Construct(cx, target, args2, newTarget, &res)) {
    return false;
  }
  args.rval().setObject(*res);
  return true;
}

// static
JSString* BoundFunctionObject::funToString(JSContext* cx, Handle<JSObject*> obj,
                                           bool isToSource) {
  // Implementation of the funToString hook used by Function.prototype.toString.

  // For the non-standard toSource extension, we include "bound" to indicate
  // it's a bound function.
  if (isToSource) {
    static constexpr std::string_view nativeCodeBound =
        "function bound() {\n    [native code]\n}";
    return NewStringCopy<CanGC>(cx, nativeCodeBound);
  }

  static constexpr std::string_view nativeCode =
      "function() {\n    [native code]\n}";
  return NewStringCopy<CanGC>(cx, nativeCode);
}

// static
SharedShape* BoundFunctionObject::assignInitialShape(
    JSContext* cx, Handle<BoundFunctionObject*> obj) {
  MOZ_ASSERT(obj->empty());

  constexpr PropertyFlags propFlags = {PropertyFlag::Configurable};
  if (!NativeObject::addPropertyInReservedSlot(cx, obj, cx->names().length,
                                               LengthSlot, propFlags)) {
    return nullptr;
  }
  if (!NativeObject::addPropertyInReservedSlot(cx, obj, cx->names().name,
                                               NameSlot, propFlags)) {
    return nullptr;
  }

  SharedShape* shape = obj->sharedShape();
  if (shape->proto() == TaggedProto(&cx->global()->getFunctionPrototype())) {
    cx->global()->setBoundFunctionShapeWithDefaultProto(shape);
  }
  return shape;
}

static MOZ_ALWAYS_INLINE bool ComputeLengthValue(
    JSContext* cx, Handle<BoundFunctionObject*> bound, Handle<JSObject*> target,
    size_t numBoundArgs, double* length) {
  *length = 0.0;

  // Try to avoid invoking the JSFunction resolve hook.
  if (target->is<JSFunction>() &&
      !target->as<JSFunction>().hasResolvedLength()) {
    uint16_t targetLength;
    if (!JSFunction::getUnresolvedLength(cx, target.as<JSFunction>(),
                                         &targetLength)) {
      return false;
    }

    if (size_t(targetLength) > numBoundArgs) {
      *length = size_t(targetLength) - numBoundArgs;
    }
    return true;
  }

  // Use a fast path for getting the .length value if the target is a bound
  // function with its initial shape.
  Value targetLength;
  if (target->is<BoundFunctionObject>() && target->shape() == bound->shape()) {
    BoundFunctionObject* targetFn = &target->as<BoundFunctionObject>();
    targetLength = targetFn->getLengthForInitialShape();
  } else {
    bool hasLength;
    Rooted<PropertyKey> key(cx, NameToId(cx->names().length));
    if (!HasOwnProperty(cx, target, key, &hasLength)) {
      return false;
    }

    if (!hasLength) {
      return true;
    }

    Rooted<Value> targetLengthRoot(cx);
    if (!GetProperty(cx, target, target, key, &targetLengthRoot)) {
      return false;
    }
    targetLength = targetLengthRoot;
  }

  if (targetLength.isNumber()) {
    *length = std::max(
        0.0, JS::ToInteger(targetLength.toNumber()) - double(numBoundArgs));
  }
  return true;
}

static MOZ_ALWAYS_INLINE JSAtom* AppendBoundFunctionPrefix(JSContext* cx,
                                                           JSString* str) {
  auto& cache = cx->zone()->boundPrefixCache();

  JSAtom* strAtom = str->isAtom() ? &str->asAtom() : nullptr;
  if (strAtom) {
    if (auto p = cache.lookup(strAtom)) {
      return p->value();
    }
  }

  StringBuffer sb(cx);
  if (!sb.append("bound ") || !sb.append(str)) {
    return nullptr;
  }
  JSAtom* atom = sb.finishAtom();
  if (!atom) {
    return nullptr;
  }

  if (strAtom) {
    (void)cache.putNew(strAtom, atom);
  }
  return atom;
}

static MOZ_ALWAYS_INLINE JSAtom* ComputeNameValue(
    JSContext* cx, Handle<BoundFunctionObject*> bound,
    Handle<JSObject*> target) {
  // Try to avoid invoking the JSFunction resolve hook.
  JSString* name = nullptr;
  if (target->is<JSFunction>() && !target->as<JSFunction>().hasResolvedName()) {
    JSFunction* targetFn = &target->as<JSFunction>();
    name = targetFn->getUnresolvedName(cx);
    if (!name) {
      return nullptr;
    }
  } else {
    // Use a fast path for getting the .name value if the target is a bound
    // function with its initial shape.
    Value targetName;
    if (target->is<BoundFunctionObject>() &&
        target->shape() == bound->shape()) {
      BoundFunctionObject* targetFn = &target->as<BoundFunctionObject>();
      targetName = targetFn->getNameForInitialShape();
    } else {
      Rooted<Value> targetNameRoot(cx);
      if (!GetProperty(cx, target, target, cx->names().name, &targetNameRoot)) {
        return nullptr;
      }
      targetName = targetNameRoot;
    }
    if (!targetName.isString()) {
      return cx->names().boundWithSpace_;
    }
    name = targetName.toString();
  }

  return AppendBoundFunctionPrefix(cx, name);
}

// ES2023 20.2.3.2 Function.prototype.bind
// https://tc39.es/ecma262/#sec-function.prototype.bind
// static
bool BoundFunctionObject::functionBind(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  if (!IsCallable(args.thisv())) {
    ReportIncompatibleMethod(cx, args, &FunctionClass);
    return false;
  }

  if (MOZ_UNLIKELY(args.length() > ARGS_LENGTH_MAX)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TOO_MANY_ARGUMENTS);
    return false;
  }

  Rooted<JSObject*> target(cx, &args.thisv().toObject());

  BoundFunctionObject* bound =
      functionBindImpl(cx, target, args.array(), args.length(), nullptr);
  if (!bound) {
    return false;
  }

  // Step 11.
  args.rval().setObject(*bound);
  return true;
}

// ES2023 20.2.3.2 Function.prototype.bind
// https://tc39.es/ecma262/#sec-function.prototype.bind
//
// ES2023 10.4.1.3 BoundFunctionCreate
// https://tc39.es/ecma262/#sec-boundfunctioncreate
//
// BoundFunctionCreate has been inlined in Function.prototype.bind for
// performance reasons.
//
// static
BoundFunctionObject* BoundFunctionObject::functionBindImpl(
    JSContext* cx, Handle<JSObject*> target, Value* args, uint32_t argc,
    Handle<BoundFunctionObject*> maybeBound) {
  MOZ_ASSERT(target->isCallable());

  // Make sure the arguments on the stack are rooted when we're called directly
  // from JIT code.
  RootedExternalValueArray argsRoot(cx, argc, args);

  size_t numBoundArgs = argc > 0 ? argc - 1 : 0;
  MOZ_ASSERT(numBoundArgs <= ARGS_LENGTH_MAX, "ensured by callers");

  // If this assertion fails, make sure we use the correct AllocKind and that we
  // use all of its slots (consider increasing MaxInlineBoundArgs).
  static_assert(gc::GetGCKindSlots(allocKind) == SlotCount);

  // ES2023 10.4.1.3 BoundFunctionCreate
  // Steps 1-5.
  Rooted<BoundFunctionObject*> bound(cx);
  if (maybeBound) {
    // We allocated a bound function in JIT code. In the uncommon case of the
    // target not having Function.prototype as proto, we have to set the right
    // proto here.
    bound = maybeBound;
    if (MOZ_UNLIKELY(bound->staticPrototype() != target->staticPrototype())) {
      Rooted<JSObject*> proto(cx, target->staticPrototype());
      if (!SetPrototype(cx, bound, proto)) {
        return nullptr;
      }
    }
  } else {
    // Step 1.
    Rooted<JSObject*> proto(cx);
    if (!GetPrototype(cx, target, &proto)) {
      return nullptr;
    }

    // Steps 2-5.
    if (proto == &cx->global()->getFunctionPrototype() &&
        cx->global()->maybeBoundFunctionShapeWithDefaultProto()) {
      Rooted<SharedShape*> shape(
          cx, cx->global()->maybeBoundFunctionShapeWithDefaultProto());
      bound = NativeObject::create<BoundFunctionObject>(
          cx, allocKind, gc::Heap::Default, shape);
      if (!bound) {
        return nullptr;
      }
    } else {
      bound = NewObjectWithGivenProto<BoundFunctionObject>(cx, proto);
      if (!bound) {
        return nullptr;
      }
      if (!SharedShape::ensureInitialCustomShape<BoundFunctionObject>(cx,
                                                                      bound)) {
        return nullptr;
      }
    }
  }

  MOZ_ASSERT(bound->lookupPure(cx->names().length)->slot() == LengthSlot);
  MOZ_ASSERT(bound->lookupPure(cx->names().name)->slot() == NameSlot);

  // Steps 6 and 9.
  bound->initFlags(numBoundArgs, target->isConstructor());

  // Step 7.
  bound->initReservedSlot(TargetSlot, ObjectValue(*target));

  // Step 8.
  if (argc > 0) {
    bound->initReservedSlot(BoundThisSlot, args[0]);
  }

  if (numBoundArgs <= MaxInlineBoundArgs) {
    for (size_t i = 0; i < numBoundArgs; i++) {
      bound->initReservedSlot(BoundArg0Slot + i, args[i + 1]);
    }
  } else {
    ArrayObject* arr = NewDenseCopiedArray(cx, numBoundArgs, args + 1);
    if (!arr) {
      return nullptr;
    }
    bound->initReservedSlot(BoundArg0Slot, ObjectValue(*arr));
  }

  // ES2023 20.2.3.2 Function.prototype.bind
  // Step 4.
  double length = 0.0;

  // Steps 5-6.
  if (!ComputeLengthValue(cx, bound, target, numBoundArgs, &length)) {
    return nullptr;
  }

  // Step 7.
  bound->initLength(length);

  // Steps 8-9.
  JSAtom* name = ComputeNameValue(cx, bound, target);
  if (!name) {
    return nullptr;
  }

  // Step 10.
  bound->initName(name);

  // Step 11.
  return bound;
}

// static
BoundFunctionObject* BoundFunctionObject::createWithTemplate(
    JSContext* cx, Handle<BoundFunctionObject*> templateObj) {
  Rooted<SharedShape*> shape(cx, templateObj->sharedShape());
  auto* bound = NativeObject::create<BoundFunctionObject>(
      cx, allocKind, gc::Heap::Default, shape);
  if (!bound) {
    return nullptr;
  }
  bound->initFlags(templateObj->numBoundArgs(), templateObj->isConstructor());
  bound->initLength(templateObj->getLengthForInitialShape().toInt32());
  bound->initName(&templateObj->getNameForInitialShape().toString()->asAtom());
  return bound;
}

// static
BoundFunctionObject* BoundFunctionObject::functionBindSpecializedBaseline(
    JSContext* cx, Handle<JSObject*> target, Value* args, uint32_t argc,
    Handle<BoundFunctionObject*> templateObj) {
  // Root the Values on the stack.
  RootedExternalValueArray argsRoot(cx, argc, args);

  MOZ_ASSERT(target->is<JSFunction>() || target->is<BoundFunctionObject>());
  MOZ_ASSERT(target->isCallable());
  MOZ_ASSERT(target->isConstructor() == templateObj->isConstructor());
  MOZ_ASSERT(target->staticPrototype() == templateObj->staticPrototype());

  size_t numBoundArgs = argc > 0 ? argc - 1 : 0;
  MOZ_ASSERT(numBoundArgs <= MaxInlineBoundArgs);

  BoundFunctionObject* bound = createWithTemplate(cx, templateObj);
  if (!bound) {
    return nullptr;
  }

  MOZ_ASSERT(bound->lookupPure(cx->names().length)->slot() == LengthSlot);
  MOZ_ASSERT(bound->lookupPure(cx->names().name)->slot() == NameSlot);

  bound->initReservedSlot(TargetSlot, ObjectValue(*target));
  if (argc > 0) {
    bound->initReservedSlot(BoundThisSlot, args[0]);
  }
  for (size_t i = 0; i < numBoundArgs; i++) {
    bound->initReservedSlot(BoundArg0Slot + i, args[i + 1]);
  }
  return bound;
}

// static
BoundFunctionObject* BoundFunctionObject::createTemplateObject(JSContext* cx) {
  Rooted<JSObject*> proto(cx, &cx->global()->getFunctionPrototype());
  Rooted<BoundFunctionObject*> bound(
      cx, NewTenuredObjectWithGivenProto<BoundFunctionObject>(cx, proto));
  if (!bound) {
    return nullptr;
  }
  if (!SharedShape::ensureInitialCustomShape<BoundFunctionObject>(cx, bound)) {
    return nullptr;
  }
  return bound;
}

bool BoundFunctionObject::initTemplateSlotsForSpecializedBind(
    JSContext* cx, uint32_t numBoundArgs, bool targetIsConstructor,
    uint32_t targetLength, JSAtom* targetName) {
  size_t len = 0;
  if (targetLength > numBoundArgs) {
    len = targetLength - numBoundArgs;
  }

  JSAtom* name = AppendBoundFunctionPrefix(cx, targetName);
  if (!name) {
    return false;
  }

  initFlags(numBoundArgs, targetIsConstructor);
  initLength(len);
  initName(name);
  return true;
}

static const JSClassOps classOps = {
    nullptr,                         // addProperty
    nullptr,                         // delProperty
    nullptr,                         // enumerate
    nullptr,                         // newEnumerate
    nullptr,                         // resolve
    nullptr,                         // mayResolve
    nullptr,                         // finalize
    BoundFunctionObject::call,       // call
    BoundFunctionObject::construct,  // construct
    nullptr,                         // trace
};

static const ObjectOps objOps = {
    nullptr,                           // lookupProperty
    nullptr,                           // qdefineProperty
    nullptr,                           // hasProperty
    nullptr,                           // getProperty
    nullptr,                           // setProperty
    nullptr,                           // getOwnPropertyDescriptor
    nullptr,                           // deleteProperty
    nullptr,                           // getElements
    BoundFunctionObject::funToString,  // funToString
};

const JSClass BoundFunctionObject::class_ = {
    "BoundFunctionObject",
    // Note: bound functions don't have their own constructor or prototype (they
    // use the prototype of the target object), but we give them a JSProtoKey
    // because that's what Xray wrappers use to identify builtin objects.
    JSCLASS_HAS_CACHED_PROTO(JSProto_BoundFunction) |
        JSCLASS_HAS_RESERVED_SLOTS(BoundFunctionObject::SlotCount),
    &classOps,
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    &objOps,
};
