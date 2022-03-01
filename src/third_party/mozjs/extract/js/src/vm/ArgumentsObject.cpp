/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ArgumentsObject-inl.h"

#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"

#include <algorithm>

#include "gc/FreeOp.h"
#include "jit/CalleeToken.h"
#include "jit/JitFrames.h"
#include "util/BitArray.h"
#include "vm/AsyncFunction.h"
#include "vm/GlobalObject.h"
#include "vm/Stack.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "gc/Nursery-inl.h"
#include "vm/FrameIter-inl.h"  // js::FrameIter::unaliasedForEachActual
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

/* static */
size_t RareArgumentsData::bytesRequired(size_t numActuals) {
  size_t extraBytes = NumWordsForBitArrayOfLength(numActuals) * sizeof(size_t);
  return offsetof(RareArgumentsData, deletedBits_) + extraBytes;
}

/* static */
RareArgumentsData* RareArgumentsData::create(JSContext* cx,
                                             ArgumentsObject* obj) {
  size_t bytes = RareArgumentsData::bytesRequired(obj->initialLength());

  uint8_t* data = AllocateObjectBuffer<uint8_t>(cx, obj, bytes);
  if (!data) {
    return nullptr;
  }

  mozilla::PodZero(data, bytes);

  AddCellMemory(obj, bytes, MemoryUse::RareArgumentsData);

  return new (data) RareArgumentsData();
}

bool ArgumentsObject::createRareData(JSContext* cx) {
  MOZ_ASSERT(!data()->rareData);

  RareArgumentsData* rareData = RareArgumentsData::create(cx, this);
  if (!rareData) {
    return false;
  }

  data()->rareData = rareData;
  markElementOverridden();
  return true;
}

bool ArgumentsObject::markElementDeleted(JSContext* cx, uint32_t i) {
  RareArgumentsData* data = getOrCreateRareData(cx);
  if (!data) {
    return false;
  }

  data->markElementDeleted(initialLength(), i);
  return true;
}

static void CopyStackFrameArguments(const AbstractFramePtr frame,
                                    GCPtrValue* dst, unsigned totalArgs) {
  MOZ_ASSERT_IF(frame.isInterpreterFrame(),
                !frame.asInterpreterFrame()->runningInJit());

  MOZ_ASSERT(std::max(frame.numActualArgs(), frame.numFormalArgs()) ==
             totalArgs);

  /* Copy arguments. */
  Value* src = frame.argv();
  Value* end = src + totalArgs;
  while (src != end) {
    (dst++)->init(*src++);
  }
}

/* static */
void ArgumentsObject::MaybeForwardToCallObject(AbstractFramePtr frame,
                                               ArgumentsObject* obj,
                                               ArgumentsData* data) {
  JSScript* script = frame.script();
  if (frame.callee()->needsCallObject() && script->argsObjAliasesFormals()) {
    obj->initFixedSlot(MAYBE_CALL_SLOT, ObjectValue(frame.callObj()));
    for (PositionalFormalParameterIter fi(script); fi; fi++) {
      if (fi.closedOver()) {
        data->args[fi.argumentSlot()] = MagicEnvSlotValue(fi.location().slot());
        obj->markArgumentForwarded();
      }
    }
  }
}

/* static */
void ArgumentsObject::MaybeForwardToCallObject(JSFunction* callee,
                                               JSObject* callObj,
                                               ArgumentsObject* obj,
                                               ArgumentsData* data) {
  JSScript* script = callee->nonLazyScript();
  if (callee->needsCallObject() && script->argsObjAliasesFormals()) {
    MOZ_ASSERT(callObj && callObj->is<CallObject>());
    obj->initFixedSlot(MAYBE_CALL_SLOT, ObjectValue(*callObj));
    for (PositionalFormalParameterIter fi(script); fi; fi++) {
      if (fi.closedOver()) {
        data->args[fi.argumentSlot()] = MagicEnvSlotValue(fi.location().slot());
        obj->markArgumentForwarded();
      }
    }
  }
}

struct CopyFrameArgs {
  AbstractFramePtr frame_;

  explicit CopyFrameArgs(AbstractFramePtr frame) : frame_(frame) {}

  void copyArgs(JSContext*, GCPtrValue* dst, unsigned totalArgs) const {
    CopyStackFrameArguments(frame_, dst, totalArgs);
  }

  /*
   * If a call object exists and the arguments object aliases formals, the
   * call object is the canonical location for formals.
   */
  void maybeForwardToCallObject(ArgumentsObject* obj, ArgumentsData* data) {
    ArgumentsObject::MaybeForwardToCallObject(frame_, obj, data);
  }
};

struct CopyJitFrameArgs {
  jit::JitFrameLayout* frame_;
  HandleObject callObj_;

  CopyJitFrameArgs(jit::JitFrameLayout* frame, HandleObject callObj)
      : frame_(frame), callObj_(callObj) {}

  void copyArgs(JSContext*, GCPtrValue* dstBase, unsigned totalArgs) const {
    unsigned numActuals = frame_->numActualArgs();
    unsigned numFormals =
        jit::CalleeTokenToFunction(frame_->calleeToken())->nargs();
    MOZ_ASSERT(numActuals <= totalArgs);
    MOZ_ASSERT(numFormals <= totalArgs);
    MOZ_ASSERT(std::max(numActuals, numFormals) == totalArgs);

    /* Copy all arguments. */
    Value* src = frame_->argv() + 1; /* +1 to skip this. */
    Value* end = src + numActuals;
    GCPtrValue* dst = dstBase;
    while (src != end) {
      (dst++)->init(*src++);
    }

    if (numActuals < numFormals) {
      GCPtrValue* dstEnd = dstBase + totalArgs;
      while (dst != dstEnd) {
        (dst++)->init(UndefinedValue());
      }
    }
  }

  /*
   * If a call object exists and the arguments object aliases formals, the
   * call object is the canonical location for formals.
   */
  void maybeForwardToCallObject(ArgumentsObject* obj, ArgumentsData* data) {
    JSFunction* callee = jit::CalleeTokenToFunction(frame_->calleeToken());
    ArgumentsObject::MaybeForwardToCallObject(callee, callObj_, obj, data);
  }
};

struct CopyScriptFrameIterArgs {
  ScriptFrameIter& iter_;

  explicit CopyScriptFrameIterArgs(ScriptFrameIter& iter) : iter_(iter) {}

  void copyArgs(JSContext* cx, GCPtrValue* dstBase, unsigned totalArgs) const {
    /* Copy actual arguments. */
    iter_.unaliasedForEachActual(cx, CopyToHeap(dstBase));

    /* Define formals which are not part of the actuals. */
    unsigned numActuals = iter_.numActualArgs();
    unsigned numFormals = iter_.calleeTemplate()->nargs();
    MOZ_ASSERT(numActuals <= totalArgs);
    MOZ_ASSERT(numFormals <= totalArgs);
    MOZ_ASSERT(std::max(numActuals, numFormals) == totalArgs);

    if (numActuals < numFormals) {
      GCPtrValue* dst = dstBase + numActuals;
      GCPtrValue* dstEnd = dstBase + totalArgs;
      while (dst != dstEnd) {
        (dst++)->init(UndefinedValue());
      }
    }
  }

  /*
   * Ion frames are copying every argument onto the stack, other locations are
   * invalid.
   */
  void maybeForwardToCallObject(ArgumentsObject* obj, ArgumentsData* data) {
    if (!iter_.isIon()) {
      ArgumentsObject::MaybeForwardToCallObject(iter_.abstractFramePtr(), obj,
                                                data);
    }
  }
};

struct CopyInlinedArgs {
  HandleValueArray args_;
  HandleObject callObj_;
  HandleFunction callee_;
  uint32_t numActuals_;

  CopyInlinedArgs(HandleValueArray args, HandleObject callObj,
                  HandleFunction callee, uint32_t numActuals)
      : args_(args),
        callObj_(callObj),
        callee_(callee),
        numActuals_(numActuals) {}

  void copyArgs(JSContext*, GCPtrValue* dstBase, unsigned totalArgs) const {
    uint32_t numFormals = callee_->nargs();
    MOZ_ASSERT(std::max(numActuals_, numFormals) == totalArgs);

    // Copy actual arguments.
    GCPtrValue* dst = dstBase;
    for (uint32_t i = 0; i < numActuals_; i++) {
      (dst++)->init(args_[i]);
    }

    // Fill in missing arguments with |undefined|.
    if (numActuals_ < numFormals) {
      GCPtrValue* dstEnd = dstBase + totalArgs;
      while (dst != dstEnd) {
        (dst++)->init(UndefinedValue());
      }
    }
  }

  /*
   * If a call object exists and the arguments object aliases formals, the
   * call object is the canonical location for formals.
   */
  void maybeForwardToCallObject(ArgumentsObject* obj, ArgumentsData* data) {
    ArgumentsObject::MaybeForwardToCallObject(callee_, callObj_, obj, data);
  }
};

ArgumentsObject* ArgumentsObject::createTemplateObject(JSContext* cx,
                                                       bool mapped) {
  const JSClass* clasp = mapped ? &MappedArgumentsObject::class_
                                : &UnmappedArgumentsObject::class_;

  RootedObject proto(
      cx, GlobalObject::getOrCreateObjectPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  constexpr ObjectFlags objectFlags = {ObjectFlag::Indexed};
  RootedShape shape(cx, SharedShape::getInitialShape(
                            cx, clasp, cx->realm(), TaggedProto(proto),
                            FINALIZE_KIND, objectFlags));
  if (!shape) {
    return nullptr;
  }

  AutoSetNewObjectMetadata metadata(cx);
  JSObject* base;
  JS_TRY_VAR_OR_RETURN_NULL(
      cx, base,
      NativeObject::create(cx, FINALIZE_KIND, gc::TenuredHeap, shape));

  ArgumentsObject* obj = &base->as<js::ArgumentsObject>();
  obj->initFixedSlot(ArgumentsObject::DATA_SLOT, PrivateValue(nullptr));
  return obj;
}

ArgumentsObject* Realm::maybeArgumentsTemplateObject(bool mapped) const {
  return mapped ? mappedArgumentsTemplate_ : unmappedArgumentsTemplate_;
}

ArgumentsObject* Realm::getOrCreateArgumentsTemplateObject(JSContext* cx,
                                                           bool mapped) {
  WeakHeapPtr<ArgumentsObject*>& obj =
      mapped ? mappedArgumentsTemplate_ : unmappedArgumentsTemplate_;

  ArgumentsObject* templateObj = obj;
  if (templateObj) {
    return templateObj;
  }

  templateObj = ArgumentsObject::createTemplateObject(cx, mapped);
  if (!templateObj) {
    return nullptr;
  }

  obj.set(templateObj);
  return templateObj;
}

template <typename CopyArgs>
/* static */
ArgumentsObject* ArgumentsObject::create(JSContext* cx, HandleFunction callee,
                                         unsigned numActuals, CopyArgs& copy) {
  bool mapped = callee->baseScript()->hasMappedArgsObj();
  ArgumentsObject* templateObj =
      cx->realm()->getOrCreateArgumentsTemplateObject(cx, mapped);
  if (!templateObj) {
    return nullptr;
  }

  RootedShape shape(cx, templateObj->shape());

  unsigned numFormals = callee->nargs();
  unsigned numArgs = std::max(numActuals, numFormals);
  unsigned numBytes = ArgumentsData::bytesRequired(numArgs);

  Rooted<ArgumentsObject*> obj(cx);
  ArgumentsData* data = nullptr;
  {
    // The copyArgs call below can allocate objects, so add this block scope
    // to make sure we set the metadata for this arguments object first.
    AutoSetNewObjectMetadata metadata(cx);

    JSObject* base;
    JS_TRY_VAR_OR_RETURN_NULL(
        cx, base,
        NativeObject::create(cx, FINALIZE_KIND, gc::DefaultHeap, shape));
    obj = &base->as<ArgumentsObject>();

    data = reinterpret_cast<ArgumentsData*>(
        AllocateObjectBuffer<uint8_t>(cx, obj, numBytes));
    if (!data) {
      // Make the object safe for GC.
      obj->initFixedSlot(DATA_SLOT, PrivateValue(nullptr));
      return nullptr;
    }

    data->numArgs = numArgs;
    data->rareData = nullptr;

    // Initialize |args| with a pattern that is safe for GC tracing.
    for (unsigned i = 0; i < numArgs; i++) {
      data->args[i].init(UndefinedValue());
    }

    InitReservedSlot(obj, DATA_SLOT, data, numBytes, MemoryUse::ArgumentsData);
    obj->initFixedSlot(CALLEE_SLOT, ObjectValue(*callee));
  }
  MOZ_ASSERT(data != nullptr);

  /* Copy [0, numArgs) into data->slots. */
  copy.copyArgs(cx, data->args, numArgs);

  obj->initFixedSlot(INITIAL_LENGTH_SLOT,
                     Int32Value(numActuals << PACKED_BITS_COUNT));

  copy.maybeForwardToCallObject(obj, data);

  MOZ_ASSERT(obj->initialLength() == numActuals);
  MOZ_ASSERT(!obj->hasOverriddenLength());
  return obj;
}

ArgumentsObject* ArgumentsObject::createExpected(JSContext* cx,
                                                 AbstractFramePtr frame) {
  MOZ_ASSERT(frame.script()->needsArgsObj());
  RootedFunction callee(cx, frame.callee());
  CopyFrameArgs copy(frame);
  ArgumentsObject* argsobj = create(cx, callee, frame.numActualArgs(), copy);
  if (!argsobj) {
    return nullptr;
  }

  frame.initArgsObj(*argsobj);
  return argsobj;
}

ArgumentsObject* ArgumentsObject::createUnexpected(JSContext* cx,
                                                   ScriptFrameIter& iter) {
  RootedFunction callee(cx, iter.callee(cx));
  CopyScriptFrameIterArgs copy(iter);
  return create(cx, callee, iter.numActualArgs(), copy);
}

ArgumentsObject* ArgumentsObject::createUnexpected(JSContext* cx,
                                                   AbstractFramePtr frame) {
  RootedFunction callee(cx, frame.callee());
  CopyFrameArgs copy(frame);
  return create(cx, callee, frame.numActualArgs(), copy);
}

ArgumentsObject* ArgumentsObject::createForIon(JSContext* cx,
                                               jit::JitFrameLayout* frame,
                                               HandleObject scopeChain) {
  jit::CalleeToken token = frame->calleeToken();
  MOZ_ASSERT(jit::CalleeTokenIsFunction(token));
  RootedFunction callee(cx, jit::CalleeTokenToFunction(token));
  RootedObject callObj(
      cx, scopeChain->is<CallObject>() ? scopeChain.get() : nullptr);
  CopyJitFrameArgs copy(frame, callObj);
  return create(cx, callee, frame->numActualArgs(), copy);
}

/* static */
ArgumentsObject* ArgumentsObject::createFromValueArray(
    JSContext* cx, HandleValueArray argsArray, HandleFunction callee,
    HandleObject scopeChain, uint32_t numActuals) {
  MOZ_ASSERT(numActuals <= MaxInlinedArgs);
  RootedObject callObj(
      cx, scopeChain->is<CallObject>() ? scopeChain.get() : nullptr);
  CopyInlinedArgs copy(argsArray, callObj, callee, numActuals);
  return create(cx, callee, numActuals, copy);
}

/* static */
ArgumentsObject* ArgumentsObject::createForInlinedIon(JSContext* cx,
                                                      Value* args,
                                                      HandleFunction callee,
                                                      HandleObject scopeChain,
                                                      uint32_t numActuals) {
  RootedExternalValueArray rootedArgs(cx, numActuals, args);
  HandleValueArray argsArray =
      HandleValueArray::fromMarkedLocation(numActuals, args);

  return createFromValueArray(cx, argsArray, callee, scopeChain, numActuals);
}

/* static */
ArgumentsObject* ArgumentsObject::finishForIonPure(JSContext* cx,
                                                   jit::JitFrameLayout* frame,
                                                   JSObject* scopeChain,
                                                   ArgumentsObject* obj) {
  // JIT code calls this directly (no callVM), because it's faster, so we're
  // not allowed to GC in here.
  AutoUnsafeCallWithABI unsafe;

  JSFunction* callee = jit::CalleeTokenToFunction(frame->calleeToken());
  RootedObject callObj(cx, scopeChain->is<CallObject>() ? scopeChain : nullptr);
  CopyJitFrameArgs copy(frame, callObj);

  unsigned numActuals = frame->numActualArgs();
  unsigned numFormals = callee->nargs();
  unsigned numArgs = std::max(numActuals, numFormals);
  unsigned numBytes = ArgumentsData::bytesRequired(numArgs);

  ArgumentsData* data = reinterpret_cast<ArgumentsData*>(
      AllocateObjectBuffer<uint8_t>(cx, obj, numBytes));
  if (!data) {
    // Make the object safe for GC. Don't report OOM, the slow path will
    // retry the allocation.
    cx->recoverFromOutOfMemory();
    obj->initFixedSlot(DATA_SLOT, PrivateValue(nullptr));
    return nullptr;
  }

  data->numArgs = numArgs;
  data->rareData = nullptr;

  obj->initFixedSlot(INITIAL_LENGTH_SLOT,
                     Int32Value(numActuals << PACKED_BITS_COUNT));
  obj->initFixedSlot(DATA_SLOT, PrivateValue(data));
  AddCellMemory(obj, numBytes, MemoryUse::ArgumentsData);
  obj->initFixedSlot(MAYBE_CALL_SLOT, UndefinedValue());
  obj->initFixedSlot(CALLEE_SLOT, ObjectValue(*callee));

  copy.copyArgs(cx, data->args, numArgs);

  if (callObj && callee->needsCallObject()) {
    copy.maybeForwardToCallObject(obj, data);
  }

  MOZ_ASSERT(obj->initialLength() == numActuals);
  MOZ_ASSERT(!obj->hasOverriddenLength());
  return obj;
}

/* static */
bool ArgumentsObject::obj_delProperty(JSContext* cx, HandleObject obj,
                                      HandleId id, ObjectOpResult& result) {
  ArgumentsObject& argsobj = obj->as<ArgumentsObject>();
  if (JSID_IS_INT(id)) {
    unsigned arg = unsigned(JSID_TO_INT(id));
    if (arg < argsobj.initialLength() && !argsobj.isElementDeleted(arg)) {
      if (!argsobj.markElementDeleted(cx, arg)) {
        return false;
      }
    }
  } else if (id.isAtom(cx->names().length)) {
    argsobj.markLengthOverridden();
  } else if (id.isAtom(cx->names().callee)) {
    argsobj.as<MappedArgumentsObject>().markCalleeOverridden();
  } else if (id.isWellKnownSymbol(JS::SymbolCode::iterator)) {
    argsobj.markIteratorOverridden();
  }
  return result.succeed();
}

/* static */
bool ArgumentsObject::obj_mayResolve(const JSAtomState& names, jsid id,
                                     JSObject*) {
  // Arguments might resolve indexes, Symbol.iterator, or length/callee.
  if (id.isAtom()) {
    JSAtom* atom = id.toAtom();
    return atom->isIndex() || atom == names.length || atom == names.callee;
  }

  return id.isInt() || id.isWellKnownSymbol(JS::SymbolCode::iterator);
}

bool js::MappedArgGetter(JSContext* cx, HandleObject obj, HandleId id,
                         MutableHandleValue vp) {
  MappedArgumentsObject& argsobj = obj->as<MappedArgumentsObject>();
  if (JSID_IS_INT(id)) {
    /*
     * arg can exceed the number of arguments if a script changed the
     * prototype to point to another Arguments object with a bigger argc.
     */
    unsigned arg = unsigned(JSID_TO_INT(id));
    if (arg < argsobj.initialLength() && !argsobj.isElementDeleted(arg)) {
      vp.set(argsobj.element(arg));
    }
  } else if (id.isAtom(cx->names().length)) {
    if (!argsobj.hasOverriddenLength()) {
      vp.setInt32(argsobj.initialLength());
    }
  } else {
    MOZ_ASSERT(id.isAtom(cx->names().callee));
    if (!argsobj.hasOverriddenCallee()) {
      vp.setObject(argsobj.callee());
    }
  }
  return true;
}

bool js::MappedArgSetter(JSContext* cx, HandleObject obj, HandleId id,
                         HandleValue v, ObjectOpResult& result) {
  Handle<MappedArgumentsObject*> argsobj = obj.as<MappedArgumentsObject>();

  Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, argsobj, id, &desc)) {
    return false;
  }
  MOZ_ASSERT(desc.isSome());
  MOZ_ASSERT(desc->isDataDescriptor());
  MOZ_ASSERT(desc->writable());
  MOZ_ASSERT(!desc->resolving());

  if (id.isInt()) {
    unsigned arg = unsigned(id.toInt());
    if (arg < argsobj->initialLength() && !argsobj->isElementDeleted(arg)) {
      argsobj->setElement(arg, v);
      return result.succeed();
    }
  } else {
    MOZ_ASSERT(id.isAtom(cx->names().length) || id.isAtom(cx->names().callee));
  }

  /*
   * For simplicity we use delete/define to replace the property with a
   * simple data property. Note that we rely on ArgumentsObject::obj_delProperty
   * to set the corresponding override-bit.
   * Note also that we must define the property instead of setting it in case
   * the user has changed the prototype to an object that has a setter for
   * this id.
   */
  Rooted<PropertyDescriptor> desc_(cx, *desc);
  desc_.setValue(v);
  ObjectOpResult ignored;
  return NativeDeleteProperty(cx, argsobj, id, ignored) &&
         NativeDefineProperty(cx, argsobj, id, desc_, result);
}

/* static */
bool ArgumentsObject::getArgumentsIterator(JSContext* cx,
                                           MutableHandleValue val) {
  HandlePropertyName shName = cx->names().ArrayValues;
  RootedAtom name(cx, cx->names().values);
  return GlobalObject::getSelfHostedFunction(cx, cx->global(), shName, name, 0,
                                             val);
}

/* static */
bool ArgumentsObject::reifyLength(JSContext* cx, Handle<ArgumentsObject*> obj) {
  if (obj->hasOverriddenLength()) {
    return true;
  }

  RootedId id(cx, NameToId(cx->names().length));
  RootedValue val(cx, Int32Value(obj->initialLength()));
  if (!NativeDefineDataProperty(cx, obj, id, val, JSPROP_RESOLVING)) {
    return false;
  }

  obj->markLengthOverridden();
  return true;
}

/* static */
bool ArgumentsObject::reifyIterator(JSContext* cx,
                                    Handle<ArgumentsObject*> obj) {
  if (obj->hasOverriddenIterator()) {
    return true;
  }

  RootedId iteratorId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
  RootedValue val(cx);
  if (!ArgumentsObject::getArgumentsIterator(cx, &val)) {
    return false;
  }
  if (!NativeDefineDataProperty(cx, obj, iteratorId, val, JSPROP_RESOLVING)) {
    return false;
  }

  obj->markIteratorOverridden();
  return true;
}

static bool ResolveArgumentsProperty(JSContext* cx,
                                     Handle<ArgumentsObject*> obj, HandleId id,
                                     PropertyFlags flags, bool* resolvedp) {
  // Note: we don't need to call ReshapeForShadowedProp here because we're just
  // resolving an existing property instead of defining a new property.

  MOZ_ASSERT(id.isInt() || id.isAtom(cx->names().length) ||
             id.isAtom(cx->names().callee));
  MOZ_ASSERT(flags.isCustomDataProperty());

  if (!NativeObject::addCustomDataProperty(cx, obj, id, flags)) {
    return false;
  }

  *resolvedp = true;
  return true;
}

/* static */
bool MappedArgumentsObject::obj_resolve(JSContext* cx, HandleObject obj,
                                        HandleId id, bool* resolvedp) {
  Rooted<MappedArgumentsObject*> argsobj(cx, &obj->as<MappedArgumentsObject>());

  if (id.isWellKnownSymbol(JS::SymbolCode::iterator)) {
    if (argsobj->hasOverriddenIterator()) {
      return true;
    }

    if (!reifyIterator(cx, argsobj)) {
      return false;
    }
    *resolvedp = true;
    return true;
  }

  PropertyFlags flags = {PropertyFlag::CustomDataProperty,
                         PropertyFlag::Configurable, PropertyFlag::Writable};
  if (JSID_IS_INT(id)) {
    uint32_t arg = uint32_t(JSID_TO_INT(id));
    if (arg >= argsobj->initialLength() || argsobj->isElementDeleted(arg)) {
      return true;
    }

    flags.setFlag(PropertyFlag::Enumerable);
  } else if (id.isAtom(cx->names().length)) {
    if (argsobj->hasOverriddenLength()) {
      return true;
    }
  } else {
    if (!id.isAtom(cx->names().callee)) {
      return true;
    }

    if (argsobj->hasOverriddenCallee()) {
      return true;
    }
  }

  return ResolveArgumentsProperty(cx, argsobj, id, flags, resolvedp);
}

/* static */
bool MappedArgumentsObject::obj_enumerate(JSContext* cx, HandleObject obj) {
  Rooted<MappedArgumentsObject*> argsobj(cx, &obj->as<MappedArgumentsObject>());

  RootedId id(cx);
  bool found;

  // Trigger reflection.
  id = NameToId(cx->names().length);
  if (!HasOwnProperty(cx, argsobj, id, &found)) {
    return false;
  }

  id = NameToId(cx->names().callee);
  if (!HasOwnProperty(cx, argsobj, id, &found)) {
    return false;
  }

  id = SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator);
  if (!HasOwnProperty(cx, argsobj, id, &found)) {
    return false;
  }

  for (unsigned i = 0; i < argsobj->initialLength(); i++) {
    id = INT_TO_JSID(i);
    if (!HasOwnProperty(cx, argsobj, id, &found)) {
      return false;
    }
  }

  return true;
}

static bool DefineMappedIndex(JSContext* cx, Handle<MappedArgumentsObject*> obj,
                              HandleId id,
                              MutableHandle<PropertyDescriptor> desc,
                              ObjectOpResult& result) {
  // The custom data properties (see MappedArgGetter, MappedArgSetter) have to
  // be (re)defined manually because PropertyDescriptor and NativeDefineProperty
  // don't support these special properties.
  //
  // This exists in order to let JS code change the configurable/enumerable
  // attributes for these properties.
  //
  // Note: because this preserves the default mapped-arguments behavior, we
  // don't need to mark elements as overridden or deleted.

  MOZ_ASSERT(id.isInt());
  MOZ_ASSERT(!obj->isElementDeleted(id.toInt()));
  MOZ_ASSERT(!obj->containsDenseElement(id.toInt()));

  MOZ_ASSERT(!desc.isAccessorDescriptor());

  // Mapped properties aren't used when defining a non-writable property.
  MOZ_ASSERT(!desc.hasWritable() || desc.writable());

  // First, resolve the property to simplify the code below.
  PropertyResult prop;
  if (!NativeLookupOwnProperty<CanGC>(cx, obj, id, &prop)) {
    return false;
  }

  MOZ_ASSERT(prop.isNativeProperty());

  PropertyInfo propInfo = prop.propertyInfo();
  MOZ_ASSERT(propInfo.writable());
  MOZ_ASSERT(propInfo.isCustomDataProperty());

  // Change the property's attributes by implementing the relevant parts of
  // ValidateAndApplyPropertyDescriptor (ES2021 draft, 10.1.6.3), in particular
  // steps 4 and 9.

  // Determine whether the property should be configurable and/or enumerable.
  bool configurable = propInfo.configurable();
  bool enumerable = propInfo.enumerable();
  if (configurable) {
    if (desc.hasConfigurable()) {
      configurable = desc.configurable();
    }
    if (desc.hasEnumerable()) {
      enumerable = desc.enumerable();
    }
  } else {
    // Property is not configurable so disallow any attribute changes.
    if ((desc.hasConfigurable() && desc.configurable()) ||
        (desc.hasEnumerable() && enumerable != desc.enumerable())) {
      return result.fail(JSMSG_CANT_REDEFINE_PROP);
    }
  }

  PropertyFlags flags = propInfo.flags();
  flags.setFlag(PropertyFlag::Configurable, configurable);
  flags.setFlag(PropertyFlag::Enumerable, enumerable);
  if (!NativeObject::changeCustomDataPropAttributes(cx, obj, id, flags)) {
    return false;
  }

  return result.succeed();
}

// ES 2017 draft 9.4.4.2
/* static */
bool MappedArgumentsObject::obj_defineProperty(JSContext* cx, HandleObject obj,
                                               HandleId id,
                                               Handle<PropertyDescriptor> desc,
                                               ObjectOpResult& result) {
  // Step 1.
  Rooted<MappedArgumentsObject*> argsobj(cx, &obj->as<MappedArgumentsObject>());

  // Steps 2-3.
  bool isMapped = false;
  if (JSID_IS_INT(id)) {
    unsigned arg = unsigned(JSID_TO_INT(id));
    isMapped =
        arg < argsobj->initialLength() && !argsobj->isElementDeleted(arg);
  }

  // Step 4.
  Rooted<PropertyDescriptor> newArgDesc(cx, desc);

  // Step 5.
  bool defineMapped = false;
  if (!desc.isAccessorDescriptor() && isMapped) {
    // Step 5.a.
    if (desc.hasWritable() && !desc.writable()) {
      if (!desc.hasValue()) {
        RootedValue v(cx, argsobj->element(JSID_TO_INT(id)));
        newArgDesc.setValue(v);
      }
    } else {
      // In this case the live mapping is supposed to keep working.
      defineMapped = true;
    }
  }

  // Step 6. NativeDefineProperty will lookup [[Value]] for us.
  if (defineMapped) {
    if (!DefineMappedIndex(cx, argsobj, id, &newArgDesc, result)) {
      return false;
    }
  } else {
    if (!NativeDefineProperty(cx, obj.as<NativeObject>(), id, newArgDesc,
                              result)) {
      return false;
    }
  }
  // Step 7.
  if (!result.ok()) {
    return true;
  }

  // Step 8.
  if (isMapped) {
    unsigned arg = unsigned(JSID_TO_INT(id));
    if (desc.isAccessorDescriptor()) {
      if (!argsobj->markElementDeleted(cx, arg)) {
        return false;
      }
    } else {
      if (desc.hasValue()) {
        argsobj->setElement(arg, desc.value());
      }
      if (desc.hasWritable() && !desc.writable()) {
        if (!argsobj->markElementDeleted(cx, arg)) {
          return false;
        }
      }
    }
  }

  // Step 9.
  return result.succeed();
}

bool js::UnmappedArgGetter(JSContext* cx, HandleObject obj, HandleId id,
                           MutableHandleValue vp) {
  UnmappedArgumentsObject& argsobj = obj->as<UnmappedArgumentsObject>();

  if (JSID_IS_INT(id)) {
    /*
     * arg can exceed the number of arguments if a script changed the
     * prototype to point to another Arguments object with a bigger argc.
     */
    unsigned arg = unsigned(JSID_TO_INT(id));
    if (arg < argsobj.initialLength() && !argsobj.isElementDeleted(arg)) {
      vp.set(argsobj.element(arg));
    }
  } else {
    MOZ_ASSERT(id.isAtom(cx->names().length));
    if (!argsobj.hasOverriddenLength()) {
      vp.setInt32(argsobj.initialLength());
    }
  }
  return true;
}

bool js::UnmappedArgSetter(JSContext* cx, HandleObject obj, HandleId id,
                           HandleValue v, ObjectOpResult& result) {
  Handle<UnmappedArgumentsObject*> argsobj = obj.as<UnmappedArgumentsObject>();

  Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, argsobj, id, &desc)) {
    return false;
  }
  MOZ_ASSERT(desc.isSome());
  MOZ_ASSERT(desc->isDataDescriptor());
  MOZ_ASSERT(desc->writable());
  MOZ_ASSERT(!desc->resolving());

  if (id.isInt()) {
    unsigned arg = unsigned(id.toInt());
    if (arg < argsobj->initialLength()) {
      argsobj->setElement(arg, v);
      return result.succeed();
    }
  } else {
    MOZ_ASSERT(id.isAtom(cx->names().length));
  }

  /*
   * For simplicity we use delete/define to replace the property with a
   * simple data property. Note that we rely on ArgumentsObject::obj_delProperty
   * to set the corresponding override-bit.
   */
  Rooted<PropertyDescriptor> desc_(cx, *desc);
  desc_.setValue(v);
  ObjectOpResult ignored;
  return NativeDeleteProperty(cx, argsobj, id, ignored) &&
         NativeDefineProperty(cx, argsobj, id, desc_, result);
}

/* static */
bool UnmappedArgumentsObject::obj_resolve(JSContext* cx, HandleObject obj,
                                          HandleId id, bool* resolvedp) {
  Rooted<UnmappedArgumentsObject*> argsobj(cx,
                                           &obj->as<UnmappedArgumentsObject>());

  if (id.isWellKnownSymbol(JS::SymbolCode::iterator)) {
    if (argsobj->hasOverriddenIterator()) {
      return true;
    }

    if (!reifyIterator(cx, argsobj)) {
      return false;
    }
    *resolvedp = true;
    return true;
  }

  if (id.isAtom(cx->names().callee)) {
    RootedObject throwTypeError(
        cx, GlobalObject::getOrCreateThrowTypeError(cx, cx->global()));
    if (!throwTypeError) {
      return false;
    }

    unsigned attrs = JSPROP_RESOLVING | JSPROP_PERMANENT;
    if (!NativeDefineAccessorProperty(cx, argsobj, id, throwTypeError,
                                      throwTypeError, attrs)) {
      return false;
    }

    *resolvedp = true;
    return true;
  }

  PropertyFlags flags = {PropertyFlag::CustomDataProperty,
                         PropertyFlag::Configurable, PropertyFlag::Writable};
  if (JSID_IS_INT(id)) {
    uint32_t arg = uint32_t(JSID_TO_INT(id));
    if (arg >= argsobj->initialLength() || argsobj->isElementDeleted(arg)) {
      return true;
    }

    flags.setFlag(PropertyFlag::Enumerable);
  } else if (id.isAtom(cx->names().length)) {
    if (argsobj->hasOverriddenLength()) {
      return true;
    }
  } else {
    return true;
  }

  return ResolveArgumentsProperty(cx, argsobj, id, flags, resolvedp);
}

/* static */
bool UnmappedArgumentsObject::obj_enumerate(JSContext* cx, HandleObject obj) {
  Rooted<UnmappedArgumentsObject*> argsobj(cx,
                                           &obj->as<UnmappedArgumentsObject>());

  RootedId id(cx);
  bool found;

  // Trigger reflection.
  id = NameToId(cx->names().length);
  if (!HasOwnProperty(cx, argsobj, id, &found)) {
    return false;
  }

  id = NameToId(cx->names().callee);
  if (!HasOwnProperty(cx, argsobj, id, &found)) {
    return false;
  }

  id = SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator);
  if (!HasOwnProperty(cx, argsobj, id, &found)) {
    return false;
  }

  for (unsigned i = 0; i < argsobj->initialLength(); i++) {
    id = INT_TO_JSID(i);
    if (!HasOwnProperty(cx, argsobj, id, &found)) {
      return false;
    }
  }

  return true;
}

void ArgumentsObject::finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(!IsInsideNursery(obj));
  ArgumentsObject& argsobj = obj->as<ArgumentsObject>();
  if (argsobj.data()) {
    fop->free_(&argsobj, argsobj.maybeRareData(),
               RareArgumentsData::bytesRequired(argsobj.initialLength()),
               MemoryUse::RareArgumentsData);
    fop->free_(&argsobj, argsobj.data(),
               ArgumentsData::bytesRequired(argsobj.data()->numArgs),
               MemoryUse::ArgumentsData);
  }
}

void ArgumentsObject::trace(JSTracer* trc, JSObject* obj) {
  ArgumentsObject& argsobj = obj->as<ArgumentsObject>();
  if (ArgumentsData* data =
          argsobj.data()) {  // Template objects have no ArgumentsData.
    TraceRange(trc, data->numArgs, data->begin(), js_arguments_str);
  }
}

/* static */
size_t ArgumentsObject::objectMoved(JSObject* dst, JSObject* src) {
  ArgumentsObject* ndst = &dst->as<ArgumentsObject>();
  const ArgumentsObject* nsrc = &src->as<ArgumentsObject>();
  MOZ_ASSERT(ndst->data() == nsrc->data());

  if (!IsInsideNursery(src)) {
    return 0;
  }

  Nursery& nursery = dst->runtimeFromMainThread()->gc.nursery();

  size_t nbytesTotal = 0;
  uint32_t nDataBytes = ArgumentsData::bytesRequired(nsrc->data()->numArgs);
  if (!nursery.isInside(nsrc->data())) {
    nursery.removeMallocedBufferDuringMinorGC(nsrc->data());
  } else {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    uint8_t* data = nsrc->zone()->pod_malloc<uint8_t>(nDataBytes);
    if (!data) {
      oomUnsafe.crash(
          "Failed to allocate ArgumentsObject data while tenuring.");
    }
    ndst->initFixedSlot(DATA_SLOT, PrivateValue(data));

    mozilla::PodCopy(data, reinterpret_cast<uint8_t*>(nsrc->data()),
                     nDataBytes);
    nbytesTotal += nDataBytes;
  }

  AddCellMemory(ndst, nDataBytes, MemoryUse::ArgumentsData);

  if (RareArgumentsData* srcRareData = nsrc->maybeRareData()) {
    uint32_t nbytes = RareArgumentsData::bytesRequired(nsrc->initialLength());
    if (!nursery.isInside(srcRareData)) {
      nursery.removeMallocedBufferDuringMinorGC(srcRareData);
    } else {
      AutoEnterOOMUnsafeRegion oomUnsafe;
      uint8_t* dstRareData = nsrc->zone()->pod_malloc<uint8_t>(nbytes);
      if (!dstRareData) {
        oomUnsafe.crash(
            "Failed to allocate RareArgumentsData data while tenuring.");
      }
      ndst->data()->rareData = (RareArgumentsData*)dstRareData;

      mozilla::PodCopy(dstRareData, reinterpret_cast<uint8_t*>(srcRareData),
                       nbytes);
      nbytesTotal += nbytes;
    }

    AddCellMemory(ndst, nbytes, MemoryUse::RareArgumentsData);
  }

  return nbytesTotal;
}

/*
 * The classes below collaborate to lazily reflect and synchronize actual
 * argument values, argument count, and callee function object stored in a
 * stack frame with their corresponding property values in the frame's
 * arguments object.
 */
const JSClassOps MappedArgumentsObject::classOps_ = {
    nullptr,                               // addProperty
    ArgumentsObject::obj_delProperty,      // delProperty
    MappedArgumentsObject::obj_enumerate,  // enumerate
    nullptr,                               // newEnumerate
    MappedArgumentsObject::obj_resolve,    // resolve
    ArgumentsObject::obj_mayResolve,       // mayResolve
    ArgumentsObject::finalize,             // finalize
    nullptr,                               // call
    nullptr,                               // hasInstance
    nullptr,                               // construct
    ArgumentsObject::trace,                // trace
};

const js::ClassExtension MappedArgumentsObject::classExt_ = {
    ArgumentsObject::objectMoved,  // objectMovedOp
};

const ObjectOps MappedArgumentsObject::objectOps_ = {
    nullptr,                                    // lookupProperty
    MappedArgumentsObject::obj_defineProperty,  // defineProperty
    nullptr,                                    // hasProperty
    nullptr,                                    // getProperty
    nullptr,                                    // setProperty
    nullptr,                                    // getOwnPropertyDescriptor
    nullptr,                                    // deleteProperty
    nullptr,                                    // getElements
    nullptr,                                    // funToString
};

const JSClass MappedArgumentsObject::class_ = {
    "Arguments",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(MappedArgumentsObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Object) |
        JSCLASS_SKIP_NURSERY_FINALIZE | JSCLASS_BACKGROUND_FINALIZE,
    &MappedArgumentsObject::classOps_,
    nullptr,
    &MappedArgumentsObject::classExt_,
    &MappedArgumentsObject::objectOps_};

/*
 * Unmapped arguments is significantly less magical than mapped arguments, so
 * it is represented by a different class while sharing some functionality.
 */
const JSClassOps UnmappedArgumentsObject::classOps_ = {
    nullptr,                                 // addProperty
    ArgumentsObject::obj_delProperty,        // delProperty
    UnmappedArgumentsObject::obj_enumerate,  // enumerate
    nullptr,                                 // newEnumerate
    UnmappedArgumentsObject::obj_resolve,    // resolve
    ArgumentsObject::obj_mayResolve,         // mayResolve
    ArgumentsObject::finalize,               // finalize
    nullptr,                                 // call
    nullptr,                                 // hasInstance
    nullptr,                                 // construct
    ArgumentsObject::trace,                  // trace
};

const js::ClassExtension UnmappedArgumentsObject::classExt_ = {
    ArgumentsObject::objectMoved,  // objectMovedOp
};

const JSClass UnmappedArgumentsObject::class_ = {
    "Arguments",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(UnmappedArgumentsObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Object) |
        JSCLASS_SKIP_NURSERY_FINALIZE | JSCLASS_BACKGROUND_FINALIZE,
    &UnmappedArgumentsObject::classOps_, nullptr,
    &UnmappedArgumentsObject::classExt_};
