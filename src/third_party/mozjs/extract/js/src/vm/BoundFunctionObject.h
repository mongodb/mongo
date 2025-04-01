/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BoundFunctionObject_h
#define vm_BoundFunctionObject_h

#include "jstypes.h"

#include "gc/Policy.h"
#include "vm/ArrayObject.h"
#include "vm/JSObject.h"

namespace js {

// Implementation of Bound Function Exotic Objects.
// ES2023 10.4.1
// https://tc39.es/ecma262/#sec-bound-function-exotic-objects
class BoundFunctionObject : public NativeObject {
 public:
  static const JSClass class_;

  // FlagsSlot uses the low bit for the is-constructor flag and the other bits
  // for the number of arguments.
  static constexpr size_t IsConstructorFlag = 0b1;
  static constexpr size_t NumBoundArgsShift = 1;

  // The maximum number of bound arguments that can be stored inline in
  // BoundArg*Slot.
  static constexpr size_t MaxInlineBoundArgs = 3;

 private:
  enum {
    // The [[BoundTargetFunction]] (a callable object).
    TargetSlot,

    // The number of arguments + the is-constructor flag, stored as Int32Value.
    FlagsSlot,

    // The [[BoundThis]] Value.
    BoundThisSlot,

    // The [[BoundArguments]]. If numBoundArgs exceeds MaxInlineBoundArgs,
    // BoundArg0Slot will contain an array object that stores the values and the
    // other two slots will be unused.
    BoundArg0Slot,
    BoundArg1Slot,
    BoundArg2Slot,

    // Initial slots for the `length` and `name` own data properties. Note that
    // these properties are configurable, so these slots can be mutated when the
    // object is exposed to JS.
    LengthSlot,
    NameSlot,

    SlotCount
  };

  // The AllocKind should match SlotCount. See assertion in functionBindImpl.
  static constexpr gc::AllocKind allocKind = gc::AllocKind::OBJECT8_BACKGROUND;

  void initFlags(size_t numBoundArgs, bool isConstructor) {
    int32_t val = (numBoundArgs << NumBoundArgsShift) | isConstructor;
    initReservedSlot(FlagsSlot, Int32Value(val));
  }

 public:
  size_t numBoundArgs() const {
    int32_t v = getReservedSlot(FlagsSlot).toInt32();
    MOZ_ASSERT(v >= 0);
    return v >> NumBoundArgsShift;
  }
  bool isConstructor() const {
    int32_t v = getReservedSlot(FlagsSlot).toInt32();
    return v & IsConstructorFlag;
  }

  Value getTargetVal() const { return getReservedSlot(TargetSlot); }
  JSObject* getTarget() const { return &getTargetVal().toObject(); }

  Value getBoundThis() const { return getReservedSlot(BoundThisSlot); }

  Value getInlineBoundArg(size_t i) const {
    MOZ_ASSERT(i < numBoundArgs());
    MOZ_ASSERT(numBoundArgs() <= MaxInlineBoundArgs);
    return getReservedSlot(BoundArg0Slot + i);
  }
  ArrayObject* getBoundArgsArray() const {
    MOZ_ASSERT(numBoundArgs() > MaxInlineBoundArgs);
    return &getReservedSlot(BoundArg0Slot).toObject().as<ArrayObject>();
  }
  Value getBoundArg(size_t i) const {
    MOZ_ASSERT(i < numBoundArgs());
    if (numBoundArgs() <= MaxInlineBoundArgs) {
      return getInlineBoundArg(i);
    }
    return getBoundArgsArray()->getDenseElement(i);
  }

  void initLength(double len) {
    MOZ_ASSERT(getReservedSlot(LengthSlot).isUndefined());
    initReservedSlot(LengthSlot, NumberValue(len));
  }
  void initName(JSAtom* name) {
    MOZ_ASSERT(getReservedSlot(NameSlot).isUndefined());
    initReservedSlot(NameSlot, StringValue(name));
  }

  // Get the `length` and `name` property values when the object has the
  // original shape. See comment for LengthSlot and NameSlot.
  Value getLengthForInitialShape() const { return getReservedSlot(LengthSlot); }
  Value getNameForInitialShape() const { return getReservedSlot(NameSlot); }

  // The [[Call]] and [[Construct]] hooks.
  static bool call(JSContext* cx, unsigned argc, Value* vp);
  static bool construct(JSContext* cx, unsigned argc, Value* vp);

  // The JSFunToStringOp implementation for Function.prototype.toString.
  static JSString* funToString(JSContext* cx, Handle<JSObject*> obj,
                               bool isToSource);

  // Implementation of Function.prototype.bind.
  static bool functionBind(JSContext* cx, unsigned argc, Value* vp);

  static SharedShape* assignInitialShape(JSContext* cx,
                                         Handle<BoundFunctionObject*> obj);

  static BoundFunctionObject* functionBindImpl(
      JSContext* cx, Handle<JSObject*> target, Value* args, uint32_t argc,
      Handle<BoundFunctionObject*> maybeBound);

  static BoundFunctionObject* createWithTemplate(
      JSContext* cx, Handle<BoundFunctionObject*> templateObj);
  static BoundFunctionObject* functionBindSpecializedBaseline(
      JSContext* cx, Handle<JSObject*> target, Value* args, uint32_t argc,
      Handle<BoundFunctionObject*> templateObj);

  static BoundFunctionObject* createTemplateObject(JSContext* cx);

  bool initTemplateSlotsForSpecializedBind(JSContext* cx, uint32_t numBoundArgs,
                                           bool targetIsConstructor,
                                           uint32_t targetLength,
                                           JSAtom* targetName);

  static constexpr size_t offsetOfTargetSlot() {
    return getFixedSlotOffset(TargetSlot);
  }
  static constexpr size_t offsetOfFlagsSlot() {
    return getFixedSlotOffset(FlagsSlot);
  }
  static constexpr size_t offsetOfBoundThisSlot() {
    return getFixedSlotOffset(BoundThisSlot);
  }
  static constexpr size_t offsetOfFirstInlineBoundArg() {
    return getFixedSlotOffset(BoundArg0Slot);
  }
  static constexpr size_t offsetOfLengthSlot() {
    return getFixedSlotOffset(LengthSlot);
  }
  static constexpr size_t offsetOfNameSlot() {
    return getFixedSlotOffset(NameSlot);
  }

  static constexpr size_t targetSlot() { return TargetSlot; }
  static constexpr size_t boundThisSlot() { return BoundThisSlot; }
  static constexpr size_t firstInlineBoundArgSlot() { return BoundArg0Slot; }
};

};  // namespace js

#endif /* vm_BoundFunctionObject_h */
