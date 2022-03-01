/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/CacheIR.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"

#include "jsmath.h"

#include "builtin/DataViewObject.h"
#include "builtin/MapObject.h"
#include "builtin/ModuleObject.h"
#include "builtin/Object.h"
#include "jit/BaselineCacheIRCompiler.h"
#include "jit/BaselineIC.h"
#include "jit/CacheIRSpewer.h"
#include "jit/InlinableNatives.h"
#include "jit/Ion.h"  // IsIonEnabled
#include "jit/JitContext.h"
#include "jit/JitRuntime.h"
#include "js/experimental/JitInfo.h"  // JSJitInfo
#include "js/friend/DOMProxy.h"       // JS::ExpandoAndGeneration
#include "js/friend/WindowProxy.h"  // js::IsWindow, js::IsWindowProxy, js::ToWindowIfWindowProxy
#include "js/friend/XrayJitInfo.h"  // js::jit::GetXrayJitInfo, JS::XrayJitInfo
#include "js/GCAPI.h"               // JS::AutoSuppressGCAnalysis
#include "js/RegExpFlags.h"         // JS::RegExpFlags
#include "js/ScalarType.h"          // js::Scalar::Type
#include "js/Wrapper.h"
#include "proxy/DOMProxy.h"  // js::GetDOMProxyHandlerFamily
#include "util/Unicode.h"
#include "vm/ArrayBufferObject.h"
#include "vm/BytecodeUtil.h"
#include "vm/Iteration.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/ProxyObject.h"
#include "vm/RegExpObject.h"
#include "vm/SelfHosting.h"
#include "vm/ThrowMsgKind.h"  // ThrowCondition
#include "wasm/WasmInstance.h"

#include "jit/BaselineFrame-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/StringObject-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;
using mozilla::Maybe;

using JS::DOMProxyShadowsResult;
using JS::ExpandoAndGeneration;

const char* const js::jit::CacheKindNames[] = {
#define DEFINE_KIND(kind) #kind,
    CACHE_IR_KINDS(DEFINE_KIND)
#undef DEFINE_KIND
};

const char* const js::jit::CacheIROpNames[] = {
#define OPNAME(op, ...) #op,
    CACHE_IR_OPS(OPNAME)
#undef OPNAME
};

const CacheIROpInfo js::jit::CacheIROpInfos[] = {
#define OPINFO(op, len, transpile, ...) {len, transpile},
    CACHE_IR_OPS(OPINFO)
#undef OPINFO
};

const uint32_t js::jit::CacheIROpHealth[] = {
#define OPHEALTH(op, len, transpile, health) health,
    CACHE_IR_OPS(OPHEALTH)
#undef OPHEALTH
};

#ifdef DEBUG
size_t js::jit::NumInputsForCacheKind(CacheKind kind) {
  switch (kind) {
    case CacheKind::NewArray:
    case CacheKind::NewObject:
    case CacheKind::GetIntrinsic:
      return 0;
    case CacheKind::GetProp:
    case CacheKind::TypeOf:
    case CacheKind::ToPropertyKey:
    case CacheKind::GetIterator:
    case CacheKind::ToBool:
    case CacheKind::UnaryArith:
    case CacheKind::GetName:
    case CacheKind::BindName:
    case CacheKind::Call:
    case CacheKind::OptimizeSpreadCall:
      return 1;
    case CacheKind::Compare:
    case CacheKind::GetElem:
    case CacheKind::GetPropSuper:
    case CacheKind::SetProp:
    case CacheKind::In:
    case CacheKind::HasOwn:
    case CacheKind::CheckPrivateField:
    case CacheKind::InstanceOf:
    case CacheKind::BinaryArith:
      return 2;
    case CacheKind::GetElemSuper:
    case CacheKind::SetElem:
      return 3;
  }
  MOZ_CRASH("Invalid kind");
}
#endif

#ifdef DEBUG
void CacheIRWriter::assertSameCompartment(JSObject* obj) {
  cx_->debugOnlyCheck(obj);
}
void CacheIRWriter::assertSameZone(Shape* shape) {
  MOZ_ASSERT(cx_->zone() == shape->zone());
}
#endif

StubField CacheIRWriter::readStubFieldForIon(uint32_t offset,
                                             StubField::Type type) const {
  size_t index = 0;
  size_t currentOffset = 0;

  // If we've seen an offset earlier than this before, we know we can start the
  // search there at least, otherwise, we start the search from the beginning.
  if (lastOffset_ < offset) {
    currentOffset = lastOffset_;
    index = lastIndex_;
  }

  while (currentOffset != offset) {
    currentOffset += StubField::sizeInBytes(stubFields_[index].type());
    index++;
    MOZ_ASSERT(index < stubFields_.length());
  }

  MOZ_ASSERT(stubFields_[index].type() == type);

  lastOffset_ = currentOffset;
  lastIndex_ = index;

  return stubFields_[index];
}

CacheIRCloner::CacheIRCloner(ICCacheIRStub* stub)
    : stubInfo_(stub->stubInfo()), stubData_(stub->stubDataStart()) {}

void CacheIRCloner::cloneOp(CacheOp op, CacheIRReader& reader,
                            CacheIRWriter& writer) {
  switch (op) {
#define DEFINE_OP(op, ...)     \
  case CacheOp::op:            \
    clone##op(reader, writer); \
    break;
    CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP
    default:
      MOZ_CRASH("Invalid op");
  }
}

uintptr_t CacheIRCloner::readStubWord(uint32_t offset) {
  return stubInfo_->getStubRawWord(stubData_, offset);
}
int64_t CacheIRCloner::readStubInt64(uint32_t offset) {
  return stubInfo_->getStubRawInt64(stubData_, offset);
}

Shape* CacheIRCloner::getShapeField(uint32_t stubOffset) {
  return reinterpret_cast<Shape*>(readStubWord(stubOffset));
}
GetterSetter* CacheIRCloner::getGetterSetterField(uint32_t stubOffset) {
  return reinterpret_cast<GetterSetter*>(readStubWord(stubOffset));
}
JSObject* CacheIRCloner::getObjectField(uint32_t stubOffset) {
  return reinterpret_cast<JSObject*>(readStubWord(stubOffset));
}
JSString* CacheIRCloner::getStringField(uint32_t stubOffset) {
  return reinterpret_cast<JSString*>(readStubWord(stubOffset));
}
JSAtom* CacheIRCloner::getAtomField(uint32_t stubOffset) {
  return reinterpret_cast<JSAtom*>(readStubWord(stubOffset));
}
PropertyName* CacheIRCloner::getPropertyNameField(uint32_t stubOffset) {
  return reinterpret_cast<PropertyName*>(readStubWord(stubOffset));
}
JS::Symbol* CacheIRCloner::getSymbolField(uint32_t stubOffset) {
  return reinterpret_cast<JS::Symbol*>(readStubWord(stubOffset));
}
BaseScript* CacheIRCloner::getBaseScriptField(uint32_t stubOffset) {
  return reinterpret_cast<BaseScript*>(readStubWord(stubOffset));
}
uint32_t CacheIRCloner::getRawInt32Field(uint32_t stubOffset) {
  return uint32_t(readStubWord(stubOffset));
}
const void* CacheIRCloner::getRawPointerField(uint32_t stubOffset) {
  return reinterpret_cast<const void*>(readStubWord(stubOffset));
}
uint64_t CacheIRCloner::getRawInt64Field(uint32_t stubOffset) {
  return static_cast<uint64_t>(readStubInt64(stubOffset));
}
gc::AllocSite* CacheIRCloner::getAllocSiteField(uint32_t stubOffset) {
  return reinterpret_cast<gc::AllocSite*>(readStubWord(stubOffset));
}

jsid CacheIRCloner::getIdField(uint32_t stubOffset) {
  return jsid::fromRawBits(readStubWord(stubOffset));
}
const Value CacheIRCloner::getValueField(uint32_t stubOffset) {
  return Value::fromRawBits(uint64_t(readStubInt64(stubOffset)));
}

IRGenerator::IRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                         CacheKind cacheKind, ICState state)
    : writer(cx),
      cx_(cx),
      script_(script),
      pc_(pc),
      cacheKind_(cacheKind),
      mode_(state.mode()),
      isFirstStub_(state.newStubIsFirstStub()) {}

GetPropIRGenerator::GetPropIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, ICState state,
                                       CacheKind cacheKind, HandleValue val,
                                       HandleValue idVal)
    : IRGenerator(cx, script, pc, cacheKind, state), val_(val), idVal_(idVal) {}

static void EmitLoadSlotResult(CacheIRWriter& writer, ObjOperandId holderId,
                               NativeObject* holder, PropertyInfo prop) {
  if (holder->isFixedSlot(prop.slot())) {
    writer.loadFixedSlotResult(holderId,
                               NativeObject::getFixedSlotOffset(prop.slot()));
  } else {
    size_t dynamicSlotOffset =
        holder->dynamicSlotIndex(prop.slot()) * sizeof(Value);
    writer.loadDynamicSlotResult(holderId, dynamicSlotOffset);
  }
}

// DOM proxies
// -----------
//
// DOM proxies are proxies that are used to implement various DOM objects like
// HTMLDocument and NodeList. DOM proxies may have an expando object - a native
// object that stores extra properties added to the object. The following
// CacheIR instructions are only used with DOM proxies:
//
// * LoadDOMExpandoValue: returns the Value in the proxy's expando slot. This
//   returns either an UndefinedValue (no expando), ObjectValue (the expando
//   object), or PrivateValue(ExpandoAndGeneration*).
//
// * LoadDOMExpandoValueGuardGeneration: guards the Value in the proxy's expando
//   slot is the same PrivateValue(ExpandoAndGeneration*), then guards on its
//   generation, then returns expandoAndGeneration->expando. This Value is
//   either an UndefinedValue or ObjectValue.
//
// * LoadDOMExpandoValueIgnoreGeneration: assumes the Value in the proxy's
//   expando slot is a PrivateValue(ExpandoAndGeneration*), unboxes it, and
//   returns the expandoAndGeneration->expando Value.
//
// * GuardDOMExpandoMissingOrGuardShape: takes an expando Value as input, then
//   guards it's either UndefinedValue or an object with the expected shape.

enum class ProxyStubType {
  None,
  DOMExpando,
  DOMShadowed,
  DOMUnshadowed,
  Generic
};

static bool IsCacheableDOMProxy(ProxyObject* obj) {
  const BaseProxyHandler* handler = obj->handler();
  if (handler->family() != GetDOMProxyHandlerFamily()) {
    return false;
  }

  // Some DOM proxies have dynamic prototypes.  We can't really cache those very
  // well.
  return obj->hasStaticPrototype();
}

static ProxyStubType GetProxyStubType(JSContext* cx, HandleObject obj,
                                      HandleId id) {
  if (!obj->is<ProxyObject>()) {
    return ProxyStubType::None;
  }
  auto proxy = obj.as<ProxyObject>();

  if (!IsCacheableDOMProxy(proxy)) {
    return ProxyStubType::Generic;
  }

  DOMProxyShadowsResult shadows = GetDOMProxyShadowsCheck()(cx, proxy, id);
  if (shadows == DOMProxyShadowsResult::ShadowCheckFailed) {
    cx->clearPendingException();
    return ProxyStubType::None;
  }

  if (DOMProxyIsShadowing(shadows)) {
    if (shadows == DOMProxyShadowsResult::ShadowsViaDirectExpando ||
        shadows == DOMProxyShadowsResult::ShadowsViaIndirectExpando) {
      return ProxyStubType::DOMExpando;
    }
    return ProxyStubType::DOMShadowed;
  }

  MOZ_ASSERT(shadows == DOMProxyShadowsResult::DoesntShadow ||
             shadows == DOMProxyShadowsResult::DoesntShadowUnique);
  return ProxyStubType::DOMUnshadowed;
}

static bool ValueToNameOrSymbolId(JSContext* cx, HandleValue idVal,
                                  MutableHandleId id, bool* nameOrSymbol) {
  *nameOrSymbol = false;

  if (!idVal.isString() && !idVal.isSymbol() && !idVal.isUndefined() &&
      !idVal.isNull()) {
    return true;
  }

  if (!PrimitiveValueToId<CanGC>(cx, idVal, id)) {
    return false;
  }

  if (!id.isAtom() && !id.isSymbol()) {
    id.set(JSID_VOID);
    return true;
  }

  if (id.isAtom() && id.toAtom()->isIndex()) {
    id.set(JSID_VOID);
    return true;
  }

  *nameOrSymbol = true;
  return true;
}

AttachDecision GetPropIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId valId(writer.setInputOperandId(0));
  if (cacheKind_ != CacheKind::GetProp) {
    MOZ_ASSERT_IF(cacheKind_ == CacheKind::GetPropSuper,
                  getSuperReceiverValueId().id() == 1);
    MOZ_ASSERT_IF(cacheKind_ != CacheKind::GetPropSuper,
                  getElemKeyValueId().id() == 1);
    writer.setInputOperandId(1);
  }
  if (cacheKind_ == CacheKind::GetElemSuper) {
    MOZ_ASSERT(getSuperReceiverValueId().id() == 2);
    writer.setInputOperandId(2);
  }

  RootedId id(cx_);
  bool nameOrSymbol;
  if (!ValueToNameOrSymbolId(cx_, idVal_, &id, &nameOrSymbol)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  // |super.prop| getter calls use a |this| value that differs from lookup
  // object.
  ValOperandId receiverId = isSuper() ? getSuperReceiverValueId() : valId;

  if (val_.isObject()) {
    RootedObject obj(cx_, &val_.toObject());
    ObjOperandId objId = writer.guardToObject(valId);
    if (nameOrSymbol) {
      TRY_ATTACH(tryAttachObjectLength(obj, objId, id));
      TRY_ATTACH(tryAttachTypedArray(obj, objId, id));
      TRY_ATTACH(tryAttachDataView(obj, objId, id));
      TRY_ATTACH(tryAttachArrayBufferMaybeShared(obj, objId, id));
      TRY_ATTACH(tryAttachRegExp(obj, objId, id));
      TRY_ATTACH(tryAttachNative(obj, objId, id, receiverId));
      TRY_ATTACH(tryAttachModuleNamespace(obj, objId, id));
      TRY_ATTACH(tryAttachWindowProxy(obj, objId, id));
      TRY_ATTACH(tryAttachCrossCompartmentWrapper(obj, objId, id));
      TRY_ATTACH(
          tryAttachXrayCrossCompartmentWrapper(obj, objId, id, receiverId));
      TRY_ATTACH(tryAttachFunction(obj, objId, id));
      TRY_ATTACH(tryAttachArgumentsObjectIterator(obj, objId, id));
      TRY_ATTACH(tryAttachArgumentsObjectCallee(obj, objId, id));
      TRY_ATTACH(tryAttachProxy(obj, objId, id, receiverId));

      trackAttached(IRGenerator::NotAttached);
      return AttachDecision::NoAction;
    }

    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem ||
               cacheKind_ == CacheKind::GetElemSuper);

    TRY_ATTACH(tryAttachProxyElement(obj, objId));
    TRY_ATTACH(tryAttachTypedArrayElement(obj, objId));

    uint32_t index;
    Int32OperandId indexId;
    if (maybeGuardInt32Index(idVal_, getElemKeyValueId(), &index, &indexId)) {
      TRY_ATTACH(tryAttachDenseElement(obj, objId, index, indexId));
      TRY_ATTACH(tryAttachDenseElementHole(obj, objId, index, indexId));
      TRY_ATTACH(tryAttachSparseElement(obj, objId, index, indexId));
      TRY_ATTACH(tryAttachArgumentsObjectArg(obj, objId, index, indexId));
      TRY_ATTACH(tryAttachGenericElement(obj, objId, index, indexId));

      trackAttached(IRGenerator::NotAttached);
      return AttachDecision::NoAction;
    }

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  if (nameOrSymbol) {
    TRY_ATTACH(tryAttachPrimitive(valId, id));
    TRY_ATTACH(tryAttachStringLength(valId, id));

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  if (idVal_.isInt32()) {
    ValOperandId indexId = getElemKeyValueId();
    TRY_ATTACH(tryAttachStringChar(valId, indexId));

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

#ifdef DEBUG
// Any property lookups performed when trying to attach ICs must be pure, i.e.
// must use LookupPropertyPure() or similar functions. Pure lookups are
// guaranteed to never modify the prototype chain. This ensures that the holder
// object can always be found on the prototype chain.
static bool IsCacheableProtoChain(NativeObject* obj, NativeObject* holder) {
  while (obj != holder) {
    JSObject* proto = obj->staticPrototype();
    if (!proto || !proto->is<NativeObject>()) {
      return false;
    }
    obj = &proto->as<NativeObject>();
  }
  return true;
}
#endif

static bool IsCacheableGetPropReadSlot(NativeObject* obj, NativeObject* holder,
                                       PropertyInfo prop) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  return prop.isDataProperty();
}

enum NativeGetPropCacheability {
  CanAttachNone,
  CanAttachReadSlot,
  CanAttachNativeGetter,
  CanAttachScriptedGetter,
};

static NativeGetPropCacheability IsCacheableGetPropCall(NativeObject* obj,
                                                        NativeObject* holder,
                                                        PropertyInfo prop) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  if (!prop.isAccessorProperty()) {
    return CanAttachNone;
  }

  JSObject* getterObject = holder->getGetter(prop);
  if (!getterObject || !getterObject->is<JSFunction>()) {
    return CanAttachNone;
  }

  JSFunction& getter = getterObject->as<JSFunction>();

  if (getter.isClassConstructor()) {
    return CanAttachNone;
  }

  // For getters that need the WindowProxy (instead of the Window) as this
  // object, don't cache if obj is the Window, since our cache will pass that
  // instead of the WindowProxy.
  if (IsWindow(obj)) {
    // Check for a getter that has jitinfo and whose jitinfo says it's
    // OK with both inner and outer objects.
    if (!getter.hasJitInfo() || getter.jitInfo()->needsOuterizedThisObject()) {
      return CanAttachNone;
    }
  }

  // Scripted functions and natives with JIT entry can use the scripted path.
  if (getter.hasJitEntry()) {
    return CanAttachScriptedGetter;
  }

  MOZ_ASSERT(getter.isNativeWithoutJitEntry());
  return CanAttachNativeGetter;
}

static bool CheckHasNoSuchOwnProperty(JSContext* cx, JSObject* obj, jsid id) {
  if (!obj->is<NativeObject>()) {
    return false;
  }
  // Don't handle objects with resolve hooks.
  if (ClassMayResolveId(cx->names(), obj->getClass(), id, obj)) {
    return false;
  }
  if (obj->as<NativeObject>().contains(cx, id)) {
    return false;
  }
  return true;
}

static bool CheckHasNoSuchProperty(JSContext* cx, JSObject* obj, jsid id) {
  JSObject* curObj = obj;
  do {
    if (!CheckHasNoSuchOwnProperty(cx, curObj, id)) {
      return false;
    }

    curObj = curObj->staticPrototype();
  } while (curObj);

  return true;
}

static bool IsCacheableNoProperty(JSContext* cx, NativeObject* obj,
                                  NativeObject* holder, jsid id,
                                  jsbytecode* pc) {
  MOZ_ASSERT(!holder);

  // If we're doing a name lookup, we have to throw a ReferenceError.
  if (JSOp(*pc) == JSOp::GetBoundName) {
    return false;
  }

  return CheckHasNoSuchProperty(cx, obj, id);
}

static NativeGetPropCacheability CanAttachNativeGetProp(
    JSContext* cx, JSObject* obj, PropertyKey id, NativeObject** holder,
    Maybe<PropertyInfo>* propInfo, jsbytecode* pc) {
  MOZ_ASSERT(id.isString() || id.isSymbol());
  MOZ_ASSERT(!*holder);

  // The lookup needs to be universally pure, otherwise we risk calling hooks
  // out of turn. We don't mind doing this even when purity isn't required,
  // because we only miss out on shape hashification, which is only a temporary
  // perf cost. The limits were arbitrarily set, anyways.
  NativeObject* baseHolder = nullptr;
  PropertyResult prop;
  if (!LookupPropertyPure(cx, obj, id, &baseHolder, &prop)) {
    return CanAttachNone;
  }
  auto* nobj = &obj->as<NativeObject>();

  if (prop.isNativeProperty()) {
    MOZ_ASSERT(baseHolder);
    *holder = baseHolder;
    *propInfo = mozilla::Some(prop.propertyInfo());

    if (IsCacheableGetPropReadSlot(nobj, *holder, propInfo->ref())) {
      return CanAttachReadSlot;
    }

    return IsCacheableGetPropCall(nobj, *holder, propInfo->ref());
  }

  if (!prop.isFound()) {
    if (IsCacheableNoProperty(cx, nobj, *holder, id, pc)) {
      return CanAttachReadSlot;
    }
  }

  return CanAttachNone;
}

static void GuardReceiverProto(CacheIRWriter& writer, NativeObject* obj,
                               ObjOperandId objId) {
  // Note: we guard on the actual prototype and not on the shape because this is
  // used for sparse elements where we expect shape changes.

  if (JSObject* proto = obj->staticPrototype()) {
    writer.guardProto(objId, proto);
  } else {
    writer.guardNullProto(objId);
  }
}

// Guard that a given object has same class and same OwnProperties (excluding
// dense elements and dynamic properties).
static void TestMatchingNativeReceiver(CacheIRWriter& writer, NativeObject* obj,
                                       ObjOperandId objId) {
  writer.guardShapeForOwnProperties(objId, obj->shape());
}

// Similar to |TestMatchingNativeReceiver|, but specialized for ProxyObject.
static void TestMatchingProxyReceiver(CacheIRWriter& writer, ProxyObject* obj,
                                      ObjOperandId objId) {
  writer.guardShapeForClass(objId, obj->shape());
}

static bool ProtoChainSupportsTeleporting(JSObject* obj, NativeObject* holder) {
  // The receiver should already have been handled since its checks are always
  // required.
  MOZ_ASSERT(obj->isUsedAsPrototype());

  // Prototype chain must have cacheable prototypes to ensure the cached
  // holder is the current holder.
  for (JSObject* tmp = obj; tmp != holder; tmp = tmp->staticPrototype()) {
    if (tmp->hasUncacheableProto()) {
      return false;
    }
  }

  // The holder itself only gets reshaped by teleportation if it is not
  // marked UncacheableProto. See: ReshapeForProtoMutation.
  return !holder->hasUncacheableProto();
}

static void GeneratePrototypeGuards(CacheIRWriter& writer, JSObject* obj,
                                    NativeObject* holder, ObjOperandId objId) {
  // Assuming target property is on |holder|, generate appropriate guards to
  // ensure |holder| is still on the prototype chain of |obj| and we haven't
  // introduced any shadowing definitions.
  //
  // For each item in the proto chain before holder, we must ensure that
  // [[GetPrototypeOf]] still has the expected result, and that
  // [[GetOwnProperty]] has no definition of the target property.
  //
  //
  // [SMDOC] Shape Teleporting Optimization
  // ------------------------------
  //
  // Starting with the assumption (and guideline to developers) that mutating
  // prototypes is an uncommon and fair-to-penalize operation we move cost
  // from the access side to the mutation side.
  //
  // Consider the following proto chain, with B defining a property 'x':
  //
  //      D  ->  C  ->  B{x: 3}  ->  A  -> null
  //
  // When accessing |D.x| we refer to D as the "receiver", and B as the
  // "holder". To optimize this access we need to ensure that neither D nor C
  // has since defined a shadowing property 'x'. Since C is a prototype that
  // we assume is rarely mutated we would like to avoid checking each time if
  // new properties are added. To do this we require that everytime C is
  // mutated that in addition to generating a new shape for itself, it will
  // walk the proto chain and generate new shapes for those objects on the
  // chain (B and A). As a result, checking the shape of D and B is
  // sufficient. Note that we do not care if the shape or properties of A
  // change since the lookup of 'x' will stop at B.
  //
  // The second condition we must verify is that the prototype chain was not
  // mutated. The same mechanism as above is used. When the prototype link is
  // changed, we generate a new shape for the object. If the object whose
  // link we are mutating is itself a prototype, we regenerate shapes down
  // the chain by setting the UncacheableProto flag on them. This means the same
  // two shape checks as above are sufficient.
  //
  // Once the UncacheableProto flag is set, it means the shape will no longer be
  // updated by ReshapeForProtoMutation. If any shape from receiver to holder
  // (inclusive) is UncacheableProto, we don't apply the optimization.
  //
  // See:
  //  - ReshapeForProtoMutation
  //  - ReshapeForShadowedProp

  MOZ_ASSERT(holder);
  MOZ_ASSERT(obj != holder);

  // Receiver guards (see TestMatchingReceiver) ensure the receiver's proto is
  // unchanged so peel off the receiver.
  JSObject* pobj = obj->staticPrototype();
  MOZ_ASSERT(pobj->isUsedAsPrototype());

  // If teleporting is supported for this prototype chain, we are done.
  if (ProtoChainSupportsTeleporting(pobj, holder)) {
    return;
  }

  // If already at the holder, no further proto checks are needed.
  if (pobj == holder) {
    return;
  }

  // Synchronize pobj and protoId.
  MOZ_ASSERT(pobj == obj->staticPrototype());
  ObjOperandId protoId = writer.loadProto(objId);

  // Guard prototype links from |pobj| to |holder|.
  while (pobj != holder) {
    pobj = pobj->staticPrototype();

    // The object's proto could be nullptr so we must use GuardProto before
    // LoadProto (LoadProto asserts the proto is non-null).
    writer.guardProto(protoId, pobj);
    protoId = writer.loadProto(protoId);
  }
}

static void GeneratePrototypeHoleGuards(CacheIRWriter& writer,
                                        NativeObject* obj, ObjOperandId objId,
                                        bool alwaysGuardFirstProto) {
  if (alwaysGuardFirstProto) {
    GuardReceiverProto(writer, obj, objId);
  }

  JSObject* pobj = obj->staticPrototype();
  while (pobj) {
    ObjOperandId protoId = writer.loadObject(pobj);

    // Make sure the shape matches, to ensure the proto is unchanged and to
    // avoid non-dense elements or anything else that is being checked by
    // CanAttachDenseElementHole.
    MOZ_ASSERT(pobj->is<NativeObject>());
    writer.guardShape(protoId, pobj->shape());

    // Also make sure there are no dense elements.
    writer.guardNoDenseElements(protoId);

    pobj = pobj->staticPrototype();
  }
}

// Similar to |TestMatchingReceiver|, but for the holder object (when it
// differs from the receiver). The holder may also be the expando of the
// receiver if it exists.
static void TestMatchingHolder(CacheIRWriter& writer, NativeObject* obj,
                               ObjOperandId objId) {
  // The GeneratePrototypeGuards + TestMatchingHolder checks only support
  // prototype chains composed of NativeObject (excluding the receiver
  // itself).
  writer.guardShapeForOwnProperties(objId, obj->shape());
}

// Emit a shape guard for all objects on the proto chain. This does NOT include
// the receiver; callers must ensure the receiver's proto is the first proto by
// either emitting a shape guard or a prototype guard for |objId|.
//
// Note: this relies on shape implying proto.
static void ShapeGuardProtoChain(CacheIRWriter& writer, NativeObject* obj,
                                 ObjOperandId objId) {
  while (true) {
    JSObject* proto = obj->staticPrototype();
    if (!proto) {
      return;
    }

    obj = &proto->as<NativeObject>();
    objId = writer.loadProto(objId);

    writer.guardShape(objId, obj->shape());
  }
}

// For cross compartment guards we shape-guard the prototype chain to avoid
// referencing the holder object.
//
// This peels off the first layer because it's guarded against obj == holder.
static void ShapeGuardProtoChainForCrossCompartmentHolder(
    CacheIRWriter& writer, NativeObject* obj, ObjOperandId objId,
    NativeObject* holder, Maybe<ObjOperandId>* holderId) {
  MOZ_ASSERT(obj != holder);
  MOZ_ASSERT(holder);
  while (true) {
    MOZ_ASSERT(obj->staticPrototype());
    obj = &obj->staticPrototype()->as<NativeObject>();

    objId = writer.loadProto(objId);
    if (obj == holder) {
      TestMatchingHolder(writer, obj, objId);
      holderId->emplace(objId);
      return;
    } else {
      writer.guardShapeForOwnProperties(objId, obj->shape());
    }
  }
}

enum class SlotReadType { Normal, CrossCompartment };

template <SlotReadType MaybeCrossCompartment = SlotReadType::Normal>
static void EmitReadSlotGuard(CacheIRWriter& writer, NativeObject* obj,
                              NativeObject* holder, ObjOperandId objId,
                              Maybe<ObjOperandId>* holderId) {
  TestMatchingNativeReceiver(writer, obj, objId);

  if (obj != holder) {
    if (holder) {
      if (MaybeCrossCompartment == SlotReadType::CrossCompartment) {
        // Guard proto chain integrity.
        // We use a variant of guards that avoid baking in any cross-compartment
        // object pointers.
        ShapeGuardProtoChainForCrossCompartmentHolder(writer, obj, objId,
                                                      holder, holderId);
      } else {
        // Guard proto chain integrity.
        GeneratePrototypeGuards(writer, obj, holder, objId);

        // Guard on the holder's shape.
        holderId->emplace(writer.loadObject(holder));
        TestMatchingHolder(writer, holder, holderId->ref());
      }
    } else {
      // The property does not exist. Guard on everything in the prototype
      // chain. This is guaranteed to see only Native objects because of
      // CanAttachNativeGetProp().
      ShapeGuardProtoChain(writer, obj, objId);
    }
  } else {
    holderId->emplace(objId);
  }
}

template <SlotReadType MaybeCrossCompartment = SlotReadType::Normal>
static void EmitReadSlotResult(CacheIRWriter& writer, NativeObject* obj,
                               NativeObject* holder, Maybe<PropertyInfo> prop,
                               ObjOperandId objId) {
  Maybe<ObjOperandId> holderId;
  EmitReadSlotGuard<MaybeCrossCompartment>(writer, obj, holder, objId,
                                           &holderId);

  // Slot access.
  if (holder) {
    MOZ_ASSERT(holderId->valid());
    EmitLoadSlotResult(writer, *holderId, holder, *prop);
  } else {
    MOZ_ASSERT(holderId.isNothing());
    writer.loadUndefinedResult();
  }
}

static void EmitCallGetterResultNoGuards(JSContext* cx, CacheIRWriter& writer,
                                         NativeObject* obj,
                                         NativeObject* holder,
                                         PropertyInfo prop,
                                         ValOperandId receiverId) {
  JSFunction* target = &holder->getGetter(prop)->as<JSFunction>();
  bool sameRealm = cx->realm() == target->realm();

  switch (IsCacheableGetPropCall(obj, holder, prop)) {
    case CanAttachNativeGetter: {
      writer.callNativeGetterResult(receiverId, target, sameRealm);
      writer.returnFromIC();
      break;
    }
    case CanAttachScriptedGetter: {
      writer.callScriptedGetterResult(receiverId, target, sameRealm);
      writer.returnFromIC();
      break;
    }
    default:
      // CanAttachNativeGetProp guarantees that the getter is either a native or
      // a scripted function.
      MOZ_ASSERT_UNREACHABLE("Can't attach getter");
      break;
  }
}

// See the SMDOC comment in vm/GetterSetter.h for more info on Getter/Setter
// properties
static void EmitGuardGetterSetterSlot(CacheIRWriter& writer,
                                      NativeObject* holder, PropertyInfo prop,
                                      ObjOperandId holderId,
                                      bool holderIsConstant = false) {
  // If the holder is guaranteed to be the same object, and it never had a
  // slot holding a GetterSetter mutated or deleted, its Shape will change when
  // that does happen so we don't need to guard on the GetterSetter.
  if (holderIsConstant && !holder->hadGetterSetterChange()) {
    return;
  }

  size_t slot = prop.slot();
  Value slotVal = holder->getSlot(slot);
  MOZ_ASSERT(slotVal.isPrivateGCThing());

  if (holder->isFixedSlot(slot)) {
    size_t offset = NativeObject::getFixedSlotOffset(slot);
    writer.guardFixedSlotValue(holderId, offset, slotVal);
  } else {
    size_t offset = holder->dynamicSlotIndex(slot) * sizeof(Value);
    writer.guardDynamicSlotValue(holderId, offset, slotVal);
  }
}

static void EmitCallGetterResultGuards(CacheIRWriter& writer, NativeObject* obj,
                                       NativeObject* holder, HandleId id,
                                       PropertyInfo prop, ObjOperandId objId,
                                       ICState::Mode mode) {
  // Use the megamorphic guard if we're in megamorphic mode, except if |obj|
  // is a Window as GuardHasGetterSetter doesn't support this yet (Window may
  // require outerizing).

  MOZ_ASSERT(holder->containsPure(id, prop));

  if (mode == ICState::Mode::Specialized || IsWindow(obj)) {
    TestMatchingNativeReceiver(writer, obj, objId);

    if (obj != holder) {
      GeneratePrototypeGuards(writer, obj, holder, objId);

      // Guard on the holder's shape.
      ObjOperandId holderId = writer.loadObject(holder);
      TestMatchingHolder(writer, holder, holderId);

      EmitGuardGetterSetterSlot(writer, holder, prop, holderId,
                                /* holderIsConstant = */ true);
    } else {
      EmitGuardGetterSetterSlot(writer, holder, prop, objId);
    }
  } else {
    GetterSetter* gs = holder->getGetterSetter(prop);
    writer.guardHasGetterSetter(objId, id, gs);
  }
}

static void EmitCallGetterResult(JSContext* cx, CacheIRWriter& writer,
                                 NativeObject* obj, NativeObject* holder,
                                 HandleId id, PropertyInfo prop,
                                 ObjOperandId objId, ValOperandId receiverId,
                                 ICState::Mode mode) {
  EmitCallGetterResultGuards(writer, obj, holder, id, prop, objId, mode);
  EmitCallGetterResultNoGuards(cx, writer, obj, holder, prop, receiverId);
}

static bool CanAttachDOMCall(JSContext* cx, JSJitInfo::OpType type,
                             JSObject* obj, JSFunction* fun,
                             ICState::Mode mode) {
  MOZ_ASSERT(type == JSJitInfo::Getter || type == JSJitInfo::Setter ||
             type == JSJitInfo::Method);

  if (mode != ICState::Mode::Specialized) {
    return false;
  }

  if (!fun->hasJitInfo()) {
    return false;
  }

  if (cx->realm() != fun->realm()) {
    return false;
  }

  const JSJitInfo* jitInfo = fun->jitInfo();
  MOZ_ASSERT_IF(IsWindow(obj), !jitInfo->needsOuterizedThisObject());
  if (jitInfo->type() != type) {
    return false;
  }

  const JSClass* clasp = obj->getClass();
  if (!clasp->isDOMClass()) {
    return false;
  }

  if (type != JSJitInfo::Method && clasp->isProxyObject()) {
    return false;
  }

  // Tell the analysis the |DOMInstanceClassHasProtoAtDepth| hook can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  DOMInstanceClassHasProtoAtDepth instanceChecker =
      cx->runtime()->DOMcallbacks->instanceClassMatchesProto;
  return instanceChecker(clasp, jitInfo->protoID, jitInfo->depth);
}

static bool CanAttachDOMGetterSetter(JSContext* cx, JSJitInfo::OpType type,
                                     NativeObject* obj, NativeObject* holder,
                                     PropertyInfo prop, ICState::Mode mode) {
  MOZ_ASSERT(type == JSJitInfo::Getter || type == JSJitInfo::Setter);

  JSObject* accessor = type == JSJitInfo::Getter ? holder->getGetter(prop)
                                                 : holder->getSetter(prop);
  JSFunction* fun = &accessor->as<JSFunction>();

  return CanAttachDOMCall(cx, type, obj, fun, mode);
}

static void EmitCallDOMGetterResultNoGuards(CacheIRWriter& writer,
                                            NativeObject* holder,
                                            PropertyInfo prop,
                                            ObjOperandId objId) {
  JSFunction* getter = &holder->getGetter(prop)->as<JSFunction>();
  writer.callDOMGetterResult(objId, getter->jitInfo());
  writer.returnFromIC();
}

static void EmitCallDOMGetterResult(JSContext* cx, CacheIRWriter& writer,
                                    NativeObject* obj, NativeObject* holder,
                                    HandleId id, PropertyInfo prop,
                                    ObjOperandId objId) {
  // Note: this relies on EmitCallGetterResultGuards emitting a shape guard
  // for specialized stubs.
  // The shape guard ensures the receiver's Class is valid for this DOM getter.
  EmitCallGetterResultGuards(writer, obj, holder, id, prop, objId,
                             ICState::Mode::Specialized);
  EmitCallDOMGetterResultNoGuards(writer, holder, prop, objId);
}

void GetPropIRGenerator::attachMegamorphicNativeSlot(ObjOperandId objId,
                                                     jsid id) {
  MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);

  if (cacheKind_ == CacheKind::GetProp ||
      cacheKind_ == CacheKind::GetPropSuper) {
    writer.megamorphicLoadSlotResult(objId, id.toAtom()->asPropertyName());
  } else {
    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem ||
               cacheKind_ == CacheKind::GetElemSuper);
    writer.megamorphicLoadSlotByValueResult(objId, getElemKeyValueId());
  }
  writer.returnFromIC();

  trackAttached("MegamorphicNativeSlot");
}

AttachDecision GetPropIRGenerator::tryAttachNative(HandleObject obj,
                                                   ObjOperandId objId,
                                                   HandleId id,
                                                   ValOperandId receiverId) {
  Maybe<PropertyInfo> prop;
  NativeObject* holder = nullptr;

  NativeGetPropCacheability type =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  switch (type) {
    case CanAttachNone:
      return AttachDecision::NoAction;
    case CanAttachReadSlot: {
      auto* nobj = &obj->as<NativeObject>();

      if (mode_ == ICState::Mode::Megamorphic) {
        attachMegamorphicNativeSlot(objId, id);
        return AttachDecision::Attach;
      }

      maybeEmitIdGuard(id);
      EmitReadSlotResult(writer, nobj, holder, prop, objId);
      writer.returnFromIC();

      trackAttached("NativeSlot");
      return AttachDecision::Attach;
    }
    case CanAttachScriptedGetter:
    case CanAttachNativeGetter: {
      auto* nobj = &obj->as<NativeObject>();

      maybeEmitIdGuard(id);

      if (!isSuper() && CanAttachDOMGetterSetter(cx_, JSJitInfo::Getter, nobj,
                                                 holder, *prop, mode_)) {
        EmitCallDOMGetterResult(cx_, writer, nobj, holder, id, *prop, objId);

        trackAttached("DOMGetter");
        return AttachDecision::Attach;
      }

      EmitCallGetterResult(cx_, writer, nobj, holder, id, *prop, objId,
                           receiverId, mode_);

      trackAttached("NativeGetter");
      return AttachDecision::Attach;
    }
  }

  MOZ_CRASH("Bad NativeGetPropCacheability");
}

// Returns whether obj is a WindowProxy wrapping the script's global.
static bool IsWindowProxyForScriptGlobal(JSScript* script, JSObject* obj) {
  if (!IsWindowProxy(obj)) {
    return false;
  }

  MOZ_ASSERT(obj->getClass() ==
             script->runtimeFromMainThread()->maybeWindowProxyClass());

  JSObject* window = ToWindowIfWindowProxy(obj);

  // Ion relies on the WindowProxy's group changing (and the group getting
  // marked as having unknown properties) on navigation. If we ever stop
  // transplanting same-compartment WindowProxies, this assert will fail and we
  // need to fix that code.
  MOZ_ASSERT(window == &obj->nonCCWGlobal());

  // This must be a WindowProxy for a global in this compartment. Else it would
  // be a cross-compartment wrapper and IsWindowProxy returns false for
  // those.
  MOZ_ASSERT(script->compartment() == obj->compartment());

  // Only optimize lookups on the WindowProxy for the current global. Other
  // WindowProxies in the compartment may require security checks (based on
  // mutable document.domain). See bug 1516775.
  return window == &script->global();
}

// Guards objId is a WindowProxy for windowObj. Returns the window's operand id.
static ObjOperandId GuardAndLoadWindowProxyWindow(CacheIRWriter& writer,
                                                  ObjOperandId objId,
                                                  GlobalObject* windowObj) {
  // Note: update AddCacheIRGetPropFunction in BaselineInspector.cpp when making
  // changes here.
  writer.guardClass(objId, GuardClassKind::WindowProxy);
  ObjOperandId windowObjId = writer.loadWrapperTarget(objId);
  writer.guardSpecificObject(windowObjId, windowObj);
  return windowObjId;
}

AttachDecision GetPropIRGenerator::tryAttachWindowProxy(HandleObject obj,
                                                        ObjOperandId objId,
                                                        HandleId id) {
  // Attach a stub when the receiver is a WindowProxy and we can do the lookup
  // on the Window (the global object).

  if (!IsWindowProxyForScriptGlobal(script_, obj)) {
    return AttachDecision::NoAction;
  }

  // If we're megamorphic prefer a generic proxy stub that handles a lot more
  // cases.
  if (mode_ == ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  // Now try to do the lookup on the Window (the current global).
  GlobalObject* windowObj = cx_->global();
  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropCacheability type =
      CanAttachNativeGetProp(cx_, windowObj, id, &holder, &prop, pc_);
  switch (type) {
    case CanAttachNone:
      return AttachDecision::NoAction;

    case CanAttachReadSlot: {
      maybeEmitIdGuard(id);
      ObjOperandId windowObjId =
          GuardAndLoadWindowProxyWindow(writer, objId, windowObj);
      EmitReadSlotResult(writer, windowObj, holder, prop, windowObjId);
      writer.returnFromIC();

      trackAttached("WindowProxySlot");
      return AttachDecision::Attach;
    }

    case CanAttachNativeGetter: {
      // Make sure the native getter is okay with the IC passing the Window
      // instead of the WindowProxy as |this| value.
      JSFunction* callee = &holder->getGetter(*prop)->as<JSFunction>();
      MOZ_ASSERT(callee->isNativeWithoutJitEntry());
      if (!callee->hasJitInfo() ||
          callee->jitInfo()->needsOuterizedThisObject()) {
        return AttachDecision::NoAction;
      }

      // If a |super| access, it is not worth the complexity to attach an IC.
      if (isSuper()) {
        return AttachDecision::NoAction;
      }

      // Guard the incoming object is a WindowProxy and inline a getter call
      // based on the Window object.
      maybeEmitIdGuard(id);
      ObjOperandId windowObjId =
          GuardAndLoadWindowProxyWindow(writer, objId, windowObj);

      if (CanAttachDOMGetterSetter(cx_, JSJitInfo::Getter, windowObj, holder,
                                   *prop, mode_)) {
        EmitCallDOMGetterResult(cx_, writer, windowObj, holder, id, *prop,
                                windowObjId);
        trackAttached("WindowProxyDOMGetter");
      } else {
        ValOperandId receiverId = writer.boxObject(windowObjId);
        EmitCallGetterResult(cx_, writer, windowObj, holder, id, *prop,
                             windowObjId, receiverId, mode_);
        trackAttached("WindowProxyGetter");
      }

      return AttachDecision::Attach;
    }

    case CanAttachScriptedGetter:
      MOZ_ASSERT_UNREACHABLE("Not possible for window proxies");
  }

  MOZ_CRASH("Unreachable");
}

AttachDecision GetPropIRGenerator::tryAttachCrossCompartmentWrapper(
    HandleObject obj, ObjOperandId objId, HandleId id) {
  // We can only optimize this very wrapper-handler, because others might
  // have a security policy.
  if (!IsWrapper(obj) ||
      Wrapper::wrapperHandler(obj) != &CrossCompartmentWrapper::singleton) {
    return AttachDecision::NoAction;
  }

  // If we're megamorphic prefer a generic proxy stub that handles a lot more
  // cases.
  if (mode_ == ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  RootedObject unwrapped(cx_, Wrapper::wrappedObject(obj));
  MOZ_ASSERT(unwrapped == UnwrapOneCheckedStatic(obj));
  MOZ_ASSERT(!IsCrossCompartmentWrapper(unwrapped),
             "CCWs must not wrap other CCWs");

  // If we allowed different zones we would have to wrap strings.
  if (unwrapped->compartment()->zone() != cx_->compartment()->zone()) {
    return AttachDecision::NoAction;
  }

  // Take the unwrapped object's global, and wrap in a
  // this-compartment wrapper. This is what will be stored in the IC
  // keep the compartment alive.
  RootedObject wrappedTargetGlobal(cx_, &unwrapped->nonCCWGlobal());
  if (!cx_->compartment()->wrap(cx_, &wrappedTargetGlobal)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;

  // Enter realm of target to prevent failing compartment assertions when doing
  // the lookup.
  {
    AutoRealm ar(cx_, unwrapped);

    NativeGetPropCacheability canCache =
        CanAttachNativeGetProp(cx_, unwrapped, id, &holder, &prop, pc_);
    if (canCache != CanAttachReadSlot) {
      return AttachDecision::NoAction;
    }
  }
  auto* unwrappedNative = &unwrapped->as<NativeObject>();

  maybeEmitIdGuard(id);
  writer.guardIsProxy(objId);
  writer.guardHasProxyHandler(objId, Wrapper::wrapperHandler(obj));

  // Load the object wrapped by the CCW
  ObjOperandId wrapperTargetId = writer.loadWrapperTarget(objId);

  // If the compartment of the wrapped object is different we should fail.
  writer.guardCompartment(wrapperTargetId, wrappedTargetGlobal,
                          unwrappedNative->compartment());

  ObjOperandId unwrappedId = wrapperTargetId;
  EmitReadSlotResult<SlotReadType::CrossCompartment>(writer, unwrappedNative,
                                                     holder, prop, unwrappedId);
  writer.wrapResult();
  writer.returnFromIC();

  trackAttached("CCWSlot");
  return AttachDecision::Attach;
}

static bool GetXrayExpandoShapeWrapper(JSContext* cx, HandleObject xray,
                                       MutableHandleObject wrapper) {
  Value v = GetProxyReservedSlot(xray, GetXrayJitInfo()->xrayHolderSlot);
  if (v.isObject()) {
    NativeObject* holder = &v.toObject().as<NativeObject>();
    v = holder->getFixedSlot(GetXrayJitInfo()->holderExpandoSlot);
    if (v.isObject()) {
      RootedNativeObject expando(
          cx, &UncheckedUnwrap(&v.toObject())->as<NativeObject>());
      wrapper.set(NewWrapperWithObjectShape(cx, expando));
      return wrapper != nullptr;
    }
  }
  wrapper.set(nullptr);
  return true;
}

AttachDecision GetPropIRGenerator::tryAttachXrayCrossCompartmentWrapper(
    HandleObject obj, ObjOperandId objId, HandleId id,
    ValOperandId receiverId) {
  if (!obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  JS::XrayJitInfo* info = GetXrayJitInfo();
  if (!info || !info->isCrossCompartmentXray(GetProxyHandler(obj))) {
    return AttachDecision::NoAction;
  }

  if (!info->compartmentHasExclusiveExpandos(obj)) {
    return AttachDecision::NoAction;
  }

  RootedObject target(cx_, UncheckedUnwrap(obj));

  RootedObject expandoShapeWrapper(cx_);
  if (!GetXrayExpandoShapeWrapper(cx_, obj, &expandoShapeWrapper)) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Look for a getter we can call on the xray or its prototype chain.
  Rooted<Maybe<PropertyDescriptor>> desc(cx_);
  RootedObject holder(cx_, obj);
  RootedObjectVector prototypes(cx_);
  RootedObjectVector prototypeExpandoShapeWrappers(cx_);
  while (true) {
    if (!GetOwnPropertyDescriptor(cx_, holder, id, &desc)) {
      cx_->clearPendingException();
      return AttachDecision::NoAction;
    }
    if (desc.isSome()) {
      break;
    }
    if (!GetPrototype(cx_, holder, &holder)) {
      cx_->clearPendingException();
      return AttachDecision::NoAction;
    }
    if (!holder || !holder->is<ProxyObject>() ||
        !info->isCrossCompartmentXray(GetProxyHandler(holder))) {
      return AttachDecision::NoAction;
    }
    RootedObject prototypeExpandoShapeWrapper(cx_);
    if (!GetXrayExpandoShapeWrapper(cx_, holder,
                                    &prototypeExpandoShapeWrapper) ||
        !prototypes.append(holder) ||
        !prototypeExpandoShapeWrappers.append(prototypeExpandoShapeWrapper)) {
      cx_->recoverFromOutOfMemory();
      return AttachDecision::NoAction;
    }
  }
  if (!desc->isAccessorDescriptor()) {
    return AttachDecision::NoAction;
  }

  RootedObject getter(cx_, desc->getter());
  if (!getter || !getter->is<JSFunction>() ||
      !getter->as<JSFunction>().isNativeWithoutJitEntry()) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);
  writer.guardIsProxy(objId);
  writer.guardHasProxyHandler(objId, GetProxyHandler(obj));

  // Load the object wrapped by the CCW
  ObjOperandId wrapperTargetId = writer.loadWrapperTarget(objId);

  // Test the wrapped object's class. The properties held by xrays or their
  // prototypes will be invariant for objects of a given class, except for
  // changes due to xray expandos or xray prototype mutations.
  writer.guardAnyClass(wrapperTargetId, target->getClass());

  // Make sure the expandos on the xray and its prototype chain match up with
  // what we expect. The expando shape needs to be consistent, to ensure it
  // has not had any shadowing properties added, and the expando cannot have
  // any custom prototype (xray prototypes are stable otherwise).
  //
  // We can only do this for xrays with exclusive access to their expandos
  // (as we checked earlier), which store a pointer to their expando
  // directly. Xrays in other compartments may share their expandos with each
  // other and a VM call is needed just to find the expando.
  if (expandoShapeWrapper) {
    writer.guardXrayExpandoShapeAndDefaultProto(objId, expandoShapeWrapper);
  } else {
    writer.guardXrayNoExpando(objId);
  }
  for (size_t i = 0; i < prototypes.length(); i++) {
    JSObject* proto = prototypes[i];
    ObjOperandId protoId = writer.loadObject(proto);
    if (JSObject* protoShapeWrapper = prototypeExpandoShapeWrappers[i]) {
      writer.guardXrayExpandoShapeAndDefaultProto(protoId, protoShapeWrapper);
    } else {
      writer.guardXrayNoExpando(protoId);
    }
  }

  bool sameRealm = cx_->realm() == getter->as<JSFunction>().realm();
  writer.callNativeGetterResult(receiverId, &getter->as<JSFunction>(),
                                sameRealm);
  writer.returnFromIC();

  trackAttached("XrayGetter");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachGenericProxy(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    bool handleDOMProxies) {
  writer.guardIsProxy(objId);

  if (!handleDOMProxies) {
    // Ensure that the incoming object is not a DOM proxy, so that we can get to
    // the specialized stubs
    writer.guardIsNotDOMProxy(objId);
  }

  if (cacheKind_ == CacheKind::GetProp || mode_ == ICState::Mode::Specialized) {
    MOZ_ASSERT(!isSuper());
    maybeEmitIdGuard(id);
    writer.proxyGetResult(objId, id);
  } else {
    // Attach a stub that handles every id.
    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem);
    MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);
    MOZ_ASSERT(!isSuper());
    writer.proxyGetByValueResult(objId, getElemKeyValueId());
  }

  writer.returnFromIC();

  trackAttached("GenericProxy");
  return AttachDecision::Attach;
}

static bool ValueIsInt64Index(const Value& val, int64_t* index) {
  // Try to convert the Value to a TypedArray index or DataView offset.

  if (val.isInt32()) {
    *index = val.toInt32();
    return true;
  }

  if (val.isDouble()) {
    // Use NumberEqualsInt64 because ToPropertyKey(-0) is 0.
    return mozilla::NumberEqualsInt64(val.toDouble(), index);
  }

  return false;
}

IntPtrOperandId IRGenerator::guardToIntPtrIndex(const Value& index,
                                                ValOperandId indexId,
                                                bool supportOOB) {
#ifdef DEBUG
  int64_t indexInt64;
  MOZ_ASSERT_IF(!supportOOB, ValueIsInt64Index(index, &indexInt64));
#endif

  if (index.isInt32()) {
    Int32OperandId int32IndexId = writer.guardToInt32(indexId);
    return writer.int32ToIntPtr(int32IndexId);
  }

  MOZ_ASSERT(index.isNumber());
  NumberOperandId numberIndexId = writer.guardIsNumber(indexId);
  return writer.guardNumberToIntPtrIndex(numberIndexId, supportOOB);
}

ObjOperandId IRGenerator::guardDOMProxyExpandoObjectAndShape(
    ProxyObject* obj, ObjOperandId objId, const Value& expandoVal,
    NativeObject* expandoObj) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  TestMatchingProxyReceiver(writer, obj, objId);

  // Shape determines Class, so now it must be a DOM proxy.
  ValOperandId expandoValId;
  if (expandoVal.isObject()) {
    expandoValId = writer.loadDOMExpandoValue(objId);
  } else {
    expandoValId = writer.loadDOMExpandoValueIgnoreGeneration(objId);
  }

  // Guard the expando is an object and shape guard.
  ObjOperandId expandoObjId = writer.guardToObject(expandoValId);
  TestMatchingHolder(writer, expandoObj, expandoObjId);
  return expandoObjId;
}

AttachDecision GetPropIRGenerator::tryAttachDOMProxyExpando(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId receiverId) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  Value expandoVal = GetProxyPrivate(obj);
  JSObject* expandoObj;
  if (expandoVal.isObject()) {
    expandoObj = &expandoVal.toObject();
  } else {
    MOZ_ASSERT(!expandoVal.isUndefined(),
               "How did a missing expando manage to shadow things?");
    auto expandoAndGeneration =
        static_cast<ExpandoAndGeneration*>(expandoVal.toPrivate());
    MOZ_ASSERT(expandoAndGeneration);
    expandoObj = &expandoAndGeneration->expando.toObject();
  }

  // Try to do the lookup on the expando object.
  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropCacheability canCache =
      CanAttachNativeGetProp(cx_, expandoObj, id, &holder, &prop, pc_);
  if (canCache == CanAttachNone) {
    return AttachDecision::NoAction;
  }
  if (!holder) {
    return AttachDecision::NoAction;
  }
  auto* nativeExpandoObj = &expandoObj->as<NativeObject>();

  MOZ_ASSERT(holder == nativeExpandoObj);

  maybeEmitIdGuard(id);
  ObjOperandId expandoObjId = guardDOMProxyExpandoObjectAndShape(
      obj, objId, expandoVal, nativeExpandoObj);

  if (canCache == CanAttachReadSlot) {
    // Load from the expando's slots.
    EmitLoadSlotResult(writer, expandoObjId, nativeExpandoObj, *prop);
    writer.returnFromIC();
  } else {
    // Call the getter. Note that we pass objId, the DOM proxy, as |this|
    // and not the expando object.
    MOZ_ASSERT(canCache == CanAttachNativeGetter ||
               canCache == CanAttachScriptedGetter);
    EmitGuardGetterSetterSlot(writer, nativeExpandoObj, *prop, expandoObjId);
    EmitCallGetterResultNoGuards(cx_, writer, nativeExpandoObj,
                                 nativeExpandoObj, *prop, receiverId);
  }

  trackAttached("DOMProxyExpando");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachDOMProxyShadowed(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id) {
  MOZ_ASSERT(!isSuper());
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  maybeEmitIdGuard(id);
  TestMatchingProxyReceiver(writer, obj, objId);
  writer.proxyGetResult(objId, id);
  writer.returnFromIC();

  trackAttached("DOMProxyShadowed");
  return AttachDecision::Attach;
}

// Callers are expected to have already guarded on the shape of the
// object, which guarantees the object is a DOM proxy.
static void CheckDOMProxyExpandoDoesNotShadow(CacheIRWriter& writer,
                                              ProxyObject* obj, jsid id,
                                              ObjOperandId objId) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  Value expandoVal = GetProxyPrivate(obj);

  ValOperandId expandoId;
  if (!expandoVal.isObject() && !expandoVal.isUndefined()) {
    auto expandoAndGeneration =
        static_cast<ExpandoAndGeneration*>(expandoVal.toPrivate());
    uint64_t generation = expandoAndGeneration->generation;
    expandoId = writer.loadDOMExpandoValueGuardGeneration(
        objId, expandoAndGeneration, generation);
    expandoVal = expandoAndGeneration->expando;
  } else {
    expandoId = writer.loadDOMExpandoValue(objId);
  }

  if (expandoVal.isUndefined()) {
    // Guard there's no expando object.
    writer.guardNonDoubleType(expandoId, ValueType::Undefined);
  } else if (expandoVal.isObject()) {
    // Guard the proxy either has no expando object or, if it has one, that
    // the shape matches the current expando object.
    NativeObject& expandoObj = expandoVal.toObject().as<NativeObject>();
    MOZ_ASSERT(!expandoObj.containsPure(id));
    writer.guardDOMExpandoMissingOrGuardShape(expandoId, expandoObj.shape());
  } else {
    MOZ_CRASH("Invalid expando value");
  }
}

AttachDecision GetPropIRGenerator::tryAttachDOMProxyUnshadowed(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId receiverId) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  JSObject* checkObj = obj->staticPrototype();
  if (!checkObj) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropCacheability canCache =
      CanAttachNativeGetProp(cx_, checkObj, id, &holder, &prop, pc_);
  if (canCache == CanAttachNone) {
    return AttachDecision::NoAction;
  }
  auto* nativeCheckObj = &checkObj->as<NativeObject>();

  maybeEmitIdGuard(id);

  // Guard that our expando object hasn't started shadowing this property.
  TestMatchingProxyReceiver(writer, obj, objId);
  CheckDOMProxyExpandoDoesNotShadow(writer, obj, id, objId);

  if (holder) {
    // Found the property on the prototype chain. Treat it like a native
    // getprop.
    GeneratePrototypeGuards(writer, obj, holder, objId);

    // Guard on the holder of the property.
    ObjOperandId holderId = writer.loadObject(holder);
    TestMatchingHolder(writer, holder, holderId);

    if (canCache == CanAttachReadSlot) {
      EmitLoadSlotResult(writer, holderId, holder, *prop);
      writer.returnFromIC();
    } else {
      // EmitCallGetterResultNoGuards expects |obj| to be the object the
      // property is on to do some checks. Since we actually looked at
      // checkObj, and no extra guards will be generated, we can just
      // pass that instead.
      MOZ_ASSERT(canCache == CanAttachNativeGetter ||
                 canCache == CanAttachScriptedGetter);
      MOZ_ASSERT(!isSuper());
      EmitGuardGetterSetterSlot(writer, holder, *prop, holderId,
                                /* holderIsConstant = */ true);
      EmitCallGetterResultNoGuards(cx_, writer, nativeCheckObj, holder, *prop,
                                   receiverId);
    }
  } else {
    // Property was not found on the prototype chain. Deoptimize down to
    // proxy get call.
    MOZ_ASSERT(!isSuper());
    writer.proxyGetResult(objId, id);
    writer.returnFromIC();
  }

  trackAttached("DOMProxyUnshadowed");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachProxy(HandleObject obj,
                                                  ObjOperandId objId,
                                                  HandleId id,
                                                  ValOperandId receiverId) {
  ProxyStubType type = GetProxyStubType(cx_, obj, id);
  if (type == ProxyStubType::None) {
    return AttachDecision::NoAction;
  }
  auto proxy = obj.as<ProxyObject>();

  // The proxy stubs don't currently support |super| access.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  if (mode_ == ICState::Mode::Megamorphic) {
    return tryAttachGenericProxy(proxy, objId, id,
                                 /* handleDOMProxies = */ true);
  }

  switch (type) {
    case ProxyStubType::None:
      break;
    case ProxyStubType::DOMExpando:
      TRY_ATTACH(tryAttachDOMProxyExpando(proxy, objId, id, receiverId));
      [[fallthrough]];  // Fall through to the generic shadowed case.
    case ProxyStubType::DOMShadowed:
      return tryAttachDOMProxyShadowed(proxy, objId, id);
    case ProxyStubType::DOMUnshadowed:
      TRY_ATTACH(tryAttachDOMProxyUnshadowed(proxy, objId, id, receiverId));
      return tryAttachGenericProxy(proxy, objId, id,
                                   /* handleDOMProxies = */ true);
    case ProxyStubType::Generic:
      return tryAttachGenericProxy(proxy, objId, id,
                                   /* handleDOMProxies = */ false);
  }

  MOZ_CRASH("Unexpected ProxyStubType");
}

AttachDecision GetPropIRGenerator::tryAttachObjectLength(HandleObject obj,
                                                         ObjOperandId objId,
                                                         HandleId id) {
  if (!id.isAtom(cx_->names().length)) {
    return AttachDecision::NoAction;
  }

  if (obj->is<ArrayObject>()) {
    if (obj->as<ArrayObject>().length() > INT32_MAX) {
      return AttachDecision::NoAction;
    }

    maybeEmitIdGuard(id);
    writer.guardClass(objId, GuardClassKind::Array);
    writer.loadInt32ArrayLengthResult(objId);
    writer.returnFromIC();

    trackAttached("ArrayLength");
    return AttachDecision::Attach;
  }

  if (obj->is<ArgumentsObject>() &&
      !obj->as<ArgumentsObject>().hasOverriddenLength()) {
    maybeEmitIdGuard(id);
    if (obj->is<MappedArgumentsObject>()) {
      writer.guardClass(objId, GuardClassKind::MappedArguments);
    } else {
      MOZ_ASSERT(obj->is<UnmappedArgumentsObject>());
      writer.guardClass(objId, GuardClassKind::UnmappedArguments);
    }
    writer.loadArgumentsObjectLengthResult(objId);
    writer.returnFromIC();

    trackAttached("ArgumentsObjectLength");
    return AttachDecision::Attach;
  }

  return AttachDecision::NoAction;
}

AttachDecision GetPropIRGenerator::tryAttachTypedArray(HandleObject obj,
                                                       ObjOperandId objId,
                                                       HandleId id) {
  if (!obj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  bool isLength = id.isAtom(cx_->names().length);
  bool isByteOffset = id.isAtom(cx_->names().byteOffset);
  if (!isLength && !isByteOffset && !id.isAtom(cx_->names().byteLength)) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropCacheability type =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (type != CanAttachNativeGetter) {
    return AttachDecision::NoAction;
  }

  JSFunction& fun = holder->getGetter(*prop)->as<JSFunction>();
  if (isLength) {
    if (!TypedArrayObject::isOriginalLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  } else if (isByteOffset) {
    if (!TypedArrayObject::isOriginalByteOffsetGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  } else {
    if (!TypedArrayObject::isOriginalByteLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  }

  auto* tarr = &obj->as<TypedArrayObject>();

  maybeEmitIdGuard(id);
  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  EmitCallGetterResultGuards(writer, tarr, holder, id, *prop, objId, mode_);
  if (isLength) {
    if (tarr->length() <= INT32_MAX) {
      writer.loadArrayBufferViewLengthInt32Result(objId);
    } else {
      writer.loadArrayBufferViewLengthDoubleResult(objId);
    }
    trackAttached("TypedArrayLength");
  } else if (isByteOffset) {
    if (tarr->byteOffset() <= INT32_MAX) {
      writer.arrayBufferViewByteOffsetInt32Result(objId);
    } else {
      writer.arrayBufferViewByteOffsetDoubleResult(objId);
    }
    trackAttached("TypedArrayByteOffset");
  } else {
    if (tarr->byteLength() <= INT32_MAX) {
      writer.typedArrayByteLengthInt32Result(objId);
    } else {
      writer.typedArrayByteLengthDoubleResult(objId);
    }
    trackAttached("TypedArrayByteLength");
  }
  writer.returnFromIC();

  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachDataView(HandleObject obj,
                                                     ObjOperandId objId,
                                                     HandleId id) {
  if (!obj->is<DataViewObject>()) {
    return AttachDecision::NoAction;
  }
  auto* dv = &obj->as<DataViewObject>();

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  bool isByteOffset = id.isAtom(cx_->names().byteOffset);
  if (!isByteOffset && !id.isAtom(cx_->names().byteLength)) {
    return AttachDecision::NoAction;
  }

  // byteOffset and byteLength both throw when the ArrayBuffer is detached.
  if (dv->hasDetachedBuffer()) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropCacheability type =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (type != CanAttachNativeGetter) {
    return AttachDecision::NoAction;
  }

  auto& fun = holder->getGetter(*prop)->as<JSFunction>();
  if (isByteOffset) {
    if (!DataViewObject::isOriginalByteOffsetGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  } else {
    if (!DataViewObject::isOriginalByteLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  }

  maybeEmitIdGuard(id);
  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  EmitCallGetterResultGuards(writer, dv, holder, id, *prop, objId, mode_);
  writer.guardHasAttachedArrayBuffer(objId);
  if (isByteOffset) {
    if (dv->byteOffset() <= INT32_MAX) {
      writer.arrayBufferViewByteOffsetInt32Result(objId);
    } else {
      writer.arrayBufferViewByteOffsetDoubleResult(objId);
    }
    trackAttached("DataViewByteOffset");
  } else {
    if (dv->byteLength() <= INT32_MAX) {
      writer.loadArrayBufferViewLengthInt32Result(objId);
    } else {
      writer.loadArrayBufferViewLengthDoubleResult(objId);
    }
    trackAttached("DataViewByteLength");
  }
  writer.returnFromIC();

  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachArrayBufferMaybeShared(
    HandleObject obj, ObjOperandId objId, HandleId id) {
  if (!obj->is<ArrayBufferObjectMaybeShared>()) {
    return AttachDecision::NoAction;
  }
  auto* buf = &obj->as<ArrayBufferObjectMaybeShared>();

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  if (!id.isAtom(cx_->names().byteLength)) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropCacheability type =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (type != CanAttachNativeGetter) {
    return AttachDecision::NoAction;
  }

  auto& fun = holder->getGetter(*prop)->as<JSFunction>();
  if (buf->is<ArrayBufferObject>()) {
    if (!ArrayBufferObject::isOriginalByteLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  } else {
    if (!SharedArrayBufferObject::isOriginalByteLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  }

  maybeEmitIdGuard(id);
  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  EmitCallGetterResultGuards(writer, buf, holder, id, *prop, objId, mode_);
  if (buf->byteLength() <= INT32_MAX) {
    writer.loadArrayBufferByteLengthInt32Result(objId);
  } else {
    writer.loadArrayBufferByteLengthDoubleResult(objId);
  }
  writer.returnFromIC();

  trackAttached("ArrayBufferMaybeSharedByteLength");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachRegExp(HandleObject obj,
                                                   ObjOperandId objId,
                                                   HandleId id) {
  if (!obj->is<RegExpObject>()) {
    return AttachDecision::NoAction;
  }
  auto* regExp = &obj->as<RegExpObject>();

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropCacheability type =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (type != CanAttachNativeGetter) {
    return AttachDecision::NoAction;
  }

  auto& fun = holder->getGetter(*prop)->as<JSFunction>();
  JS::RegExpFlags flags = JS::RegExpFlag::NoFlags;
  if (!RegExpObject::isOriginalFlagGetter(fun.native(), &flags)) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);
  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  EmitCallGetterResultGuards(writer, regExp, holder, id, *prop, objId, mode_);

  writer.regExpFlagResult(objId, flags.value());
  writer.returnFromIC();

  trackAttached("RegExpFlag");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachFunction(HandleObject obj,
                                                     ObjOperandId objId,
                                                     HandleId id) {
  // Function properties are lazily resolved so they might not be defined yet.
  // And we might end up in a situation where we always have a fresh function
  // object during the IC generation.
  if (!obj->is<JSFunction>()) {
    return AttachDecision::NoAction;
  }

  bool isLength = id.isAtom(cx_->names().length);
  if (!isLength && !id.isAtom(cx_->names().name)) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  PropertyResult prop;
  // If this property exists already, don't attach the stub.
  if (LookupPropertyPure(cx_, obj, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }

  JSFunction* fun = &obj->as<JSFunction>();

  if (isLength) {
    // length was probably deleted from the function.
    if (fun->hasResolvedLength()) {
      return AttachDecision::NoAction;
    }

    // Lazy functions don't store the length.
    if (!fun->hasBytecode()) {
      return AttachDecision::NoAction;
    }

    // Length can be non-int32 for bound functions.
    if (fun->isBoundFunction()) {
      constexpr auto lengthSlot = FunctionExtended::BOUND_FUNCTION_LENGTH_SLOT;
      if (!fun->getExtendedSlot(lengthSlot).isInt32()) {
        return AttachDecision::NoAction;
      }
    }
  } else {
    // name was probably deleted from the function.
    if (fun->hasResolvedName()) {
      return AttachDecision::NoAction;
    }

    // Unless the bound function name prefix is present, we need to call into
    // the VM to compute the full name.
    if (fun->isBoundFunction() && !fun->hasBoundFunctionNamePrefix()) {
      return AttachDecision::NoAction;
    }
  }

  maybeEmitIdGuard(id);
  writer.guardClass(objId, GuardClassKind::JSFunction);
  if (isLength) {
    writer.loadFunctionLengthResult(objId);
    writer.returnFromIC();
    trackAttached("FunctionLength");
  } else {
    writer.loadFunctionNameResult(objId);
    writer.returnFromIC();
    trackAttached("FunctionName");
  }
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachArgumentsObjectIterator(
    HandleObject obj, ObjOperandId objId, HandleId id) {
  if (!obj->is<ArgumentsObject>()) {
    return AttachDecision::NoAction;
  }

  if (!id.isWellKnownSymbol(JS::SymbolCode::iterator)) {
    return AttachDecision::NoAction;
  }

  Handle<ArgumentsObject*> args = obj.as<ArgumentsObject>();
  if (args->hasOverriddenIterator()) {
    return AttachDecision::NoAction;
  }

  RootedValue iterator(cx_);
  if (!ArgumentsObject::getArgumentsIterator(cx_, &iterator)) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(iterator.isObject());

  maybeEmitIdGuard(id);
  if (args->is<MappedArgumentsObject>()) {
    writer.guardClass(objId, GuardClassKind::MappedArguments);
  } else {
    MOZ_ASSERT(args->is<UnmappedArgumentsObject>());
    writer.guardClass(objId, GuardClassKind::UnmappedArguments);
  }
  uint32_t flags = ArgumentsObject::ITERATOR_OVERRIDDEN_BIT;
  writer.guardArgumentsObjectFlags(objId, flags);

  ObjOperandId iterId = writer.loadObject(&iterator.toObject());
  writer.loadObjectResult(iterId);
  writer.returnFromIC();

  trackAttached("ArgumentsObjectIterator");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachModuleNamespace(HandleObject obj,
                                                            ObjOperandId objId,
                                                            HandleId id) {
  if (!obj->is<ModuleNamespaceObject>()) {
    return AttachDecision::NoAction;
  }

  auto* ns = &obj->as<ModuleNamespaceObject>();
  ModuleEnvironmentObject* env = nullptr;
  Maybe<PropertyInfo> prop;
  if (!ns->bindings().lookup(id, &env, &prop)) {
    return AttachDecision::NoAction;
  }

  // Don't emit a stub until the target binding has been initialized.
  if (env->getSlot(prop->slot()).isMagic(JS_UNINITIALIZED_LEXICAL)) {
    return AttachDecision::NoAction;
  }

  // Check for the specific namespace object.
  maybeEmitIdGuard(id);
  writer.guardSpecificObject(objId, ns);

  ObjOperandId envId = writer.loadObject(env);
  EmitLoadSlotResult(writer, envId, env, *prop);
  writer.returnFromIC();

  trackAttached("ModuleNamespace");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachPrimitive(ValOperandId valId,
                                                      HandleId id) {
  MOZ_ASSERT(!isSuper(), "SuperBase is guaranteed to be an object");

  JSProtoKey protoKey;
  switch (val_.type()) {
    case ValueType::String:
      if (id.isAtom(cx_->names().length)) {
        // String length is special-cased, see js::GetProperty.
        return AttachDecision::NoAction;
      }
      protoKey = JSProto_String;
      break;
    case ValueType::Int32:
    case ValueType::Double:
      protoKey = JSProto_Number;
      break;
    case ValueType::Boolean:
      protoKey = JSProto_Boolean;
      break;
    case ValueType::Symbol:
      protoKey = JSProto_Symbol;
      break;
    case ValueType::BigInt:
      protoKey = JSProto_BigInt;
      break;
    case ValueType::Null:
    case ValueType::Undefined:
    case ValueType::Magic:
      return AttachDecision::NoAction;
    case ValueType::Object:
    case ValueType::PrivateGCThing:
      MOZ_CRASH("unexpected type");
  }

  JSObject* proto = cx_->global()->maybeGetPrototype(protoKey);
  if (!proto) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropCacheability type =
      CanAttachNativeGetProp(cx_, proto, id, &holder, &prop, pc_);
  switch (type) {
    case CanAttachNone:
      return AttachDecision::NoAction;
    case CanAttachReadSlot: {
      auto* nproto = &proto->as<NativeObject>();

      if (val_.isNumber()) {
        writer.guardIsNumber(valId);
      } else {
        writer.guardNonDoubleType(valId, val_.type());
      }
      maybeEmitIdGuard(id);

      ObjOperandId protoId = writer.loadObject(nproto);
      EmitReadSlotResult(writer, nproto, holder, prop, protoId);
      writer.returnFromIC();

      trackAttached("PrimitiveSlot");
      return AttachDecision::Attach;
    }
    case CanAttachScriptedGetter:
    case CanAttachNativeGetter: {
      auto* nproto = &proto->as<NativeObject>();

      if (val_.isNumber()) {
        writer.guardIsNumber(valId);
      } else {
        writer.guardNonDoubleType(valId, val_.type());
      }
      maybeEmitIdGuard(id);

      ObjOperandId protoId = writer.loadObject(nproto);
      EmitCallGetterResult(cx_, writer, nproto, holder, id, *prop, protoId,
                           valId, mode_);

      trackAttached("PrimitiveGetter");
      return AttachDecision::Attach;
    }
  }

  MOZ_CRASH("Bad NativeGetPropCacheability");
}

AttachDecision GetPropIRGenerator::tryAttachStringLength(ValOperandId valId,
                                                         HandleId id) {
  if (!val_.isString() || !id.isAtom(cx_->names().length)) {
    return AttachDecision::NoAction;
  }

  StringOperandId strId = writer.guardToString(valId);
  maybeEmitIdGuard(id);
  writer.loadStringLengthResult(strId);
  writer.returnFromIC();

  trackAttached("StringLength");
  return AttachDecision::Attach;
}

static bool CanAttachStringChar(const Value& val, const Value& idVal) {
  if (!val.isString() || !idVal.isInt32()) {
    return false;
  }

  int32_t index = idVal.toInt32();
  if (index < 0) {
    return false;
  }

  JSString* str = val.toString();
  if (size_t(index) >= str->length()) {
    return false;
  }

  // This follows JSString::getChar, otherwise we fail to attach getChar in a
  // lot of cases.
  if (str->isRope()) {
    JSRope* rope = &str->asRope();

    // Make sure the left side contains the index.
    if (size_t(index) >= rope->leftChild()->length()) {
      return false;
    }

    str = rope->leftChild();
  }

  if (!str->isLinear()) {
    return false;
  }

  return true;
}

AttachDecision GetPropIRGenerator::tryAttachStringChar(ValOperandId valId,
                                                       ValOperandId indexId) {
  MOZ_ASSERT(idVal_.isInt32());

  if (!CanAttachStringChar(val_, idVal_)) {
    return AttachDecision::NoAction;
  }

  StringOperandId strId = writer.guardToString(valId);
  Int32OperandId int32IndexId = writer.guardToInt32Index(indexId);
  writer.loadStringCharResult(strId, int32IndexId);
  writer.returnFromIC();

  trackAttached("StringChar");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachArgumentsObjectArg(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<ArgumentsObject>()) {
    return AttachDecision::NoAction;
  }
  auto* args = &obj->as<ArgumentsObject>();

  // No elements must have been overridden or deleted.
  if (args->hasOverriddenElement()) {
    return AttachDecision::NoAction;
  }

  // Check bounds.
  if (index >= args->initialLength()) {
    return AttachDecision::NoAction;
  }

  // And finally also check that the argument isn't forwarded.
  if (args->argIsForwarded(index)) {
    return AttachDecision::NoAction;
  }

  if (args->is<MappedArgumentsObject>()) {
    writer.guardClass(objId, GuardClassKind::MappedArguments);
  } else {
    MOZ_ASSERT(args->is<UnmappedArgumentsObject>());
    writer.guardClass(objId, GuardClassKind::UnmappedArguments);
  }

  writer.loadArgumentsObjectArgResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("ArgumentsObjectArg");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachArgumentsObjectCallee(
    HandleObject obj, ObjOperandId objId, HandleId id) {
  // Only mapped arguments objects have a `callee` property.
  if (!obj->is<MappedArgumentsObject>()) {
    return AttachDecision::NoAction;
  }

  if (!id.isAtom(cx_->names().callee)) {
    return AttachDecision::NoAction;
  }

  // The callee must not have been overridden or deleted.
  if (obj->as<MappedArgumentsObject>().hasOverriddenCallee()) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);
  writer.guardClass(objId, GuardClassKind::MappedArguments);

  uint32_t flags = ArgumentsObject::CALLEE_OVERRIDDEN_BIT;
  writer.guardArgumentsObjectFlags(objId, flags);

  writer.loadFixedSlotResult(objId,
                             MappedArgumentsObject::getCalleeSlotOffset());
  writer.returnFromIC();

  trackAttached("ArgumentsObjectCallee");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachDenseElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }

  TestMatchingNativeReceiver(writer, nobj, objId);
  writer.loadDenseElementResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("DenseElement");
  return AttachDecision::Attach;
}

static bool ClassCanHaveExtraProperties(const JSClass* clasp) {
  return clasp->getResolve() || clasp->getOpsLookupProperty() ||
         clasp->getOpsGetProperty() || IsTypedArrayClass(clasp);
}

static bool CanAttachDenseElementHole(NativeObject* obj, bool ownProp,
                                      bool allowIndexedReceiver = false) {
  // Make sure the objects on the prototype don't have any indexed properties
  // or that such properties can't appear without a shape change.
  // Otherwise returning undefined for holes would obviously be incorrect,
  // because we would have to lookup a property on the prototype instead.
  do {
    // The first two checks are also relevant to the receiver object.
    if (!allowIndexedReceiver && obj->isIndexed()) {
      return false;
    }
    allowIndexedReceiver = false;

    if (ClassCanHaveExtraProperties(obj->getClass())) {
      return false;
    }

    // Don't need to check prototype for OwnProperty checks
    if (ownProp) {
      return true;
    }

    JSObject* proto = obj->staticPrototype();
    if (!proto) {
      break;
    }

    if (!proto->is<NativeObject>()) {
      return false;
    }

    // Make sure objects on the prototype don't have dense elements.
    if (proto->as<NativeObject>().getDenseInitializedLength() != 0) {
      return false;
    }

    obj = &proto->as<NativeObject>();
  } while (true);

  return true;
}

AttachDecision GetPropIRGenerator::tryAttachDenseElementHole(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }
  if (!CanAttachDenseElementHole(nobj, false)) {
    return AttachDecision::NoAction;
  }

  // Guard on the shape, to prevent non-dense elements from appearing.
  TestMatchingNativeReceiver(writer, nobj, objId);
  GeneratePrototypeHoleGuards(writer, nobj, objId,
                              /* alwaysGuardFirstProto = */ false);
  writer.loadDenseElementHoleResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("DenseElementHole");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachSparseElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }
  NativeObject* nobj = &obj->as<NativeObject>();

  // Stub doesn't handle negative indices.
  if (index > INT_MAX) {
    return AttachDecision::NoAction;
  }

  // We also need to be past the end of the dense capacity, to ensure sparse.
  if (index < nobj->getDenseInitializedLength()) {
    return AttachDecision::NoAction;
  }

  // Only handle Array objects in this stub.
  if (!nobj->is<ArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Here, we ensure that the prototype chain does not define any sparse
  // indexed properties on the shape lineage. This allows us to guard on
  // the shapes up the prototype chain to ensure that no indexed properties
  // exist outside of the dense elements.
  //
  // The `GeneratePrototypeHoleGuards` call below will guard on the shapes,
  // as well as ensure that no prototypes contain dense elements, allowing
  // us to perform a pure shape-search for out-of-bounds integer-indexed
  // properties on the recevier object.
  if ((nobj->staticPrototype() != nullptr) &&
      ObjectMayHaveExtraIndexedProperties(nobj->staticPrototype())) {
    return AttachDecision::NoAction;
  }

  // Ensure that obj is an Array.
  writer.guardClass(objId, GuardClassKind::Array);

  // The helper we are going to call only applies to non-dense elements.
  writer.guardIndexGreaterThanDenseInitLength(objId, indexId);

  // Ensures we are able to efficiently able to map to an integral jsid.
  writer.guardInt32IsNonNegative(indexId);

  // Shape guard the prototype chain to avoid shadowing indexes from appearing.
  // The helper function also ensures that the index does not appear within the
  // dense element set of the prototypes.
  GeneratePrototypeHoleGuards(writer, nobj, objId,
                              /* alwaysGuardFirstProto = */ true);

  // At this point, we are guaranteed that the indexed property will not
  // be found on one of the prototypes. We are assured that we only have
  // to check that the receiving object has the property.

  writer.callGetSparseElementResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("GetSparseElement");
  return AttachDecision::Attach;
}

// For Uint32Array we let the stub return an Int32 if we have not seen a
// double, to allow better codegen in Warp while avoiding bailout loops.
static bool ForceDoubleForUint32Array(TypedArrayObject* tarr, uint64_t index) {
  MOZ_ASSERT(index < tarr->length());

  if (tarr->type() != Scalar::Type::Uint32) {
    // Return value is only relevant for Uint32Array.
    return false;
  }

  Value res;
  MOZ_ALWAYS_TRUE(tarr->getElementPure(index, &res));
  MOZ_ASSERT(res.isNumber());
  return res.isDouble();
}

AttachDecision GetPropIRGenerator::tryAttachTypedArrayElement(
    HandleObject obj, ObjOperandId objId) {
  if (!obj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  if (!idVal_.isNumber()) {
    return AttachDecision::NoAction;
  }

  TypedArrayObject* tarr = &obj->as<TypedArrayObject>();

  bool handleOOB = false;
  int64_t indexInt64;
  if (!ValueIsInt64Index(idVal_, &indexInt64) || indexInt64 < 0 ||
      uint64_t(indexInt64) >= tarr->length()) {
    handleOOB = true;
  }

  // If the number is not representable as an integer the result will be
  // |undefined| so we leave |forceDoubleForUint32| as false.
  bool forceDoubleForUint32 = false;
  if (!handleOOB) {
    uint64_t index = uint64_t(indexInt64);
    forceDoubleForUint32 = ForceDoubleForUint32Array(tarr, index);
  }

  writer.guardShapeForClass(objId, tarr->shape());

  ValOperandId keyId = getElemKeyValueId();
  IntPtrOperandId intPtrIndexId = guardToIntPtrIndex(idVal_, keyId, handleOOB);

  writer.loadTypedArrayElementResult(objId, intPtrIndexId, tarr->type(),
                                     handleOOB, forceDoubleForUint32);
  writer.returnFromIC();

  trackAttached("TypedElement");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachGenericElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  // To allow other types to attach in the non-megamorphic case we test the
  // specific matching native receiver; however, once megamorphic we can attach
  // for any native
  if (mode_ == ICState::Mode::Megamorphic) {
    writer.guardIsNativeObject(objId);
  } else {
    NativeObject* nobj = &obj->as<NativeObject>();
    TestMatchingNativeReceiver(writer, nobj, objId);
  }
  writer.guardIndexGreaterThanDenseInitLength(objId, indexId);
  writer.callNativeGetElementResult(objId, indexId);
  writer.returnFromIC();

  trackAttached(mode_ == ICState::Mode::Megamorphic
                    ? "GenericElementMegamorphic"
                    : "GenericElement");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachProxyElement(HandleObject obj,
                                                         ObjOperandId objId) {
  if (!obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  // The proxy stubs don't currently support |super| access.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  writer.guardIsProxy(objId);

  // We are not guarding against DOM proxies here, because there is no other
  // specialized DOM IC we could attach.
  // We could call maybeEmitIdGuard here and then emit ProxyGetResult,
  // but for GetElem we prefer to attach a stub that can handle any Value
  // so we don't attach a new stub for every id.
  MOZ_ASSERT(cacheKind_ == CacheKind::GetElem);
  MOZ_ASSERT(!isSuper());
  writer.proxyGetByValueResult(objId, getElemKeyValueId());
  writer.returnFromIC();

  trackAttached("ProxyElement");
  return AttachDecision::Attach;
}

void GetPropIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", val_);
    sp.valueProperty("property", idVal_);
  }
#endif
}

void IRGenerator::emitIdGuard(ValOperandId valId, const Value& idVal, jsid id) {
  if (id.isSymbol()) {
    MOZ_ASSERT(idVal.toSymbol() == id.toSymbol());
    SymbolOperandId symId = writer.guardToSymbol(valId);
    writer.guardSpecificSymbol(symId, id.toSymbol());
  } else {
    MOZ_ASSERT(id.isAtom());
    if (idVal.isUndefined()) {
      MOZ_ASSERT(id.isAtom(cx_->names().undefined));
      writer.guardIsUndefined(valId);
    } else if (idVal.isNull()) {
      MOZ_ASSERT(id.isAtom(cx_->names().null));
      writer.guardIsNull(valId);
    } else {
      MOZ_ASSERT(idVal.isString());
      StringOperandId strId = writer.guardToString(valId);
      writer.guardSpecificAtom(strId, id.toAtom());
    }
  }
}

void GetPropIRGenerator::maybeEmitIdGuard(jsid id) {
  if (cacheKind_ == CacheKind::GetProp ||
      cacheKind_ == CacheKind::GetPropSuper) {
    // Constant PropertyName, no guards necessary.
    MOZ_ASSERT(&idVal_.toString()->asAtom() == id.toAtom());
    return;
  }

  MOZ_ASSERT(cacheKind_ == CacheKind::GetElem ||
             cacheKind_ == CacheKind::GetElemSuper);
  emitIdGuard(getElemKeyValueId(), idVal_, id);
}

void SetPropIRGenerator::maybeEmitIdGuard(jsid id) {
  if (cacheKind_ == CacheKind::SetProp) {
    // Constant PropertyName, no guards necessary.
    MOZ_ASSERT(&idVal_.toString()->asAtom() == id.toAtom());
    return;
  }

  MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
  emitIdGuard(setElemKeyValueId(), idVal_, id);
}

GetNameIRGenerator::GetNameIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, ICState state,
                                       HandleObject env,
                                       HandlePropertyName name)
    : IRGenerator(cx, script, pc, CacheKind::GetName, state),
      env_(env),
      name_(name) {}

AttachDecision GetNameIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::GetName);

  AutoAssertNoPendingException aanpe(cx_);

  ObjOperandId envId(writer.setInputOperandId(0));
  RootedId id(cx_, NameToId(name_));

  TRY_ATTACH(tryAttachGlobalNameValue(envId, id));
  TRY_ATTACH(tryAttachGlobalNameGetter(envId, id));
  TRY_ATTACH(tryAttachEnvironmentName(envId, id));

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

static bool CanAttachGlobalName(JSContext* cx,
                                GlobalLexicalEnvironmentObject* globalLexical,
                                PropertyKey id, NativeObject** holder,
                                Maybe<PropertyInfo>* prop) {
  // The property must be found, and it must be found as a normal data property.
  NativeObject* current = globalLexical;
  while (true) {
    *prop = current->lookup(cx, id);
    if (prop->isSome()) {
      break;
    }

    if (current == globalLexical) {
      current = &globalLexical->global();
    } else {
      // In the browser the global prototype chain should be immutable.
      if (!current->staticPrototypeIsImmutable()) {
        return false;
      }

      JSObject* proto = current->staticPrototype();
      if (!proto || !proto->is<NativeObject>()) {
        return false;
      }

      current = &proto->as<NativeObject>();
    }
  }

  *holder = current;
  return true;
}

AttachDecision GetNameIRGenerator::tryAttachGlobalNameValue(ObjOperandId objId,
                                                            HandleId id) {
  if (!IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope()) {
    return AttachDecision::NoAction;
  }

  auto* globalLexical = &env_->as<GlobalLexicalEnvironmentObject>();

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  if (!CanAttachGlobalName(cx_, globalLexical, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }

  // The property must be found, and it must be found as a normal data property.
  if (!prop->isDataProperty()) {
    return AttachDecision::NoAction;
  }

  // This might still be an uninitialized lexical.
  if (holder->getSlot(prop->slot()).isMagic()) {
    return AttachDecision::NoAction;
  }

  if (holder == globalLexical) {
    // There is no need to guard on the shape. Lexical bindings are
    // non-configurable, and this stub cannot be shared across globals.
    size_t dynamicSlotOffset =
        holder->dynamicSlotIndex(prop->slot()) * sizeof(Value);
    writer.loadDynamicSlotResult(objId, dynamicSlotOffset);
  } else {
    // Check the prototype chain from the global to the holder
    // prototype. Ignore the global lexical scope as it doesn't figure
    // into the prototype chain. We guard on the global lexical
    // scope's shape independently.
    if (!IsCacheableGetPropReadSlot(&globalLexical->global(), holder, *prop)) {
      return AttachDecision::NoAction;
    }

    // Shape guard for global lexical.
    writer.guardShape(objId, globalLexical->shape());

    // Guard on the shape of the GlobalObject.
    ObjOperandId globalId = writer.loadEnclosingEnvironment(objId);
    writer.guardShape(globalId, globalLexical->global().shape());

    ObjOperandId holderId = globalId;
    if (holder != &globalLexical->global()) {
      // Shape guard holder.
      holderId = writer.loadObject(holder);
      writer.guardShape(holderId, holder->shape());
    }

    EmitLoadSlotResult(writer, holderId, holder, *prop);
  }

  writer.returnFromIC();

  trackAttached("GlobalNameValue");
  return AttachDecision::Attach;
}

AttachDecision GetNameIRGenerator::tryAttachGlobalNameGetter(ObjOperandId objId,
                                                             HandleId id) {
  if (!IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope()) {
    return AttachDecision::NoAction;
  }

  Handle<GlobalLexicalEnvironmentObject*> globalLexical =
      env_.as<GlobalLexicalEnvironmentObject>();
  MOZ_ASSERT(globalLexical->isGlobal());

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  if (!CanAttachGlobalName(cx_, globalLexical, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }

  if (holder == globalLexical) {
    return AttachDecision::NoAction;
  }

  GlobalObject* global = &globalLexical->global();

  if (IsCacheableGetPropCall(global, holder, *prop) != CanAttachNativeGetter) {
    return AttachDecision::NoAction;
  }

  // Shape guard for global lexical.
  writer.guardShape(objId, globalLexical->shape());

  // Guard on the shape of the GlobalObject.
  ObjOperandId globalId = writer.loadEnclosingEnvironment(objId);
  writer.guardShape(globalId, global->shape());

  if (holder != global) {
    // Shape guard holder.
    ObjOperandId holderId = writer.loadObject(holder);
    writer.guardShape(holderId, holder->shape());
    EmitGuardGetterSetterSlot(writer, holder, *prop, holderId,
                              /* holderIsConstant = */ true);
  } else {
    // Note: pass true for |holderIsConstant| because the holder must be the
    // current global object.
    EmitGuardGetterSetterSlot(writer, holder, *prop, globalId,
                              /* holderIsConstant = */ true);
  }

  if (CanAttachDOMGetterSetter(cx_, JSJitInfo::Getter, global, holder, *prop,
                               mode_)) {
    // The global shape guard above ensures the instance JSClass is correct.
    EmitCallDOMGetterResultNoGuards(writer, holder, *prop, globalId);
    trackAttached("GlobalNameDOMGetter");
  } else {
    ValOperandId receiverId = writer.boxObject(globalId);
    EmitCallGetterResultNoGuards(cx_, writer, global, holder, *prop,
                                 receiverId);
    trackAttached("GlobalNameGetter");
  }

  return AttachDecision::Attach;
}

static bool NeedEnvironmentShapeGuard(JSObject* envObj) {
  if (!envObj->is<CallObject>()) {
    return true;
  }

  // We can skip a guard on the call object if the script's bindings are
  // guaranteed to be immutable (and thus cannot introduce shadowing variables).
  // If the function is a relazified self-hosted function it has no BaseScript
  // and we pessimistically create the guard.
  CallObject* callObj = &envObj->as<CallObject>();
  JSFunction* fun = &callObj->callee();
  if (!fun->hasBaseScript() || fun->baseScript()->funHasExtensibleScope()) {
    return true;
  }

  return false;
}

AttachDecision GetNameIRGenerator::tryAttachEnvironmentName(ObjOperandId objId,
                                                            HandleId id) {
  if (IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope()) {
    return AttachDecision::NoAction;
  }

  JSObject* env = env_;
  Maybe<PropertyInfo> prop;
  NativeObject* holder = nullptr;

  while (env) {
    if (env->is<GlobalObject>()) {
      prop = env->as<GlobalObject>().lookup(cx_, id);
      if (prop.isSome()) {
        break;
      }
      return AttachDecision::NoAction;
    }

    if (!env->is<EnvironmentObject>() || env->is<WithEnvironmentObject>()) {
      return AttachDecision::NoAction;
    }

    // Check for an 'own' property on the env. There is no need to
    // check the prototype as non-with scopes do not inherit properties
    // from any prototype.
    prop = env->as<NativeObject>().lookup(cx_, id);
    if (prop.isSome()) {
      break;
    }

    env = env->enclosingEnvironment();
  }

  holder = &env->as<NativeObject>();
  if (!IsCacheableGetPropReadSlot(holder, holder, *prop)) {
    return AttachDecision::NoAction;
  }
  if (holder->getSlot(prop->slot()).isMagic()) {
    return AttachDecision::NoAction;
  }

  ObjOperandId lastObjId = objId;
  env = env_;
  while (env) {
    if (NeedEnvironmentShapeGuard(env)) {
      writer.guardShape(lastObjId, env->shape());
    }

    if (env == holder) {
      break;
    }

    lastObjId = writer.loadEnclosingEnvironment(lastObjId);
    env = env->enclosingEnvironment();
  }

  if (holder->isFixedSlot(prop->slot())) {
    writer.loadEnvironmentFixedSlotResult(
        lastObjId, NativeObject::getFixedSlotOffset(prop->slot()));
  } else {
    size_t dynamicSlotOffset =
        holder->dynamicSlotIndex(prop->slot()) * sizeof(Value);
    writer.loadEnvironmentDynamicSlotResult(lastObjId, dynamicSlotOffset);
  }
  writer.returnFromIC();

  trackAttached("EnvironmentName");
  return AttachDecision::Attach;
}

void GetNameIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", ObjectValue(*env_));
    sp.valueProperty("property", StringValue(name_));
  }
#endif
}

BindNameIRGenerator::BindNameIRGenerator(JSContext* cx, HandleScript script,
                                         jsbytecode* pc, ICState state,
                                         HandleObject env,
                                         HandlePropertyName name)
    : IRGenerator(cx, script, pc, CacheKind::BindName, state),
      env_(env),
      name_(name) {}

AttachDecision BindNameIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::BindName);

  AutoAssertNoPendingException aanpe(cx_);

  ObjOperandId envId(writer.setInputOperandId(0));
  RootedId id(cx_, NameToId(name_));

  TRY_ATTACH(tryAttachGlobalName(envId, id));
  TRY_ATTACH(tryAttachEnvironmentName(envId, id));

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision BindNameIRGenerator::tryAttachGlobalName(ObjOperandId objId,
                                                        HandleId id) {
  if (!IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope()) {
    return AttachDecision::NoAction;
  }

  Handle<GlobalLexicalEnvironmentObject*> globalLexical =
      env_.as<GlobalLexicalEnvironmentObject>();
  MOZ_ASSERT(globalLexical->isGlobal());

  JSObject* result = nullptr;
  if (Maybe<PropertyInfo> prop = globalLexical->lookup(cx_, id)) {
    // If this is an uninitialized lexical or a const, we need to return a
    // RuntimeLexicalErrorObject.
    if (globalLexical->getSlot(prop->slot()).isMagic() || !prop->writable()) {
      return AttachDecision::NoAction;
    }
    result = globalLexical;
  } else {
    result = &globalLexical->global();
  }

  if (result == globalLexical) {
    // Lexical bindings are non-configurable so we can just return the
    // global lexical.
    writer.loadObjectResult(objId);
  } else {
    // If the property exists on the global and is non-configurable, it cannot
    // be shadowed by the lexical scope so we can just return the global without
    // a shape guard.
    Maybe<PropertyInfo> prop = result->as<GlobalObject>().lookup(cx_, id);
    if (prop.isNothing() || prop->configurable()) {
      writer.guardShape(objId, globalLexical->shape());
    }
    ObjOperandId globalId = writer.loadEnclosingEnvironment(objId);
    writer.loadObjectResult(globalId);
  }
  writer.returnFromIC();

  trackAttached("GlobalName");
  return AttachDecision::Attach;
}

AttachDecision BindNameIRGenerator::tryAttachEnvironmentName(ObjOperandId objId,
                                                             HandleId id) {
  if (IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope()) {
    return AttachDecision::NoAction;
  }

  JSObject* env = env_;
  Maybe<PropertyInfo> prop;
  while (true) {
    if (!env->is<GlobalObject>() && !env->is<EnvironmentObject>()) {
      return AttachDecision::NoAction;
    }
    if (env->is<WithEnvironmentObject>()) {
      return AttachDecision::NoAction;
    }

    // When we reach an unqualified variables object (like the global) we
    // have to stop looking and return that object.
    if (env->isUnqualifiedVarObj()) {
      break;
    }

    // Check for an 'own' property on the env. There is no need to
    // check the prototype as non-with scopes do not inherit properties
    // from any prototype.
    prop = env->as<NativeObject>().lookup(cx_, id);
    if (prop.isSome()) {
      break;
    }

    env = env->enclosingEnvironment();
  }

  // If this is an uninitialized lexical or a const, we need to return a
  // RuntimeLexicalErrorObject.
  auto* holder = &env->as<NativeObject>();
  if (prop.isSome() && holder->is<EnvironmentObject>() &&
      (holder->getSlot(prop->slot()).isMagic() || !prop->writable())) {
    return AttachDecision::NoAction;
  }

  ObjOperandId lastObjId = objId;
  env = env_;
  while (env) {
    if (NeedEnvironmentShapeGuard(env) && !env->is<GlobalObject>()) {
      writer.guardShape(lastObjId, env->shape());
    }

    if (env == holder) {
      break;
    }

    lastObjId = writer.loadEnclosingEnvironment(lastObjId);
    env = env->enclosingEnvironment();
  }
  writer.loadObjectResult(lastObjId);
  writer.returnFromIC();

  trackAttached("EnvironmentName");
  return AttachDecision::Attach;
}

void BindNameIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", ObjectValue(*env_));
    sp.valueProperty("property", StringValue(name_));
  }
#endif
}

HasPropIRGenerator::HasPropIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, ICState state,
                                       CacheKind cacheKind, HandleValue idVal,
                                       HandleValue val)
    : IRGenerator(cx, script, pc, cacheKind, state), val_(val), idVal_(idVal) {}

AttachDecision HasPropIRGenerator::tryAttachDense(HandleObject obj,
                                                  ObjOperandId objId,
                                                  uint32_t index,
                                                  Int32OperandId indexId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }

  // Guard shape to ensure object class is NativeObject.
  TestMatchingNativeReceiver(writer, nobj, objId);
  writer.loadDenseElementExistsResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("DenseHasProp");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachDenseHole(HandleObject obj,
                                                      ObjOperandId objId,
                                                      uint32_t index,
                                                      Int32OperandId indexId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }
  if (!CanAttachDenseElementHole(nobj, hasOwn)) {
    return AttachDecision::NoAction;
  }

  // Guard shape to ensure class is NativeObject and to prevent non-dense
  // elements being added. Also ensures prototype doesn't change if dynamic
  // checks aren't emitted.
  TestMatchingNativeReceiver(writer, nobj, objId);

  // Generate prototype guards if needed. This includes monitoring that
  // properties were not added in the chain.
  if (!hasOwn) {
    GeneratePrototypeHoleGuards(writer, nobj, objId,
                                /* alwaysGuardFirstProto = */ false);
  }

  writer.loadDenseElementHoleExistsResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("DenseHasPropHole");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachSparse(HandleObject obj,
                                                   ObjOperandId objId,
                                                   Int32OperandId indexId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }
  auto* nobj = &obj->as<NativeObject>();

  if (!nobj->isIndexed()) {
    return AttachDecision::NoAction;
  }
  if (!CanAttachDenseElementHole(nobj, hasOwn,
                                 /* allowIndexedReceiver = */ true)) {
    return AttachDecision::NoAction;
  }

  // Guard that this is a native object.
  writer.guardIsNativeObject(objId);

  // Generate prototype guards if needed. This includes monitoring that
  // properties were not added in the chain.
  if (!hasOwn) {
    GeneratePrototypeHoleGuards(writer, nobj, objId,
                                /* alwaysGuardFirstProto = */ true);
  }

  // Because of the prototype guard we know that the prototype chain
  // does not include any dense or sparse (i.e indexed) properties.
  writer.callObjectHasSparseElementResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("Sparse");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachNamedProp(HandleObject obj,
                                                      ObjOperandId objId,
                                                      HandleId key,
                                                      ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  NativeObject* holder = nullptr;
  PropertyResult prop;

  if (hasOwn) {
    if (!LookupOwnPropertyPure(cx_, obj, key, &prop)) {
      return AttachDecision::NoAction;
    }

    holder = &obj->as<NativeObject>();
  } else {
    if (!LookupPropertyPure(cx_, obj, key, &holder, &prop)) {
      return AttachDecision::NoAction;
    }
  }
  if (prop.isNotFound()) {
    return AttachDecision::NoAction;
  }
  auto* nobj = &obj->as<NativeObject>();

  TRY_ATTACH(tryAttachMegamorphic(objId, keyId));
  TRY_ATTACH(tryAttachNative(nobj, objId, key, keyId, prop, holder));

  return AttachDecision::NoAction;
}

AttachDecision HasPropIRGenerator::tryAttachMegamorphic(ObjOperandId objId,
                                                        ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  if (mode_ != ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  writer.megamorphicHasPropResult(objId, keyId, hasOwn);
  writer.returnFromIC();
  trackAttached("MegamorphicHasProp");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachNative(NativeObject* obj,
                                                   ObjOperandId objId, jsid key,
                                                   ValOperandId keyId,
                                                   PropertyResult prop,
                                                   NativeObject* holder) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  if (!prop.isNativeProperty()) {
    return AttachDecision::NoAction;
  }

  Maybe<ObjOperandId> tempId;
  emitIdGuard(keyId, idVal_, key);
  EmitReadSlotGuard(writer, obj, holder, objId, &tempId);
  writer.loadBooleanResult(true);
  writer.returnFromIC();

  trackAttached("NativeHasProp");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachTypedArray(HandleObject obj,
                                                       ObjOperandId objId,
                                                       ValOperandId keyId) {
  if (!obj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  int64_t index;
  if (!ValueIsInt64Index(idVal_, &index)) {
    return AttachDecision::NoAction;
  }

  writer.guardIsTypedArray(objId);
  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(idVal_, keyId, /* supportOOB = */ true);
  writer.loadTypedArrayElementExistsResult(objId, intPtrIndexId);
  writer.returnFromIC();

  trackAttached("TypedArrayObject");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachSlotDoesNotExist(
    NativeObject* obj, ObjOperandId objId, jsid key, ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  emitIdGuard(keyId, idVal_, key);
  if (hasOwn) {
    TestMatchingNativeReceiver(writer, obj, objId);
  } else {
    Maybe<ObjOperandId> tempId;
    EmitReadSlotGuard(writer, obj, nullptr, objId, &tempId);
  }
  writer.loadBooleanResult(false);
  writer.returnFromIC();

  trackAttached("DoesNotExist");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachDoesNotExist(HandleObject obj,
                                                         ObjOperandId objId,
                                                         HandleId key,
                                                         ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  // Check that property doesn't exist on |obj| or it's prototype chain. These
  // checks allow NativeObjects with a NativeObject prototype chain. They return
  // NoAction if unknown such as resolve hooks or proxies.
  if (hasOwn) {
    if (!CheckHasNoSuchOwnProperty(cx_, obj, key)) {
      return AttachDecision::NoAction;
    }
  } else {
    if (!CheckHasNoSuchProperty(cx_, obj, key)) {
      return AttachDecision::NoAction;
    }
  }
  auto* nobj = &obj->as<NativeObject>();

  TRY_ATTACH(tryAttachMegamorphic(objId, keyId));
  TRY_ATTACH(tryAttachSlotDoesNotExist(nobj, objId, key, keyId));

  return AttachDecision::NoAction;
}

AttachDecision HasPropIRGenerator::tryAttachProxyElement(HandleObject obj,
                                                         ObjOperandId objId,
                                                         ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  if (!obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  writer.guardIsProxy(objId);
  writer.proxyHasPropResult(objId, keyId, hasOwn);
  writer.returnFromIC();

  trackAttached("ProxyHasProp");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::In || cacheKind_ == CacheKind::HasOwn);

  AutoAssertNoPendingException aanpe(cx_);

  // NOTE: Argument order is PROPERTY, OBJECT
  ValOperandId keyId(writer.setInputOperandId(0));
  ValOperandId valId(writer.setInputOperandId(1));

  if (!val_.isObject()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }
  RootedObject obj(cx_, &val_.toObject());
  ObjOperandId objId = writer.guardToObject(valId);

  // Optimize Proxies
  TRY_ATTACH(tryAttachProxyElement(obj, objId, keyId));

  RootedId id(cx_);
  bool nameOrSymbol;
  if (!ValueToNameOrSymbolId(cx_, idVal_, &id, &nameOrSymbol)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  if (nameOrSymbol) {
    TRY_ATTACH(tryAttachNamedProp(obj, objId, id, keyId));
    TRY_ATTACH(tryAttachDoesNotExist(obj, objId, id, keyId));

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  TRY_ATTACH(tryAttachTypedArray(obj, objId, keyId));

  uint32_t index;
  Int32OperandId indexId;
  if (maybeGuardInt32Index(idVal_, keyId, &index, &indexId)) {
    TRY_ATTACH(tryAttachDense(obj, objId, index, indexId));
    TRY_ATTACH(tryAttachDenseHole(obj, objId, index, indexId));
    TRY_ATTACH(tryAttachSparse(obj, objId, indexId));

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

void HasPropIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", val_);
    sp.valueProperty("property", idVal_);
  }
#endif
}

CheckPrivateFieldIRGenerator::CheckPrivateFieldIRGenerator(
    JSContext* cx, HandleScript script, jsbytecode* pc, ICState state,
    CacheKind cacheKind, HandleValue idVal, HandleValue val)
    : IRGenerator(cx, script, pc, cacheKind, state), val_(val), idVal_(idVal) {
  MOZ_ASSERT(idVal.isSymbol() && idVal.toSymbol()->isPrivateName());
}

AttachDecision CheckPrivateFieldIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId valId(writer.setInputOperandId(0));
  ValOperandId keyId(writer.setInputOperandId(1));

  if (!val_.isObject()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }
  JSObject* obj = &val_.toObject();
  ObjOperandId objId = writer.guardToObject(valId);
  PropertyKey key = SYMBOL_TO_JSID(idVal_.toSymbol());

  ThrowCondition condition;
  ThrowMsgKind msgKind;
  GetCheckPrivateFieldOperands(pc_, &condition, &msgKind);

  bool hasOwn = false;
  if (!HasOwnDataPropertyPure(cx_, obj, key, &hasOwn)) {
    // Can't determine if HasOwnProperty purely.
    return AttachDecision::NoAction;
  }

  if (CheckPrivateFieldWillThrow(condition, hasOwn)) {
    // Don't attach a stub if the operation will throw.
    return AttachDecision::NoAction;
  }

  TRY_ATTACH(tryAttachNative(obj, objId, key, keyId, hasOwn));

  return AttachDecision::NoAction;
}

AttachDecision CheckPrivateFieldIRGenerator::tryAttachNative(JSObject* obj,
                                                             ObjOperandId objId,
                                                             jsid key,
                                                             ValOperandId keyId,
                                                             bool hasOwn) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }
  auto* nobj = &obj->as<NativeObject>();

  Maybe<ObjOperandId> tempId;
  emitIdGuard(keyId, idVal_, key);
  EmitReadSlotGuard(writer, nobj, nobj, objId, &tempId);
  writer.loadBooleanResult(hasOwn);
  writer.returnFromIC();

  trackAttached("NativeCheckPrivateField");
  return AttachDecision::Attach;
}

void CheckPrivateFieldIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", val_);
    sp.valueProperty("property", idVal_);
  }
#endif
}

bool IRGenerator::maybeGuardInt32Index(const Value& index, ValOperandId indexId,
                                       uint32_t* int32Index,
                                       Int32OperandId* int32IndexId) {
  if (index.isNumber()) {
    int32_t indexSigned;
    if (index.isInt32()) {
      indexSigned = index.toInt32();
    } else {
      // We allow negative zero here.
      if (!mozilla::NumberEqualsInt32(index.toDouble(), &indexSigned)) {
        return false;
      }
    }

    if (indexSigned < 0) {
      return false;
    }

    *int32Index = uint32_t(indexSigned);
    *int32IndexId = writer.guardToInt32Index(indexId);
    return true;
  }

  if (index.isString()) {
    int32_t indexSigned = GetIndexFromString(index.toString());
    if (indexSigned < 0) {
      return false;
    }

    StringOperandId strId = writer.guardToString(indexId);
    *int32Index = uint32_t(indexSigned);
    *int32IndexId = writer.guardStringToIndex(strId);
    return true;
  }

  return false;
}

SetPropIRGenerator::SetPropIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, CacheKind cacheKind,
                                       ICState state, HandleValue lhsVal,
                                       HandleValue idVal, HandleValue rhsVal)
    : IRGenerator(cx, script, pc, cacheKind, state),
      lhsVal_(lhsVal),
      idVal_(idVal),
      rhsVal_(rhsVal) {}

AttachDecision SetPropIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId objValId(writer.setInputOperandId(0));
  ValOperandId rhsValId;
  if (cacheKind_ == CacheKind::SetProp) {
    rhsValId = ValOperandId(writer.setInputOperandId(1));
  } else {
    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    MOZ_ASSERT(setElemKeyValueId().id() == 1);
    writer.setInputOperandId(1);
    rhsValId = ValOperandId(writer.setInputOperandId(2));
  }

  RootedId id(cx_);
  bool nameOrSymbol;
  if (!ValueToNameOrSymbolId(cx_, idVal_, &id, &nameOrSymbol)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  if (lhsVal_.isObject()) {
    RootedObject obj(cx_, &lhsVal_.toObject());

    ObjOperandId objId = writer.guardToObject(objValId);
    if (IsPropertySetOp(JSOp(*pc_))) {
      TRY_ATTACH(tryAttachMegamorphicSetElement(obj, objId, rhsValId));
    }
    if (nameOrSymbol) {
      TRY_ATTACH(tryAttachNativeSetSlot(obj, objId, id, rhsValId));
      if (IsPropertySetOp(JSOp(*pc_))) {
        TRY_ATTACH(tryAttachSetArrayLength(obj, objId, id, rhsValId));
        TRY_ATTACH(tryAttachSetter(obj, objId, id, rhsValId));
        TRY_ATTACH(tryAttachWindowProxy(obj, objId, id, rhsValId));
        TRY_ATTACH(tryAttachProxy(obj, objId, id, rhsValId));
      }
      if (canAttachAddSlotStub(obj, id)) {
        deferType_ = DeferType::AddSlot;
        return AttachDecision::Deferred;
      }
      return AttachDecision::NoAction;
    }

    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);

    if (IsPropertySetOp(JSOp(*pc_))) {
      TRY_ATTACH(tryAttachProxyElement(obj, objId, rhsValId));
    }

    TRY_ATTACH(tryAttachSetTypedArrayElement(obj, objId, rhsValId));

    uint32_t index;
    Int32OperandId indexId;
    if (maybeGuardInt32Index(idVal_, setElemKeyValueId(), &index, &indexId)) {
      TRY_ATTACH(
          tryAttachSetDenseElement(obj, objId, index, indexId, rhsValId));
      TRY_ATTACH(
          tryAttachSetDenseElementHole(obj, objId, index, indexId, rhsValId));
      TRY_ATTACH(tryAttachAddOrUpdateSparseElement(obj, objId, index, indexId,
                                                   rhsValId));
      return AttachDecision::NoAction;
    }
  }
  return AttachDecision::NoAction;
}

static void EmitStoreSlotAndReturn(CacheIRWriter& writer, ObjOperandId objId,
                                   NativeObject* nobj, PropertyInfo prop,
                                   ValOperandId rhsId) {
  if (nobj->isFixedSlot(prop.slot())) {
    size_t offset = NativeObject::getFixedSlotOffset(prop.slot());
    writer.storeFixedSlot(objId, offset, rhsId);
  } else {
    size_t offset = nobj->dynamicSlotIndex(prop.slot()) * sizeof(Value);
    writer.storeDynamicSlot(objId, offset, rhsId);
  }
  writer.returnFromIC();
}

static Maybe<PropertyInfo> LookupShapeForSetSlot(JSOp op, NativeObject* obj,
                                                 jsid id) {
  Maybe<PropertyInfo> prop = obj->lookupPure(id);
  if (prop.isNothing() || !prop->isDataProperty() || !prop->writable()) {
    return mozilla::Nothing();
  }

  // If this is an op like JSOp::InitElem / [[DefineOwnProperty]], the
  // property's attributes may have to be changed too, so make sure it's a
  // simple data property.
  if (IsPropertyInitOp(op) && (!prop->configurable() || !prop->enumerable())) {
    return mozilla::Nothing();
  }

  return prop;
}

static bool CanAttachNativeSetSlot(JSOp op, JSObject* obj, PropertyKey id,
                                   Maybe<PropertyInfo>* prop) {
  if (!obj->is<NativeObject>()) {
    return false;
  }

  *prop = LookupShapeForSetSlot(op, &obj->as<NativeObject>(), id);
  return prop->isSome();
}

// There is no need to guard on the shape. Global lexical bindings are
// non-configurable and can not be shadowed.
static bool IsGlobalLexicalSetGName(JSOp op, NativeObject* obj,
                                    PropertyInfo prop) {
  // Ensure that the env can't change.
  if (op != JSOp::SetGName && op != JSOp::StrictSetGName) {
    return false;
  }

  if (!obj->is<GlobalLexicalEnvironmentObject>()) {
    return false;
  }

  // Uninitialized let bindings use a RuntimeLexicalErrorObject.
  MOZ_ASSERT(!obj->getSlot(prop.slot()).isMagic());
  MOZ_ASSERT(prop.writable());
  MOZ_ASSERT(!prop.configurable());
  return true;
}

AttachDecision SetPropIRGenerator::tryAttachNativeSetSlot(HandleObject obj,
                                                          ObjOperandId objId,
                                                          HandleId id,
                                                          ValOperandId rhsId) {
  Maybe<PropertyInfo> prop;
  if (!CanAttachNativeSetSlot(JSOp(*pc_), obj, id, &prop)) {
    return AttachDecision::NoAction;
  }

  // Don't attach a megamorphic store slot stub for ops like JSOp::InitElem.
  if (mode_ == ICState::Mode::Megamorphic && cacheKind_ == CacheKind::SetProp &&
      IsPropertySetOp(JSOp(*pc_))) {
    writer.megamorphicStoreSlot(objId, id.toAtom()->asPropertyName(), rhsId);
    writer.returnFromIC();
    trackAttached("MegamorphicNativeSlot");
    return AttachDecision::Attach;
  }

  maybeEmitIdGuard(id);

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!IsGlobalLexicalSetGName(JSOp(*pc_), nobj, *prop)) {
    TestMatchingNativeReceiver(writer, nobj, objId);
  }

  EmitStoreSlotAndReturn(writer, objId, nobj, *prop, rhsId);

  trackAttached("NativeSlot");
  return AttachDecision::Attach;
}

OperandId IRGenerator::emitNumericGuard(ValOperandId valId, Scalar::Type type) {
  switch (type) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
      return writer.guardToInt32ModUint32(valId);

    case Scalar::Float32:
    case Scalar::Float64:
      return writer.guardIsNumber(valId);

    case Scalar::Uint8Clamped:
      return writer.guardToUint8Clamped(valId);

    case Scalar::BigInt64:
    case Scalar::BigUint64:
      return writer.guardToBigInt(valId);

    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      break;
  }
  MOZ_CRASH("Unsupported TypedArray type");
}

static bool ValueIsNumeric(Scalar::Type type, const Value& val) {
  if (Scalar::isBigIntType(type)) {
    return val.isBigInt();
  }
  return val.isNumber();
}

void SetPropIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.opcodeProperty("op", JSOp(*pc_));
    sp.valueProperty("base", lhsVal_);
    sp.valueProperty("property", idVal_);
    sp.valueProperty("value", rhsVal_);
  }
#endif
}

static bool IsCacheableSetPropCallNative(NativeObject* obj,
                                         NativeObject* holder,
                                         PropertyInfo prop) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  if (!prop.isAccessorProperty()) {
    return false;
  }

  JSObject* setterObject = holder->getSetter(prop);
  if (!setterObject || !setterObject->is<JSFunction>()) {
    return false;
  }

  JSFunction& setter = setterObject->as<JSFunction>();
  if (!setter.isNativeWithoutJitEntry()) {
    return false;
  }

  if (setter.isClassConstructor()) {
    return false;
  }

  if (setter.hasJitInfo() && !setter.jitInfo()->needsOuterizedThisObject()) {
    return true;
  }

  return !IsWindow(obj);
}

static bool IsCacheableSetPropCallScripted(NativeObject* obj,
                                           NativeObject* holder,
                                           PropertyInfo prop) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  if (IsWindow(obj)) {
    return false;
  }

  if (!prop.isAccessorProperty()) {
    return false;
  }

  JSObject* setterObject = holder->getSetter(prop);
  if (!setterObject || !setterObject->is<JSFunction>()) {
    return false;
  }

  JSFunction& setter = setterObject->as<JSFunction>();
  if (setter.isClassConstructor()) {
    return false;
  }

  // Scripted functions and natives with JIT entry can use the scripted path.
  return setter.hasJitEntry();
}

static bool CanAttachSetter(JSContext* cx, jsbytecode* pc, JSObject* obj,
                            PropertyKey id, NativeObject** holder,
                            Maybe<PropertyInfo>* propInfo) {
  // Don't attach a setter stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc)));

  PropertyResult prop;
  if (!LookupPropertyPure(cx, obj, id, holder, &prop)) {
    return false;
  }
  auto* nobj = &obj->as<NativeObject>();

  if (!prop.isNativeProperty()) {
    return false;
  }

  if (!IsCacheableSetPropCallScripted(nobj, *holder, prop.propertyInfo()) &&
      !IsCacheableSetPropCallNative(nobj, *holder, prop.propertyInfo())) {
    return false;
  }

  *propInfo = mozilla::Some(prop.propertyInfo());
  return true;
}

static void EmitCallSetterNoGuards(JSContext* cx, CacheIRWriter& writer,
                                   NativeObject* obj, NativeObject* holder,
                                   PropertyInfo prop, ObjOperandId objId,
                                   ValOperandId rhsId) {
  JSFunction* target = &holder->getSetter(prop)->as<JSFunction>();
  bool sameRealm = cx->realm() == target->realm();

  if (target->isNativeWithoutJitEntry()) {
    MOZ_ASSERT(IsCacheableSetPropCallNative(obj, holder, prop));
    writer.callNativeSetter(objId, target, rhsId, sameRealm);
    writer.returnFromIC();
    return;
  }

  MOZ_ASSERT(IsCacheableSetPropCallScripted(obj, holder, prop));
  writer.callScriptedSetter(objId, target, rhsId, sameRealm);
  writer.returnFromIC();
}

static void EmitCallDOMSetterNoGuards(JSContext* cx, CacheIRWriter& writer,
                                      NativeObject* holder, PropertyInfo prop,
                                      ObjOperandId objId, ValOperandId rhsId) {
  JSFunction* setter = &holder->getSetter(prop)->as<JSFunction>();
  MOZ_ASSERT(cx->realm() == setter->realm());

  writer.callDOMSetter(objId, setter->jitInfo(), rhsId);
  writer.returnFromIC();
}

AttachDecision SetPropIRGenerator::tryAttachSetter(HandleObject obj,
                                                   ObjOperandId objId,
                                                   HandleId id,
                                                   ValOperandId rhsId) {
  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  if (!CanAttachSetter(cx_, pc_, obj, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }
  auto* nobj = &obj->as<NativeObject>();

  maybeEmitIdGuard(id);

  // Use the megamorphic guard if we're in megamorphic mode, except if |obj|
  // is a Window as GuardHasGetterSetter doesn't support this yet (Window may
  // require outerizing).
  if (mode_ == ICState::Mode::Specialized || IsWindow(nobj)) {
    TestMatchingNativeReceiver(writer, nobj, objId);

    if (nobj != holder) {
      GeneratePrototypeGuards(writer, nobj, holder, objId);

      // Guard on the holder's shape.
      ObjOperandId holderId = writer.loadObject(holder);
      TestMatchingHolder(writer, holder, holderId);

      EmitGuardGetterSetterSlot(writer, holder, *prop, holderId,
                                /* holderIsConstant = */ true);
    } else {
      EmitGuardGetterSetterSlot(writer, holder, *prop, objId);
    }
  } else {
    GetterSetter* gs = holder->getGetterSetter(*prop);
    writer.guardHasGetterSetter(objId, id, gs);
  }

  if (CanAttachDOMGetterSetter(cx_, JSJitInfo::Setter, nobj, holder, *prop,
                               mode_)) {
    EmitCallDOMSetterNoGuards(cx_, writer, holder, *prop, objId, rhsId);

    trackAttached("DOMSetter");
    return AttachDecision::Attach;
  }

  EmitCallSetterNoGuards(cx_, writer, nobj, holder, *prop, objId, rhsId);

  trackAttached("Setter");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachSetArrayLength(HandleObject obj,
                                                           ObjOperandId objId,
                                                           HandleId id,
                                                           ValOperandId rhsId) {
  // Don't attach an array length stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  if (!obj->is<ArrayObject>() || !id.isAtom(cx_->names().length) ||
      !obj->as<ArrayObject>().lengthIsWritable()) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);
  writer.guardClass(objId, GuardClassKind::Array);
  writer.callSetArrayLength(objId, IsStrictSetPC(pc_), rhsId);
  writer.returnFromIC();

  trackAttached("SetArrayLength");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachSetDenseElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId, ValOperandId rhsId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->containsDenseElement(index) || nobj->denseElementsAreFrozen()) {
    return AttachDecision::NoAction;
  }

  // Setting holes requires extra code for marking the elements non-packed.
  MOZ_ASSERT(!rhsVal_.isMagic(JS_ELEMENTS_HOLE));

  // Don't optimize InitElem (DefineProperty) on non-extensible objects: when
  // the elements are sealed, we have to throw an exception. Note that we have
  // to check !isExtensible instead of denseElementsAreSealed because sealing
  // a (non-extensible) object does not necessarily trigger a Shape change.
  if (IsPropertyInitOp(JSOp(*pc_)) && !nobj->isExtensible()) {
    return AttachDecision::NoAction;
  }

  TestMatchingNativeReceiver(writer, nobj, objId);

  writer.storeDenseElement(objId, indexId, rhsId);
  writer.returnFromIC();

  trackAttached("SetDenseElement");
  return AttachDecision::Attach;
}

static bool CanAttachAddElement(NativeObject* obj, bool isInit) {
  // Make sure the objects on the prototype don't have any indexed properties
  // or that such properties can't appear without a shape change.
  do {
    // The first two checks are also relevant to the receiver object.
    if (obj->isIndexed()) {
      return false;
    }

    const JSClass* clasp = obj->getClass();
    if (clasp != &ArrayObject::class_ &&
        (clasp->getAddProperty() || clasp->getResolve() ||
         clasp->getOpsLookupProperty() || clasp->getOpsSetProperty())) {
      return false;
    }

    // If we're initializing a property instead of setting one, the objects
    // on the prototype are not relevant.
    if (isInit) {
      break;
    }

    JSObject* proto = obj->staticPrototype();
    if (!proto) {
      break;
    }

    if (!proto->is<NativeObject>()) {
      return false;
    }

    // We have to make sure the proto has no non-writable (frozen) elements
    // because we're not allowed to shadow them.
    NativeObject* nproto = &proto->as<NativeObject>();
    if (nproto->denseElementsAreFrozen() &&
        nproto->getDenseInitializedLength() > 0) {
      return false;
    }

    obj = nproto;
  } while (true);

  return true;
}

AttachDecision SetPropIRGenerator::tryAttachSetDenseElementHole(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId, ValOperandId rhsId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  // Setting holes requires extra code for marking the elements non-packed.
  if (rhsVal_.isMagic(JS_ELEMENTS_HOLE)) {
    return AttachDecision::NoAction;
  }

  JSOp op = JSOp(*pc_);
  MOZ_ASSERT(IsPropertySetOp(op) || IsPropertyInitOp(op));

  if (op == JSOp::InitHiddenElem) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->isExtensible()) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(!nobj->denseElementsAreFrozen(),
             "Extensible objects should not have frozen elements");

  uint32_t initLength = nobj->getDenseInitializedLength();

  // Optimize if we're adding an element at initLength or writing to a hole.
  //
  // In the case where index > initLength, we need noteHasDenseAdd to be called
  // to ensure Ion is aware that writes have occurred to-out-of-bound indexes
  // before.
  bool isAdd = index == initLength;
  bool isHoleInBounds =
      index < initLength && !nobj->containsDenseElement(index);
  if (!isAdd && !isHoleInBounds) {
    return AttachDecision::NoAction;
  }

  // Can't add new elements to arrays with non-writable length.
  if (isAdd && nobj->is<ArrayObject>() &&
      !nobj->as<ArrayObject>().lengthIsWritable()) {
    return AttachDecision::NoAction;
  }

  // Typed arrays don't have dense elements.
  if (nobj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Check for other indexed properties or class hooks.
  if (!CanAttachAddElement(nobj, IsPropertyInitOp(op))) {
    return AttachDecision::NoAction;
  }

  TestMatchingNativeReceiver(writer, nobj, objId);

  // Also shape guard the proto chain, unless this is an InitElem.
  if (IsPropertySetOp(op)) {
    ShapeGuardProtoChain(writer, nobj, objId);
  }

  writer.storeDenseElementHole(objId, indexId, rhsId, isAdd);
  writer.returnFromIC();

  trackAttached(isAdd ? "AddDenseElement" : "StoreDenseElementHole");
  return AttachDecision::Attach;
}

// Add an IC for adding or updating a sparse array element.
AttachDecision SetPropIRGenerator::tryAttachAddOrUpdateSparseElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId, ValOperandId rhsId) {
  JSOp op = JSOp(*pc_);
  MOZ_ASSERT(IsPropertySetOp(op) || IsPropertyInitOp(op));

  if (op != JSOp::SetElem && op != JSOp::StrictSetElem) {
    return AttachDecision::NoAction;
  }

  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }
  NativeObject* nobj = &obj->as<NativeObject>();

  // We cannot attach a stub to a non-extensible object
  if (!nobj->isExtensible()) {
    return AttachDecision::NoAction;
  }

  // Stub doesn't handle negative indices.
  if (index > INT_MAX) {
    return AttachDecision::NoAction;
  }

  // We also need to be past the end of the dense capacity, to ensure sparse.
  if (index < nobj->getDenseInitializedLength()) {
    return AttachDecision::NoAction;
  }

  // Only handle Array objects in this stub.
  if (!nobj->is<ArrayObject>()) {
    return AttachDecision::NoAction;
  }
  ArrayObject* aobj = &nobj->as<ArrayObject>();

  // Don't attach if we're adding to an array with non-writable length.
  bool isAdd = (index >= aobj->length());
  if (isAdd && !aobj->lengthIsWritable()) {
    return AttachDecision::NoAction;
  }

  // Indexed properties on the prototype chain aren't handled by the helper.
  if ((aobj->staticPrototype() != nullptr) &&
      ObjectMayHaveExtraIndexedProperties(aobj->staticPrototype())) {
    return AttachDecision::NoAction;
  }

  // Ensure we are still talking about an array class.
  writer.guardClass(objId, GuardClassKind::Array);

  // The helper we are going to call only applies to non-dense elements.
  writer.guardIndexGreaterThanDenseInitLength(objId, indexId);

  // Guard extensible: We may be trying to add a new element, and so we'd best
  // be able to do so safely.
  writer.guardIsExtensible(objId);

  // Ensures we are able to efficiently able to map to an integral jsid.
  writer.guardInt32IsNonNegative(indexId);

  // Shape guard the prototype chain to avoid shadowing indexes from appearing.
  // Guard the prototype of the receiver explicitly, because the receiver's
  // shape is not being guarded as a proxy for that.
  GuardReceiverProto(writer, aobj, objId);

  // Dense elements may appear on the prototype chain (and prototypes may
  // have a different notion of which elements are dense), but they can
  // only be data properties, so our specialized Set handler is ok to bind
  // to them.
  ShapeGuardProtoChain(writer, aobj, objId);

  // Ensure that if we're adding an element to the object, the object's
  // length is writable.
  writer.guardIndexIsValidUpdateOrAdd(objId, indexId);

  writer.callAddOrUpdateSparseElementHelper(
      objId, indexId, rhsId,
      /* strict = */ op == JSOp::StrictSetElem);
  writer.returnFromIC();

  trackAttached("AddOrUpdateSparseElement");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachSetTypedArrayElement(
    HandleObject obj, ObjOperandId objId, ValOperandId rhsId) {
  if (!obj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }
  if (!idVal_.isNumber()) {
    return AttachDecision::NoAction;
  }

  TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
  Scalar::Type elementType = tarr->type();

  // Don't attach if the input type doesn't match the guard added below.
  if (!ValueIsNumeric(elementType, rhsVal_)) {
    return AttachDecision::NoAction;
  }

  bool handleOOB = false;
  int64_t indexInt64;
  if (!ValueIsInt64Index(idVal_, &indexInt64) || indexInt64 < 0 ||
      uint64_t(indexInt64) >= tarr->length()) {
    handleOOB = true;
  }

  // InitElem (DefineProperty) has to throw an exception on out-of-bounds.
  if (handleOOB && IsPropertyInitOp(JSOp(*pc_))) {
    return AttachDecision::NoAction;
  }

  writer.guardShapeForClass(objId, tarr->shape());

  OperandId rhsValId = emitNumericGuard(rhsId, elementType);

  ValOperandId keyId = setElemKeyValueId();
  IntPtrOperandId indexId = guardToIntPtrIndex(idVal_, keyId, handleOOB);

  writer.storeTypedArrayElement(objId, elementType, indexId, rhsValId,
                                handleOOB);
  writer.returnFromIC();

  trackAttached(handleOOB ? "SetTypedElementOOB" : "SetTypedElement");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachGenericProxy(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId rhsId, bool handleDOMProxies) {
  writer.guardIsProxy(objId);

  if (!handleDOMProxies) {
    // Ensure that the incoming object is not a DOM proxy, so that we can
    // get to the specialized stubs. If handleDOMProxies is true, we were
    // unable to attach a specialized DOM stub, so we just handle all
    // proxies here.
    writer.guardIsNotDOMProxy(objId);
  }

  if (cacheKind_ == CacheKind::SetProp || mode_ == ICState::Mode::Specialized) {
    maybeEmitIdGuard(id);
    writer.proxySet(objId, id, rhsId, IsStrictSetPC(pc_));
  } else {
    // Attach a stub that handles every id.
    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);
    writer.proxySetByValue(objId, setElemKeyValueId(), rhsId,
                           IsStrictSetPC(pc_));
  }

  writer.returnFromIC();

  trackAttached("GenericProxy");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachDOMProxyShadowed(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId rhsId) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  maybeEmitIdGuard(id);
  TestMatchingProxyReceiver(writer, obj, objId);
  writer.proxySet(objId, id, rhsId, IsStrictSetPC(pc_));
  writer.returnFromIC();

  trackAttached("DOMProxyShadowed");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachDOMProxyUnshadowed(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId rhsId) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  JSObject* proto = obj->staticPrototype();
  if (!proto) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  if (!CanAttachSetter(cx_, pc_, proto, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }
  auto* nproto = &proto->as<NativeObject>();

  maybeEmitIdGuard(id);

  // Guard that our expando object hasn't started shadowing this property.
  TestMatchingProxyReceiver(writer, obj, objId);
  CheckDOMProxyExpandoDoesNotShadow(writer, obj, id, objId);

  GeneratePrototypeGuards(writer, obj, holder, objId);

  // Guard on the holder of the property.
  ObjOperandId holderId = writer.loadObject(holder);
  TestMatchingHolder(writer, holder, holderId);

  EmitGuardGetterSetterSlot(writer, holder, *prop, holderId,
                            /* holderIsConstant = */ true);

  // EmitCallSetterNoGuards expects |obj| to be the object the property is
  // on to do some checks. Since we actually looked at proto, and no extra
  // guards will be generated, we can just pass that instead.
  EmitCallSetterNoGuards(cx_, writer, nproto, holder, *prop, objId, rhsId);

  trackAttached("DOMProxyUnshadowed");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachDOMProxyExpando(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId rhsId) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  Value expandoVal = GetProxyPrivate(obj);
  JSObject* expandoObj;
  if (expandoVal.isObject()) {
    expandoObj = &expandoVal.toObject();
  } else {
    MOZ_ASSERT(!expandoVal.isUndefined(),
               "How did a missing expando manage to shadow things?");
    auto expandoAndGeneration =
        static_cast<ExpandoAndGeneration*>(expandoVal.toPrivate());
    MOZ_ASSERT(expandoAndGeneration);
    expandoObj = &expandoAndGeneration->expando.toObject();
  }

  Maybe<PropertyInfo> prop;
  if (CanAttachNativeSetSlot(JSOp(*pc_), expandoObj, id, &prop)) {
    auto* nativeExpandoObj = &expandoObj->as<NativeObject>();

    maybeEmitIdGuard(id);
    ObjOperandId expandoObjId = guardDOMProxyExpandoObjectAndShape(
        obj, objId, expandoVal, nativeExpandoObj);

    EmitStoreSlotAndReturn(writer, expandoObjId, nativeExpandoObj, *prop,
                           rhsId);
    trackAttached("DOMProxyExpandoSlot");
    return AttachDecision::Attach;
  }

  NativeObject* holder = nullptr;
  if (CanAttachSetter(cx_, pc_, expandoObj, id, &holder, &prop)) {
    auto* nativeExpandoObj = &expandoObj->as<NativeObject>();

    // Call the setter. Note that we pass objId, the DOM proxy, as |this|
    // and not the expando object.
    maybeEmitIdGuard(id);
    ObjOperandId expandoObjId = guardDOMProxyExpandoObjectAndShape(
        obj, objId, expandoVal, nativeExpandoObj);

    MOZ_ASSERT(holder == nativeExpandoObj);
    EmitGuardGetterSetterSlot(writer, nativeExpandoObj, *prop, expandoObjId);
    EmitCallSetterNoGuards(cx_, writer, nativeExpandoObj, nativeExpandoObj,
                           *prop, objId, rhsId);
    trackAttached("DOMProxyExpandoSetter");
    return AttachDecision::Attach;
  }

  return AttachDecision::NoAction;
}

AttachDecision SetPropIRGenerator::tryAttachProxy(HandleObject obj,
                                                  ObjOperandId objId,
                                                  HandleId id,
                                                  ValOperandId rhsId) {
  // Don't attach a proxy stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  ProxyStubType type = GetProxyStubType(cx_, obj, id);
  if (type == ProxyStubType::None) {
    return AttachDecision::NoAction;
  }
  auto proxy = obj.as<ProxyObject>();

  if (mode_ == ICState::Mode::Megamorphic) {
    return tryAttachGenericProxy(proxy, objId, id, rhsId,
                                 /* handleDOMProxies = */ true);
  }

  switch (type) {
    case ProxyStubType::None:
      break;
    case ProxyStubType::DOMExpando:
      TRY_ATTACH(tryAttachDOMProxyExpando(proxy, objId, id, rhsId));
      [[fallthrough]];  // Fall through to the generic shadowed case.
    case ProxyStubType::DOMShadowed:
      return tryAttachDOMProxyShadowed(proxy, objId, id, rhsId);
    case ProxyStubType::DOMUnshadowed:
      TRY_ATTACH(tryAttachDOMProxyUnshadowed(proxy, objId, id, rhsId));
      return tryAttachGenericProxy(proxy, objId, id, rhsId,
                                   /* handleDOMProxies = */ true);
    case ProxyStubType::Generic:
      return tryAttachGenericProxy(proxy, objId, id, rhsId,
                                   /* handleDOMProxies = */ false);
  }

  MOZ_CRASH("Unexpected ProxyStubType");
}

AttachDecision SetPropIRGenerator::tryAttachProxyElement(HandleObject obj,
                                                         ObjOperandId objId,
                                                         ValOperandId rhsId) {
  // Don't attach a proxy stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  if (!obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  writer.guardIsProxy(objId);

  // Like GetPropIRGenerator::tryAttachProxyElement, don't check for DOM
  // proxies here as we don't have specialized DOM stubs for this.
  MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
  writer.proxySetByValue(objId, setElemKeyValueId(), rhsId, IsStrictSetPC(pc_));
  writer.returnFromIC();

  trackAttached("ProxyElement");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachMegamorphicSetElement(
    HandleObject obj, ObjOperandId objId, ValOperandId rhsId) {
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  if (mode_ != ICState::Mode::Megamorphic || cacheKind_ != CacheKind::SetElem) {
    return AttachDecision::NoAction;
  }

  // The generic proxy stubs are faster.
  if (obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  writer.megamorphicSetElement(objId, setElemKeyValueId(), rhsId,
                               IsStrictSetPC(pc_));
  writer.returnFromIC();

  trackAttached("MegamorphicSetElement");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachWindowProxy(HandleObject obj,
                                                        ObjOperandId objId,
                                                        HandleId id,
                                                        ValOperandId rhsId) {
  // Attach a stub when the receiver is a WindowProxy and we can do the set
  // on the Window (the global object).

  if (!IsWindowProxyForScriptGlobal(script_, obj)) {
    return AttachDecision::NoAction;
  }

  // If we're megamorphic prefer a generic proxy stub that handles a lot more
  // cases.
  if (mode_ == ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  // Now try to do the set on the Window (the current global).
  GlobalObject* windowObj = cx_->global();

  Maybe<PropertyInfo> prop;
  if (!CanAttachNativeSetSlot(JSOp(*pc_), windowObj, id, &prop)) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);

  ObjOperandId windowObjId =
      GuardAndLoadWindowProxyWindow(writer, objId, windowObj);
  writer.guardShape(windowObjId, windowObj->shape());

  EmitStoreSlotAndReturn(writer, windowObjId, windowObj, *prop, rhsId);

  trackAttached("WindowProxySlot");
  return AttachDecision::Attach;
}

bool SetPropIRGenerator::canAttachAddSlotStub(HandleObject obj, HandleId id) {
  // Special-case JSFunction resolve hook to allow redefining the 'prototype'
  // property without triggering lazy expansion of property and object
  // allocation.
  if (obj->is<JSFunction>() && id.isAtom(cx_->names().prototype)) {
    MOZ_ASSERT(ClassMayResolveId(cx_->names(), obj->getClass(), id, obj));

    // We're only interested in functions that have a builtin .prototype
    // property (needsPrototypeProperty). The stub will guard on this because
    // the builtin .prototype property is non-configurable/non-enumerable and it
    // would be wrong to add a property with those attributes to a function that
    // doesn't have a builtin .prototype.
    //
    // Inlining needsPrototypeProperty in JIT code is complicated so we use
    // isNonBuiltinConstructor as a stronger condition that's easier to check
    // from JIT code.
    JSFunction* fun = &obj->as<JSFunction>();
    if (!fun->isNonBuiltinConstructor()) {
      return false;
    }
    MOZ_ASSERT(fun->needsPrototypeProperty());

    // If property exists this isn't an "add".
    if (fun->lookupPure(id)) {
      return false;
    }
  } else {
    // Normal Case: If property exists this isn't an "add"
    PropertyResult prop;
    if (!LookupOwnPropertyPure(cx_, obj, id, &prop)) {
      return false;
    }
    if (prop.isFound()) {
      return false;
    }
  }

  // Object must be extensible, or we must be initializing a private
  // elem.
  bool canAddNewProperty = obj->nonProxyIsExtensible() || id.isPrivateName();
  if (!canAddNewProperty) {
    return false;
  }

  // Also watch out for addProperty hooks. Ignore the Array addProperty hook,
  // because it doesn't do anything for non-index properties.
  DebugOnly<uint32_t> index;
  MOZ_ASSERT_IF(obj->is<ArrayObject>(), !IdIsIndex(id, &index));
  if (!obj->is<ArrayObject>() && obj->getClass()->getAddProperty()) {
    return false;
  }

  // Walk up the object prototype chain and ensure that all prototypes are
  // native, and that all prototypes have no setter defined on the property.
  for (JSObject* proto = obj->staticPrototype(); proto;
       proto = proto->staticPrototype()) {
    if (!proto->is<NativeObject>()) {
      return false;
    }

    // If prototype defines this property in a non-plain way, don't optimize.
    Maybe<PropertyInfo> protoProp = proto->as<NativeObject>().lookup(cx_, id);
    if (protoProp.isSome() && !protoProp->isDataProperty()) {
      return false;
    }

    // Otherwise, if there's no such property, watch out for a resolve hook
    // that would need to be invoked and thus prevent inlining of property
    // addition. Allow the JSFunction resolve hook as it only defines plain
    // data properties and we don't need to invoke it for objects on the
    // proto chain.
    if (ClassMayResolveId(cx_->names(), proto->getClass(), id, proto) &&
        !proto->is<JSFunction>()) {
      return false;
    }
  }

  return true;
}

AttachDecision SetPropIRGenerator::tryAttachAddSlotStub(HandleShape oldShape) {
  ValOperandId objValId(writer.setInputOperandId(0));
  ValOperandId rhsValId;
  if (cacheKind_ == CacheKind::SetProp) {
    rhsValId = ValOperandId(writer.setInputOperandId(1));
  } else {
    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    MOZ_ASSERT(setElemKeyValueId().id() == 1);
    writer.setInputOperandId(1);
    rhsValId = ValOperandId(writer.setInputOperandId(2));
  }

  RootedId id(cx_);
  bool nameOrSymbol;
  if (!ValueToNameOrSymbolId(cx_, idVal_, &id, &nameOrSymbol)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  if (!lhsVal_.isObject() || !nameOrSymbol) {
    return AttachDecision::NoAction;
  }

  JSObject* obj = &lhsVal_.toObject();

  PropertyResult prop;
  if (!LookupOwnPropertyPure(cx_, obj, id, &prop)) {
    return AttachDecision::NoAction;
  }
  if (prop.isNotFound()) {
    return AttachDecision::NoAction;
  }

  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }
  auto* nobj = &obj->as<NativeObject>();

  PropertyInfo propInfo = prop.propertyInfo();
  NativeObject* holder = nobj;

  // The property must be the last added property of the object.
  Shape* newShape = holder->shape();
  MOZ_RELEASE_ASSERT(newShape->lastProperty() == propInfo);

#ifdef DEBUG
  // Verify exactly one property was added by comparing the property map
  // lengths.
  if (oldShape->propMapLength() == PropMap::Capacity) {
    MOZ_ASSERT(newShape->propMapLength() == 1);
  } else {
    MOZ_ASSERT(newShape->propMapLength() == oldShape->propMapLength() + 1);
  }
#endif

  // Basic shape checks.
  if (newShape->isDictionary() || !propInfo.isDataProperty() ||
      !propInfo.writable()) {
    return AttachDecision::NoAction;
  }

  ObjOperandId objId = writer.guardToObject(objValId);
  maybeEmitIdGuard(id);

  // Shape guard the object.
  writer.guardShape(objId, oldShape);

  // If this is the special function.prototype case, we need to guard the
  // function is a non-builtin constructor. See canAttachAddSlotStub.
  if (nobj->is<JSFunction>() && id.isAtom(cx_->names().prototype)) {
    MOZ_ASSERT(nobj->as<JSFunction>().isNonBuiltinConstructor());
    writer.guardFunctionIsNonBuiltinCtor(objId);
  }

  ShapeGuardProtoChain(writer, nobj, objId);

  if (holder->isFixedSlot(propInfo.slot())) {
    size_t offset = NativeObject::getFixedSlotOffset(propInfo.slot());
    writer.addAndStoreFixedSlot(objId, offset, rhsValId, newShape);
    trackAttached("AddSlot");
  } else {
    size_t offset = holder->dynamicSlotIndex(propInfo.slot()) * sizeof(Value);
    uint32_t numOldSlots = NativeObject::calculateDynamicSlots(oldShape);
    uint32_t numNewSlots = holder->numDynamicSlots();
    if (numOldSlots == numNewSlots) {
      writer.addAndStoreDynamicSlot(objId, offset, rhsValId, newShape);
      trackAttached("AddSlot");
    } else {
      MOZ_ASSERT(numNewSlots > numOldSlots);
      writer.allocateAndStoreDynamicSlot(objId, offset, rhsValId, newShape,
                                         numNewSlots);
      trackAttached("AllocateSlot");
    }
  }
  writer.returnFromIC();

  return AttachDecision::Attach;
}

InstanceOfIRGenerator::InstanceOfIRGenerator(JSContext* cx, HandleScript script,
                                             jsbytecode* pc, ICState state,
                                             HandleValue lhs, HandleObject rhs)
    : IRGenerator(cx, script, pc, CacheKind::InstanceOf, state),
      lhsVal_(lhs),
      rhsObj_(rhs) {}

AttachDecision InstanceOfIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::InstanceOf);
  AutoAssertNoPendingException aanpe(cx_);

  // Ensure RHS is a function -- could be a Proxy, which the IC isn't prepared
  // to handle.
  if (!rhsObj_->is<JSFunction>()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  HandleFunction fun = rhsObj_.as<JSFunction>();

  if (fun->isBoundFunction()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  // Look up the @@hasInstance property, and check that Function.__proto__ is
  // the property holder, and that no object further down the prototype chain
  // (including this function) has shadowed it; together with the fact that
  // Function.__proto__[@@hasInstance] is immutable, this ensures that the
  // hasInstance hook will not change without the need to guard on the actual
  // property value.
  PropertyResult hasInstanceProp;
  NativeObject* hasInstanceHolder = nullptr;
  jsid hasInstanceID = SYMBOL_TO_JSID(cx_->wellKnownSymbols().hasInstance);
  if (!LookupPropertyPure(cx_, fun, hasInstanceID, &hasInstanceHolder,
                          &hasInstanceProp) ||
      !hasInstanceProp.isNativeProperty()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  Value funProto = cx_->global()->getPrototype(JSProto_Function);
  if (hasInstanceHolder != &funProto.toObject()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  // If the above succeeded, then these should be true about @@hasInstance,
  // because the property on Function.__proto__ is an immutable data property:
  MOZ_ASSERT(hasInstanceProp.propertyInfo().isDataProperty());
  MOZ_ASSERT(!hasInstanceProp.propertyInfo().configurable());
  MOZ_ASSERT(!hasInstanceProp.propertyInfo().writable());

  MOZ_ASSERT(IsCacheableProtoChain(fun, hasInstanceHolder));

  // Ensure that the function's prototype slot is the same.
  Maybe<PropertyInfo> prop = fun->lookupPure(cx_->names().prototype);
  if (prop.isNothing() || !prop->isDataProperty()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  uint32_t slot = prop->slot();

  MOZ_ASSERT(fun->numFixedSlots() == 0, "Stub code relies on this");
  if (!fun->getSlot(slot).isObject()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  JSObject* prototypeObject = &fun->getSlot(slot).toObject();

  // Abstract Objects
  ValOperandId lhs(writer.setInputOperandId(0));
  ValOperandId rhs(writer.setInputOperandId(1));

  ObjOperandId rhsId = writer.guardToObject(rhs);
  writer.guardShape(rhsId, fun->shape());

  // Ensure that the shapes up the prototype chain for the RHS remain the same
  // so that @@hasInstance is not shadowed by some intermediate prototype
  // object.
  if (hasInstanceHolder != fun) {
    GeneratePrototypeGuards(writer, fun, hasInstanceHolder, rhsId);
    ObjOperandId holderId = writer.loadObject(hasInstanceHolder);
    TestMatchingHolder(writer, hasInstanceHolder, holderId);
  }

  // Load prototypeObject into the cache -- consumed twice in the IC
  ObjOperandId protoId = writer.loadObject(prototypeObject);
  // Ensure that rhs[slot] == prototypeObject.
  writer.guardDynamicSlotIsSpecificObject(rhsId, protoId, slot);

  // Needn't guard LHS is object, because the actual stub can handle that
  // and correctly return false.
  writer.loadInstanceOfObjectResult(lhs, protoId);
  writer.returnFromIC();
  trackAttached("InstanceOf");
  return AttachDecision::Attach;
}

void InstanceOfIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("lhs", lhsVal_);
    sp.valueProperty("rhs", ObjectValue(*rhsObj_));
  }
#else
  // Silence Clang -Wunused-private-field warning.
  (void)lhsVal_;
#endif
}

TypeOfIRGenerator::TypeOfIRGenerator(JSContext* cx, HandleScript script,
                                     jsbytecode* pc, ICState state,
                                     HandleValue value)
    : IRGenerator(cx, script, pc, CacheKind::TypeOf, state), val_(value) {}

void TypeOfIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

AttachDecision TypeOfIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::TypeOf);

  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId valId(writer.setInputOperandId(0));

  TRY_ATTACH(tryAttachPrimitive(valId));
  TRY_ATTACH(tryAttachObject(valId));

  MOZ_ASSERT_UNREACHABLE("Failed to attach TypeOf");
  return AttachDecision::NoAction;
}

AttachDecision TypeOfIRGenerator::tryAttachPrimitive(ValOperandId valId) {
  if (!val_.isPrimitive()) {
    return AttachDecision::NoAction;
  }

  // Note: we don't use GuardIsNumber for int32 values because it's less
  // efficient in Warp (unboxing to double instead of int32).
  if (val_.isDouble()) {
    writer.guardIsNumber(valId);
  } else {
    writer.guardNonDoubleType(valId, val_.type());
  }

  writer.loadConstantStringResult(
      TypeName(js::TypeOfValue(val_), cx_->names()));
  writer.returnFromIC();
  writer.setTypeData(TypeData(JSValueType(val_.type())));
  trackAttached("Primitive");
  return AttachDecision::Attach;
}

AttachDecision TypeOfIRGenerator::tryAttachObject(ValOperandId valId) {
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }

  ObjOperandId objId = writer.guardToObject(valId);
  writer.loadTypeOfObjectResult(objId);
  writer.returnFromIC();
  writer.setTypeData(TypeData(JSValueType(val_.type())));
  trackAttached("Object");
  return AttachDecision::Attach;
}

GetIteratorIRGenerator::GetIteratorIRGenerator(JSContext* cx,
                                               HandleScript script,
                                               jsbytecode* pc, ICState state,
                                               HandleValue value)
    : IRGenerator(cx, script, pc, CacheKind::GetIterator, state), val_(value) {}

AttachDecision GetIteratorIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::GetIterator);

  AutoAssertNoPendingException aanpe(cx_);

  if (mode_ == ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }

  RootedObject obj(cx_, &val_.toObject());

  ObjOperandId objId = writer.guardToObject(valId);
  TRY_ATTACH(tryAttachNativeIterator(objId, obj));

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision GetIteratorIRGenerator::tryAttachNativeIterator(
    ObjOperandId objId, HandleObject obj) {
  MOZ_ASSERT(JSOp(*pc_) == JSOp::Iter);

  PropertyIteratorObject* iterobj = LookupInIteratorCache(cx_, obj);
  if (!iterobj) {
    return AttachDecision::NoAction;
  }
  auto* nobj = &obj->as<NativeObject>();

  // Guard on the receiver's shape.
  TestMatchingNativeReceiver(writer, nobj, objId);

  // Ensure the receiver has no dense elements.
  writer.guardNoDenseElements(objId);

  // Do the same for the objects on the proto chain.
  GeneratePrototypeHoleGuards(writer, nobj, objId,
                              /* alwaysGuardFirstProto = */ false);

  ObjOperandId iterId = writer.guardAndGetIterator(
      objId, iterobj, &ObjectRealm::get(obj).enumerators);
  writer.loadObjectResult(iterId);
  writer.returnFromIC();

  trackAttached("GetIterator");
  return AttachDecision::Attach;
}

void GetIteratorIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

OptimizeSpreadCallIRGenerator::OptimizeSpreadCallIRGenerator(
    JSContext* cx, HandleScript script, jsbytecode* pc, ICState state,
    HandleValue value)
    : IRGenerator(cx, script, pc, CacheKind::OptimizeSpreadCall, state),
      val_(value) {}

AttachDecision OptimizeSpreadCallIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::OptimizeSpreadCall);

  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachArray());
  TRY_ATTACH(tryAttachNotOptimizable());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

static bool IsArrayPrototypeOptimizable(JSContext* cx, ArrayObject* arr,
                                        NativeObject** arrProto, uint32_t* slot,
                                        JSFunction** iterFun) {
  // Prototype must be Array.prototype.
  auto* proto = cx->global()->maybeGetArrayPrototype();
  if (!proto || arr->staticPrototype() != proto) {
    return false;
  }
  *arrProto = proto;

  // The object must not have an own @@iterator property.
  PropertyKey iteratorKey = SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator);
  if (arr->lookupPure(iteratorKey)) {
    return false;
  }

  // Ensure that Array.prototype's @@iterator slot is unchanged.
  Maybe<PropertyInfo> prop = proto->lookupPure(iteratorKey);
  if (prop.isNothing() || !prop->isDataProperty()) {
    return false;
  }

  *slot = prop->slot();
  MOZ_ASSERT(proto->numFixedSlots() == 0, "Stub code relies on this");

  const Value& iterVal = proto->getSlot(*slot);
  if (!iterVal.isObject() || !iterVal.toObject().is<JSFunction>()) {
    return false;
  }

  *iterFun = &iterVal.toObject().as<JSFunction>();
  return IsSelfHostedFunctionWithName(*iterFun, cx->names().ArrayValues);
}

static bool IsArrayIteratorPrototypeOptimizable(JSContext* cx,
                                                NativeObject** arrIterProto,
                                                uint32_t* slot,
                                                JSFunction** nextFun) {
  auto* proto = cx->global()->maybeGetArrayIteratorPrototype();
  if (!proto) {
    return false;
  }
  *arrIterProto = proto;

  // Ensure that %ArrayIteratorPrototype%'s "next" slot is unchanged.
  Maybe<PropertyInfo> prop = proto->lookupPure(cx->names().next);
  if (prop.isNothing() || !prop->isDataProperty()) {
    return false;
  }

  *slot = prop->slot();
  MOZ_ASSERT(proto->numFixedSlots() == 0, "Stub code relies on this");

  const Value& nextVal = proto->getSlot(*slot);
  if (!nextVal.isObject() || !nextVal.toObject().is<JSFunction>()) {
    return false;
  }

  *nextFun = &nextVal.toObject().as<JSFunction>();
  return IsSelfHostedFunctionWithName(*nextFun, cx->names().ArrayIteratorNext);
}

AttachDecision OptimizeSpreadCallIRGenerator::tryAttachArray() {
  if (!isFirstStub_) {
    return AttachDecision::NoAction;
  }

  // The value must be a packed array.
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }
  JSObject* obj = &val_.toObject();
  if (!IsPackedArray(obj)) {
    return AttachDecision::NoAction;
  }

  // Prototype must be Array.prototype and Array.prototype[@@iterator] must not
  // be modified.
  NativeObject* arrProto;
  uint32_t arrProtoIterSlot;
  JSFunction* iterFun;
  if (!IsArrayPrototypeOptimizable(cx_, &obj->as<ArrayObject>(), &arrProto,
                                   &arrProtoIterSlot, &iterFun)) {
    return AttachDecision::NoAction;
  }

  // %ArrayIteratorPrototype%.next must not be modified.
  NativeObject* arrayIteratorProto;
  uint32_t iterNextSlot;
  JSFunction* nextFun;
  if (!IsArrayIteratorPrototypeOptimizable(cx_, &arrayIteratorProto,
                                           &iterNextSlot, &nextFun)) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  ObjOperandId objId = writer.guardToObject(valId);

  // Guard the object is a packed array with Array.prototype as proto.
  MOZ_ASSERT(obj->is<ArrayObject>());
  writer.guardShape(objId, obj->shape());
  writer.guardArrayIsPacked(objId);

  // Guard on Array.prototype[@@iterator].
  ObjOperandId arrProtoId = writer.loadObject(arrProto);
  ObjOperandId iterId = writer.loadObject(iterFun);
  writer.guardShape(arrProtoId, arrProto->shape());
  writer.guardDynamicSlotIsSpecificObject(arrProtoId, iterId, arrProtoIterSlot);

  // Guard on %ArrayIteratorPrototype%.next.
  ObjOperandId iterProtoId = writer.loadObject(arrayIteratorProto);
  ObjOperandId nextId = writer.loadObject(nextFun);
  writer.guardShape(iterProtoId, arrayIteratorProto->shape());
  writer.guardDynamicSlotIsSpecificObject(iterProtoId, nextId, iterNextSlot);

  writer.loadBooleanResult(true);
  writer.returnFromIC();

  trackAttached("Array");
  return AttachDecision::Attach;
}

AttachDecision OptimizeSpreadCallIRGenerator::tryAttachNotOptimizable() {
  ValOperandId valId(writer.setInputOperandId(0));

  writer.loadBooleanResult(false);
  writer.returnFromIC();

  trackAttached("NotOptimizable");
  return AttachDecision::Attach;
}

void OptimizeSpreadCallIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

CallIRGenerator::CallIRGenerator(JSContext* cx, HandleScript script,
                                 jsbytecode* pc, JSOp op, ICState state,
                                 uint32_t argc, HandleValue callee,
                                 HandleValue thisval, HandleValue newTarget,
                                 HandleValueArray args)
    : IRGenerator(cx, script, pc, CacheKind::Call, state),
      op_(op),
      argc_(argc),
      callee_(callee),
      thisval_(thisval),
      newTarget_(newTarget),
      args_(args) {}

void CallIRGenerator::emitNativeCalleeGuard(JSFunction* callee) {
  // Note: we rely on GuardSpecificFunction to also guard against the same
  // native from a different realm.
  MOZ_ASSERT(callee->isNativeWithoutJitEntry());

  bool isConstructing = IsConstructPC(pc_);
  CallFlags flags(isConstructing, IsSpreadPC(pc_));

  ValOperandId calleeValId =
      writer.loadArgumentFixedSlot(ArgumentKind::Callee, argc_, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);
  writer.guardSpecificFunction(calleeObjId, callee);

  // If we're constructing we also need to guard newTarget == callee.
  if (isConstructing) {
    MOZ_ASSERT(&newTarget_.toObject() == callee);
    ValOperandId newTargetValId =
        writer.loadArgumentFixedSlot(ArgumentKind::NewTarget, argc_, flags);
    ObjOperandId newTargetObjId = writer.guardToObject(newTargetValId);
    writer.guardSpecificFunction(newTargetObjId, callee);
  }
}

void CallIRGenerator::emitCalleeGuard(ObjOperandId calleeId,
                                      JSFunction* callee) {
  // Guarding on the callee JSFunction* is most efficient, but doesn't work well
  // for lambda clones (multiple functions with the same BaseScript). We guard
  // on the function's BaseScript if the callee is scripted and this isn't the
  // first IC stub.
  if (isFirstStub_ || !callee->hasBaseScript() ||
      callee->isSelfHostedBuiltin()) {
    writer.guardSpecificFunction(calleeId, callee);
  } else {
    writer.guardClass(calleeId, GuardClassKind::JSFunction);
    writer.guardFunctionScript(calleeId, callee->baseScript());
  }
}

AttachDecision CallIRGenerator::tryAttachArrayPush(HandleFunction callee) {
  // Only optimize on obj.push(val);
  if (argc_ != 1 || !thisval_.isObject()) {
    return AttachDecision::NoAction;
  }

  // Where |obj| is a native array.
  JSObject* thisobj = &thisval_.toObject();
  if (!thisobj->is<ArrayObject>()) {
    return AttachDecision::NoAction;
  }

  auto* thisarray = &thisobj->as<ArrayObject>();

  // Check for other indexed properties or class hooks.
  if (!CanAttachAddElement(thisarray, /* isInit = */ false)) {
    return AttachDecision::NoAction;
  }

  // Can't add new elements to arrays with non-writable length.
  if (!thisarray->lengthIsWritable()) {
    return AttachDecision::NoAction;
  }

  // Check that array is extensible.
  if (!thisarray->isExtensible()) {
    return AttachDecision::NoAction;
  }

  // Check that the array is completely initialized (no holes).
  if (thisarray->getDenseInitializedLength() != thisarray->length()) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(!thisarray->denseElementsAreFrozen(),
             "Extensible arrays should not have frozen elements");

  // After this point, we can generate code fine.

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'push' native function.
  emitNativeCalleeGuard(callee);

  // Guard this is an array object.
  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);

  // Guard that the shape matches.
  TestMatchingNativeReceiver(writer, thisarray, thisObjId);

  // Guard proto chain shapes.
  ShapeGuardProtoChain(writer, thisarray, thisObjId);

  // arr.push(x) is equivalent to arr[arr.length] = x for regular arrays.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  writer.arrayPush(thisObjId, argId);

  writer.returnFromIC();

  trackAttached("ArrayPush");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachArrayPopShift(HandleFunction callee,
                                                       InlinableNative native) {
  // Expecting no arguments.
  if (argc_ != 0) {
    return AttachDecision::NoAction;
  }

  // Only optimize if |this| is a packed array.
  if (!thisval_.isObject() || !IsPackedArray(&thisval_.toObject())) {
    return AttachDecision::NoAction;
  }

  // Other conditions:
  //
  // * The array length needs to be writable because we're changing it.
  // * The array must be extensible. Non-extensible arrays require preserving
  //   the |initializedLength == capacity| invariant on ObjectElements.
  //   See NativeObject::shrinkCapacityToInitializedLength.
  //   This also ensures the elements aren't sealed/frozen.
  // * There must not be a for-in iterator for the elements because the IC stub
  //   does not suppress deleted properties.
  ArrayObject* arr = &thisval_.toObject().as<ArrayObject>();
  if (!arr->lengthIsWritable() || !arr->isExtensible() ||
      arr->denseElementsHaveMaybeInIterationFlag()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'pop' or 'shift' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  ObjOperandId objId = writer.guardToObject(thisValId);
  writer.guardClass(objId, GuardClassKind::Array);

  if (native == InlinableNative::ArrayPop) {
    writer.packedArrayPopResult(objId);
  } else {
    MOZ_ASSERT(native == InlinableNative::ArrayShift);
    writer.packedArrayShiftResult(objId);
  }

  writer.returnFromIC();

  trackAttached("ArrayPopShift");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachArrayJoin(HandleFunction callee) {
  // Only handle argc <= 1.
  if (argc_ > 1) {
    return AttachDecision::NoAction;
  }

  // Only optimize if |this| is an array.
  if (!thisval_.isObject() || !thisval_.toObject().is<ArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // The separator argument must be a string, if present.
  if (argc_ > 0 && !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  // IC stub code can handle non-packed array.

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'join' native function.
  emitNativeCalleeGuard(callee);

  // Guard this is an array object.
  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);
  writer.guardClass(thisObjId, GuardClassKind::Array);

  StringOperandId sepId;
  if (argc_ == 1) {
    // If argcount is 1, guard that the argument is a string.
    ValOperandId argValId =
        writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
    sepId = writer.guardToString(argValId);
  } else {
    sepId = writer.loadConstantString(cx_->names().comma);
  }

  // Do the join.
  writer.arrayJoinResult(thisObjId, sepId);

  writer.returnFromIC();

  trackAttached("ArrayJoin");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachArraySlice(HandleFunction callee) {
  // Only handle argc <= 2.
  if (argc_ > 2) {
    return AttachDecision::NoAction;
  }

  // Only optimize if |this| is a packed array.
  if (!thisval_.isObject() || !IsPackedArray(&thisval_.toObject())) {
    return AttachDecision::NoAction;
  }

  // Arguments for the sliced region must be integers.
  if (argc_ > 0 && !args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }
  if (argc_ > 1 && !args_[1].isInt32()) {
    return AttachDecision::NoAction;
  }

  RootedArrayObject arr(cx_, &thisval_.toObject().as<ArrayObject>());

  JSObject* templateObj =
      NewDenseFullyAllocatedArray(cx_, 0, /* proto = */ nullptr, TenuredObject);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'slice' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  ObjOperandId objId = writer.guardToObject(thisValId);
  writer.guardClass(objId, GuardClassKind::Array);

  Int32OperandId int32BeginId;
  if (argc_ > 0) {
    ValOperandId beginId =
        writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
    int32BeginId = writer.guardToInt32(beginId);
  } else {
    int32BeginId = writer.loadInt32Constant(0);
  }

  Int32OperandId int32EndId;
  if (argc_ > 1) {
    ValOperandId endId =
        writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
    int32EndId = writer.guardToInt32(endId);
  } else {
    int32EndId = writer.loadInt32ArrayLength(objId);
  }

  writer.packedArraySliceResult(templateObj, objId, int32BeginId, int32EndId);
  writer.returnFromIC();

  trackAttached("ArraySlice");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachArrayIsArray(HandleFunction callee) {
  // Need a single argument.
  if (argc_ != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'isArray' native function.
  emitNativeCalleeGuard(callee);

  // Check if the argument is an Array and return result.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  writer.isArrayResult(argId);
  writer.returnFromIC();

  trackAttached("ArrayIsArray");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachDataViewGet(HandleFunction callee,
                                                     Scalar::Type type) {
  // Ensure |this| is a DataViewObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<DataViewObject>()) {
    return AttachDecision::NoAction;
  }

  // Expected arguments: offset (number), optional littleEndian (boolean).
  if (argc_ < 1 || argc_ > 2) {
    return AttachDecision::NoAction;
  }
  int64_t offsetInt64;
  if (!ValueIsInt64Index(args_[0], &offsetInt64)) {
    return AttachDecision::NoAction;
  }
  if (argc_ > 1 && !args_[1].isBoolean()) {
    return AttachDecision::NoAction;
  }

  DataViewObject* dv = &thisval_.toObject().as<DataViewObject>();

  // Bounds check the offset.
  if (offsetInt64 < 0 ||
      !dv->offsetIsInBounds(Scalar::byteSize(type), offsetInt64)) {
    return AttachDecision::NoAction;
  }

  // For getUint32 we let the stub return an Int32 if we have not seen a
  // double, to allow better codegen in Warp while avoiding bailout loops.
  bool forceDoubleForUint32 = false;
  if (type == Scalar::Uint32) {
    bool isLittleEndian = argc_ > 1 && args_[1].toBoolean();
    uint32_t res = dv->read<uint32_t>(offsetInt64, isLittleEndian);
    forceDoubleForUint32 = res >= INT32_MAX;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is this DataView native function.
  emitNativeCalleeGuard(callee);

  // Guard |this| is a DataViewObject.
  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  ObjOperandId objId = writer.guardToObject(thisValId);
  writer.guardClass(objId, GuardClassKind::DataView);

  // Convert offset to intPtr.
  ValOperandId offsetId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  IntPtrOperandId intPtrOffsetId =
      guardToIntPtrIndex(args_[0], offsetId, /* supportOOB = */ false);

  BooleanOperandId boolLittleEndianId;
  if (argc_ > 1) {
    ValOperandId littleEndianId =
        writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
    boolLittleEndianId = writer.guardToBoolean(littleEndianId);
  } else {
    boolLittleEndianId = writer.loadBooleanConstant(false);
  }

  writer.loadDataViewValueResult(objId, intPtrOffsetId, boolLittleEndianId,
                                 type, forceDoubleForUint32);
  writer.returnFromIC();

  trackAttached("DataViewGet");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachDataViewSet(HandleFunction callee,
                                                     Scalar::Type type) {
  // Ensure |this| is a DataViewObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<DataViewObject>()) {
    return AttachDecision::NoAction;
  }

  // Expected arguments: offset (number), value, optional littleEndian (boolean)
  if (argc_ < 2 || argc_ > 3) {
    return AttachDecision::NoAction;
  }
  int64_t offsetInt64;
  if (!ValueIsInt64Index(args_[0], &offsetInt64)) {
    return AttachDecision::NoAction;
  }
  if (!ValueIsNumeric(type, args_[1])) {
    return AttachDecision::NoAction;
  }
  if (argc_ > 2 && !args_[2].isBoolean()) {
    return AttachDecision::NoAction;
  }

  DataViewObject* dv = &thisval_.toObject().as<DataViewObject>();

  // Bounds check the offset.
  if (offsetInt64 < 0 ||
      !dv->offsetIsInBounds(Scalar::byteSize(type), offsetInt64)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is this DataView native function.
  emitNativeCalleeGuard(callee);

  // Guard |this| is a DataViewObject.
  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  ObjOperandId objId = writer.guardToObject(thisValId);
  writer.guardClass(objId, GuardClassKind::DataView);

  // Convert offset to intPtr.
  ValOperandId offsetId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  IntPtrOperandId intPtrOffsetId =
      guardToIntPtrIndex(args_[0], offsetId, /* supportOOB = */ false);

  // Convert value to number or BigInt.
  ValOperandId valueId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  OperandId numericValueId = emitNumericGuard(valueId, type);

  BooleanOperandId boolLittleEndianId;
  if (argc_ > 2) {
    ValOperandId littleEndianId =
        writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_);
    boolLittleEndianId = writer.guardToBoolean(littleEndianId);
  } else {
    boolLittleEndianId = writer.loadBooleanConstant(false);
  }

  writer.storeDataViewValueResult(objId, intPtrOffsetId, numericValueId,
                                  boolLittleEndianId, type);
  writer.returnFromIC();

  trackAttached("DataViewSet");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachUnsafeGetReservedSlot(
    HandleFunction callee, InlinableNative native) {
  // Self-hosted code calls this with (object, int32) arguments.
  MOZ_ASSERT(argc_ == 2);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isInt32());
  MOZ_ASSERT(args_[1].toInt32() >= 0);

  uint32_t slot = uint32_t(args_[1].toInt32());
  if (slot >= NativeObject::MAX_FIXED_SLOTS) {
    return AttachDecision::NoAction;
  }
  size_t offset = NativeObject::getFixedSlotOffset(slot);

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the first argument is an object.
  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(arg0Id);

  // BytecodeEmitter::checkSelfHostedUnsafeGetReservedSlot ensures that the
  // slot argument is constant. (At least for direct calls)

  switch (native) {
    case InlinableNative::IntrinsicUnsafeGetReservedSlot:
      writer.loadFixedSlotResult(objId, offset);
      break;
    case InlinableNative::IntrinsicUnsafeGetObjectFromReservedSlot:
      writer.loadFixedSlotTypedResult(objId, offset, ValueType::Object);
      break;
    case InlinableNative::IntrinsicUnsafeGetInt32FromReservedSlot:
      writer.loadFixedSlotTypedResult(objId, offset, ValueType::Int32);
      break;
    case InlinableNative::IntrinsicUnsafeGetStringFromReservedSlot:
      writer.loadFixedSlotTypedResult(objId, offset, ValueType::String);
      break;
    case InlinableNative::IntrinsicUnsafeGetBooleanFromReservedSlot:
      writer.loadFixedSlotTypedResult(objId, offset, ValueType::Boolean);
      break;
    default:
      MOZ_CRASH("unexpected native");
  }

  writer.returnFromIC();

  trackAttached("UnsafeGetReservedSlot");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachUnsafeSetReservedSlot(
    HandleFunction callee) {
  // Self-hosted code calls this with (object, int32, value) arguments.
  MOZ_ASSERT(argc_ == 3);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isInt32());
  MOZ_ASSERT(args_[1].toInt32() >= 0);

  uint32_t slot = uint32_t(args_[1].toInt32());
  if (slot >= NativeObject::MAX_FIXED_SLOTS) {
    return AttachDecision::NoAction;
  }
  size_t offset = NativeObject::getFixedSlotOffset(slot);

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the first argument is an object.
  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(arg0Id);

  // BytecodeEmitter::checkSelfHostedUnsafeSetReservedSlot ensures that the
  // slot argument is constant. (At least for direct calls)

  // Get the value to set.
  ValOperandId valId = writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_);

  // Set the fixed slot and return undefined.
  writer.storeFixedSlotUndefinedResult(objId, offset, valId);

  // This stub always returns undefined.
  writer.returnFromIC();

  trackAttached("UnsafeSetReservedSlot");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachIsSuspendedGenerator(
    HandleFunction callee) {
  // The IsSuspendedGenerator intrinsic is only called in
  // self-hosted code, so it's safe to assume we have a single
  // argument and the callee is our intrinsic.

  MOZ_ASSERT(argc_ == 1);

  Int32OperandId argcId(writer.setInputOperandId(0));

  // Stack layout here is (bottom to top):
  //  2: Callee
  //  1: ThisValue
  //  0: Arg <-- Top of stack.
  // We only care about the argument.
  ValOperandId valId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  // Check whether the argument is a suspended generator.
  // We don't need guards, because IsSuspendedGenerator returns
  // false for values that are not generator objects.
  writer.callIsSuspendedGeneratorResult(valId);
  writer.returnFromIC();

  trackAttached("IsSuspendedGenerator");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachToObject(HandleFunction callee,
                                                  InlinableNative native) {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT_IF(native == InlinableNative::IntrinsicToObject, argc_ == 1);

  // Need a single object argument.
  // TODO(Warp): Support all or more conversions to object.
  // Note: ToObject and Object differ in their behavior for undefined/null.
  if (argc_ != 1 || !args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'Object' function.
  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.
  if (native == InlinableNative::Object) {
    emitNativeCalleeGuard(callee);
  }

  // Guard that the argument is an object.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(argId);

  // Return the object.
  writer.loadObjectResult(objId);
  writer.returnFromIC();

  if (native == InlinableNative::IntrinsicToObject) {
    trackAttached("ToObject");
  } else {
    MOZ_ASSERT(native == InlinableNative::Object);
    trackAttached("Object");
  }
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachToInteger(HandleFunction callee) {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(argc_ == 1);

  // Need a single int32 argument.
  // TODO(Warp): Support all or more conversions to integer.
  // Make sure to update this code correctly if we ever start
  // returning non-int32 integers.
  if (!args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an int32.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  Int32OperandId int32Id = writer.guardToInt32(argId);

  // Return the int32.
  writer.loadInt32Result(int32Id);
  writer.returnFromIC();

  trackAttached("ToInteger");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachToLength(HandleFunction callee) {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(argc_ == 1);

  // Need a single int32 argument.
  if (!args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // ToLength(int32) is equivalent to max(int32, 0).
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  Int32OperandId int32ArgId = writer.guardToInt32(argId);
  Int32OperandId zeroId = writer.loadInt32Constant(0);
  bool isMax = true;
  Int32OperandId maxId = writer.int32MinMax(isMax, int32ArgId, zeroId);
  writer.loadInt32Result(maxId);
  writer.returnFromIC();

  trackAttached("ToLength");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachIsObject(HandleFunction callee) {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(argc_ == 1);

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Type check the argument and return result.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  writer.isObjectResult(argId);
  writer.returnFromIC();

  trackAttached("IsObject");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachIsPackedArray(HandleFunction callee) {
  // Self-hosted code calls this with a single object argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Check if the argument is packed and return result.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objArgId = writer.guardToObject(argId);
  writer.isPackedArrayResult(objArgId);
  writer.returnFromIC();

  trackAttached("IsPackedArray");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachIsCallable(HandleFunction callee) {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(argc_ == 1);

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Check if the argument is callable and return result.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  writer.isCallableResult(argId);
  writer.returnFromIC();

  trackAttached("IsCallable");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachIsConstructor(HandleFunction callee) {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(argc_ == 1);

  // Need a single object argument.
  if (!args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an object.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(argId);

  // Check if the argument is a constructor and return result.
  writer.isConstructorResult(objId);
  writer.returnFromIC();

  trackAttached("IsConstructor");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachIsCrossRealmArrayConstructor(
    HandleFunction callee) {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());

  if (args_[0].toObject().is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(argId);
  writer.guardIsNotProxy(objId);
  writer.isCrossRealmArrayConstructorResult(objId);
  writer.returnFromIC();

  trackAttached("IsCrossRealmArrayConstructor");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachGuardToClass(HandleFunction callee,
                                                      InlinableNative native) {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Class must match.
  const JSClass* clasp = InlinableNativeGuardToClass(native);
  if (args_[0].toObject().getClass() != clasp) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an object.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(argId);

  // Guard that the object has the correct class.
  writer.guardAnyClass(objId, clasp);

  // Return the object.
  writer.loadObjectResult(objId);
  writer.returnFromIC();

  trackAttached("GuardToClass");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachHasClass(HandleFunction callee,
                                                  const JSClass* clasp,
                                                  bool isPossiblyWrapped) {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Only optimize when the object isn't a proxy.
  if (isPossiblyWrapped && args_[0].toObject().is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Perform the Class check.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(argId);

  if (isPossiblyWrapped) {
    writer.guardIsNotProxy(objId);
  }

  writer.hasClassResult(objId, clasp);
  writer.returnFromIC();

  trackAttached("HasClass");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachRegExpMatcherSearcherTester(
    HandleFunction callee, InlinableNative native) {
  // Self-hosted code calls this with (object, string, number) arguments.
  MOZ_ASSERT(argc_ == 3);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isString());
  MOZ_ASSERT(args_[2].isNumber());

  // It's not guaranteed that the JITs have typed |lastIndex| as an Int32.
  if (!args_[2].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard argument types.
  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId reId = writer.guardToObject(arg0Id);

  ValOperandId arg1Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  StringOperandId inputId = writer.guardToString(arg1Id);

  ValOperandId arg2Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_);
  Int32OperandId lastIndexId = writer.guardToInt32(arg2Id);

  switch (native) {
    case InlinableNative::RegExpMatcher:
      writer.callRegExpMatcherResult(reId, inputId, lastIndexId);
      writer.returnFromIC();
      trackAttached("RegExpMatcher");
      break;

    case InlinableNative::RegExpSearcher:
      writer.callRegExpSearcherResult(reId, inputId, lastIndexId);
      writer.returnFromIC();
      trackAttached("RegExpSearcher");
      break;

    case InlinableNative::RegExpTester:
      writer.callRegExpTesterResult(reId, inputId, lastIndexId);
      writer.returnFromIC();
      trackAttached("RegExpTester");
      break;

    default:
      MOZ_CRASH("Unexpected native");
  }

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachRegExpPrototypeOptimizable(
    HandleFunction callee) {
  // Self-hosted code calls this with a single object argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId protoId = writer.guardToObject(arg0Id);

  writer.regExpPrototypeOptimizableResult(protoId);
  writer.returnFromIC();

  trackAttached("RegExpPrototypeOptimizable");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachRegExpInstanceOptimizable(
    HandleFunction callee) {
  // Self-hosted code calls this with two object arguments.
  MOZ_ASSERT(argc_ == 2);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isObject());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId regexpId = writer.guardToObject(arg0Id);

  ValOperandId arg1Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  ObjOperandId protoId = writer.guardToObject(arg1Id);

  writer.regExpInstanceOptimizableResult(regexpId, protoId);
  writer.returnFromIC();

  trackAttached("RegExpInstanceOptimizable");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachGetFirstDollarIndex(
    HandleFunction callee) {
  // Self-hosted code calls this with a single string argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isString());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  StringOperandId strId = writer.guardToString(arg0Id);

  writer.getFirstDollarIndexResult(strId);
  writer.returnFromIC();

  trackAttached("GetFirstDollarIndex");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachSubstringKernel(
    HandleFunction callee) {
  // Self-hosted code calls this with (string, int32, int32) arguments.
  MOZ_ASSERT(argc_ == 3);
  MOZ_ASSERT(args_[0].isString());
  MOZ_ASSERT(args_[1].isInt32());
  MOZ_ASSERT(args_[2].isInt32());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  StringOperandId strId = writer.guardToString(arg0Id);

  ValOperandId arg1Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  Int32OperandId beginId = writer.guardToInt32(arg1Id);

  ValOperandId arg2Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_);
  Int32OperandId lengthId = writer.guardToInt32(arg2Id);

  writer.callSubstringKernelResult(strId, beginId, lengthId);
  writer.returnFromIC();

  trackAttached("SubstringKernel");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachObjectHasPrototype(
    HandleFunction callee) {
  // Self-hosted code calls this with (object, object) arguments.
  MOZ_ASSERT(argc_ == 2);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isObject());

  auto* obj = &args_[0].toObject().as<NativeObject>();
  auto* proto = &args_[1].toObject().as<NativeObject>();

  // Only attach when obj.__proto__ is proto.
  if (obj->staticPrototype() != proto) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(arg0Id);

  writer.guardProto(objId, proto);
  writer.loadBooleanResult(true);
  writer.returnFromIC();

  trackAttached("ObjectHasPrototype");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachString(HandleFunction callee) {
  // Need a single string argument.
  // TODO(Warp): Support all or more conversions to strings.
  if (argc_ != 1 || !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'String' function.
  emitNativeCalleeGuard(callee);

  // Guard that the argument is a string.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  StringOperandId strId = writer.guardToString(argId);

  // Return the string.
  writer.loadStringResult(strId);
  writer.returnFromIC();

  trackAttached("String");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStringConstructor(
    HandleFunction callee) {
  // Need a single string argument.
  // TODO(Warp): Support all or more conversions to strings.
  if (argc_ != 1 || !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  RootedString emptyString(cx_, cx_->runtime()->emptyString);
  JSObject* templateObj = StringObject::create(
      cx_, emptyString, /* proto = */ nullptr, TenuredObject);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'String' function.
  emitNativeCalleeGuard(callee);

  CallFlags flags(IsConstructPC(pc_), IsSpreadPC(pc_));

  // Guard that the argument is a string.
  ValOperandId argId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_, flags);
  StringOperandId strId = writer.guardToString(argId);
  writer.newStringObjectResult(templateObj, strId);
  writer.returnFromIC();

  trackAttached("StringConstructor");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStringToStringValueOf(
    HandleFunction callee) {
  // Expecting no arguments.
  if (argc_ != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'toString' OR 'valueOf' native function.
  emitNativeCalleeGuard(callee);

  // Guard |this| is a string.
  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  StringOperandId strId = writer.guardToString(thisValId);

  // Return the string
  writer.loadStringResult(strId);
  writer.returnFromIC();

  trackAttached("StringToStringValueOf");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStringReplaceString(
    HandleFunction callee) {
  // Self-hosted code calls this with (string, string, string) arguments.
  MOZ_ASSERT(argc_ == 3);
  MOZ_ASSERT(args_[0].isString());
  MOZ_ASSERT(args_[1].isString());
  MOZ_ASSERT(args_[2].isString());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  StringOperandId strId = writer.guardToString(arg0Id);

  ValOperandId arg1Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  StringOperandId patternId = writer.guardToString(arg1Id);

  ValOperandId arg2Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_);
  StringOperandId replacementId = writer.guardToString(arg2Id);

  writer.stringReplaceStringResult(strId, patternId, replacementId);
  writer.returnFromIC();

  trackAttached("StringReplaceString");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStringSplitString(
    HandleFunction callee) {
  // Self-hosted code calls this with (string, string) arguments.
  MOZ_ASSERT(argc_ == 2);
  MOZ_ASSERT(args_[0].isString());
  MOZ_ASSERT(args_[1].isString());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  StringOperandId strId = writer.guardToString(arg0Id);

  ValOperandId arg1Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  StringOperandId separatorId = writer.guardToString(arg1Id);

  writer.stringSplitStringResult(strId, separatorId);
  writer.returnFromIC();

  trackAttached("StringSplitString");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStringChar(HandleFunction callee,
                                                    StringChar kind) {
  // Need one argument.
  if (argc_ != 1) {
    return AttachDecision::NoAction;
  }

  if (!CanAttachStringChar(thisval_, args_[0])) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'charCodeAt' or 'charAt' native function.
  emitNativeCalleeGuard(callee);

  // Guard this is a string.
  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  StringOperandId strId = writer.guardToString(thisValId);

  // Guard int32 index.
  ValOperandId indexId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  Int32OperandId int32IndexId = writer.guardToInt32Index(indexId);

  // Load string char or code.
  if (kind == StringChar::CodeAt) {
    writer.loadStringCharCodeResult(strId, int32IndexId);
  } else {
    writer.loadStringCharResult(strId, int32IndexId);
  }

  writer.returnFromIC();

  if (kind == StringChar::CodeAt) {
    trackAttached("StringCharCodeAt");
  } else {
    trackAttached("StringCharAt");
  }

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStringCharCodeAt(
    HandleFunction callee) {
  return tryAttachStringChar(callee, StringChar::CodeAt);
}

AttachDecision CallIRGenerator::tryAttachStringCharAt(HandleFunction callee) {
  return tryAttachStringChar(callee, StringChar::At);
}

AttachDecision CallIRGenerator::tryAttachStringFromCharCode(
    HandleFunction callee) {
  // Need one int32 argument.
  if (argc_ != 1 || !args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'fromCharCode' native function.
  emitNativeCalleeGuard(callee);

  // Guard int32 argument.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  Int32OperandId codeId = writer.guardToInt32(argId);

  // Return string created from code.
  writer.stringFromCharCodeResult(codeId);
  writer.returnFromIC();

  trackAttached("StringFromCharCode");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStringFromCodePoint(
    HandleFunction callee) {
  // Need one int32 argument.
  if (argc_ != 1 || !args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // String.fromCodePoint throws for invalid code points.
  int32_t codePoint = args_[0].toInt32();
  if (codePoint < 0 || codePoint > int32_t(unicode::NonBMPMax)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'fromCodePoint' native function.
  emitNativeCalleeGuard(callee);

  // Guard int32 argument.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  Int32OperandId codeId = writer.guardToInt32(argId);

  // Return string created from code point.
  writer.stringFromCodePointResult(codeId);
  writer.returnFromIC();

  trackAttached("StringFromCodePoint");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStringToLowerCase(
    HandleFunction callee) {
  // Expecting no arguments.
  if (argc_ != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'toLowerCase' native function.
  emitNativeCalleeGuard(callee);

  // Guard this is a string.
  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  StringOperandId strId = writer.guardToString(thisValId);

  // Return string converted to lower-case.
  writer.stringToLowerCaseResult(strId);
  writer.returnFromIC();

  trackAttached("StringToLowerCase");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStringToUpperCase(
    HandleFunction callee) {
  // Expecting no arguments.
  if (argc_ != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'toUpperCase' native function.
  emitNativeCalleeGuard(callee);

  // Guard this is a string.
  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);
  StringOperandId strId = writer.guardToString(thisValId);

  // Return string converted to upper-case.
  writer.stringToUpperCaseResult(strId);
  writer.returnFromIC();

  trackAttached("StringToUpperCase");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathRandom(HandleFunction callee) {
  // Expecting no arguments.
  if (argc_ != 0) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(cx_->realm() == callee->realm(),
             "Shouldn't inline cross-realm Math.random because per-realm RNG");

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'random' native function.
  emitNativeCalleeGuard(callee);

  mozilla::non_crypto::XorShift128PlusRNG* rng =
      &cx_->realm()->getOrCreateRandomNumberGenerator();
  writer.mathRandomResult(rng);

  writer.returnFromIC();

  trackAttached("MathRandom");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathAbs(HandleFunction callee) {
  // Need one argument.
  if (argc_ != 1) {
    return AttachDecision::NoAction;
  }

  if (!args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'abs' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argumentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  // abs(INT_MIN) is a double.
  if (args_[0].isInt32() && args_[0].toInt32() != INT_MIN) {
    Int32OperandId int32Id = writer.guardToInt32(argumentId);
    writer.mathAbsInt32Result(int32Id);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);
    writer.mathAbsNumberResult(numberId);
  }

  writer.returnFromIC();

  trackAttached("MathAbs");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathClz32(HandleFunction callee) {
  // Need one (number) argument.
  if (argc_ != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'clz32' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  Int32OperandId int32Id;
  if (args_[0].isInt32()) {
    int32Id = writer.guardToInt32(argId);
  } else {
    MOZ_ASSERT(args_[0].isDouble());
    NumberOperandId numId = writer.guardIsNumber(argId);
    int32Id = writer.truncateDoubleToUInt32(numId);
  }
  writer.mathClz32Result(int32Id);
  writer.returnFromIC();

  trackAttached("MathClz32");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathSign(HandleFunction callee) {
  // Need one (number) argument.
  if (argc_ != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'sign' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  if (args_[0].isInt32()) {
    Int32OperandId int32Id = writer.guardToInt32(argId);
    writer.mathSignInt32Result(int32Id);
  } else {
    // Math.sign returns a double only if the input is -0 or NaN so try to
    // optimize the common Number => Int32 case.
    double d = math_sign_impl(args_[0].toDouble());
    int32_t unused;
    bool resultIsInt32 = mozilla::NumberIsInt32(d, &unused);

    NumberOperandId numId = writer.guardIsNumber(argId);
    if (resultIsInt32) {
      writer.mathSignNumberToInt32Result(numId);
    } else {
      writer.mathSignNumberResult(numId);
    }
  }

  writer.returnFromIC();

  trackAttached("MathSign");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathImul(HandleFunction callee) {
  // Need two (number) arguments.
  if (argc_ != 2 || !args_[0].isNumber() || !args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'imul' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ValOperandId arg1Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);

  Int32OperandId int32Arg0Id, int32Arg1Id;
  if (args_[0].isInt32() && args_[1].isInt32()) {
    int32Arg0Id = writer.guardToInt32(arg0Id);
    int32Arg1Id = writer.guardToInt32(arg1Id);
  } else {
    // Treat both arguments as numbers if at least one of them is non-int32.
    NumberOperandId numArg0Id = writer.guardIsNumber(arg0Id);
    NumberOperandId numArg1Id = writer.guardIsNumber(arg1Id);
    int32Arg0Id = writer.truncateDoubleToUInt32(numArg0Id);
    int32Arg1Id = writer.truncateDoubleToUInt32(numArg1Id);
  }
  writer.mathImulResult(int32Arg0Id, int32Arg1Id);
  writer.returnFromIC();

  trackAttached("MathImul");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathFloor(HandleFunction callee) {
  // Need one (number) argument.
  if (argc_ != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Check if the result fits in int32.
  double res = math_floor_impl(args_[0].toNumber());
  int32_t unused;
  bool resultIsInt32 = mozilla::NumberIsInt32(res, &unused);

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'floor' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argumentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  if (args_[0].isInt32()) {
    MOZ_ASSERT(resultIsInt32);

    // Use an indirect truncation to inform the optimizer it needs to preserve
    // a bailout when the input can't be represented as an int32, even if the
    // final result is fully truncated.
    Int32OperandId intId = writer.guardToInt32(argumentId);
    writer.indirectTruncateInt32Result(intId);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);

    if (resultIsInt32) {
      writer.mathFloorToInt32Result(numberId);
    } else {
      writer.mathFloorNumberResult(numberId);
    }
  }

  writer.returnFromIC();

  trackAttached("MathFloor");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathCeil(HandleFunction callee) {
  // Need one (number) argument.
  if (argc_ != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Check if the result fits in int32.
  double res = math_ceil_impl(args_[0].toNumber());
  int32_t unused;
  bool resultIsInt32 = mozilla::NumberIsInt32(res, &unused);

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'ceil' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argumentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  if (args_[0].isInt32()) {
    MOZ_ASSERT(resultIsInt32);

    // Use an indirect truncation to inform the optimizer it needs to preserve
    // a bailout when the input can't be represented as an int32, even if the
    // final result is fully truncated.
    Int32OperandId intId = writer.guardToInt32(argumentId);
    writer.indirectTruncateInt32Result(intId);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);

    if (resultIsInt32) {
      writer.mathCeilToInt32Result(numberId);
    } else {
      writer.mathCeilNumberResult(numberId);
    }
  }

  writer.returnFromIC();

  trackAttached("MathCeil");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathTrunc(HandleFunction callee) {
  // Need one (number) argument.
  if (argc_ != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Check if the result fits in int32.
  double res = math_trunc_impl(args_[0].toNumber());
  int32_t unused;
  bool resultIsInt32 = mozilla::NumberIsInt32(res, &unused);

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'trunc' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argumentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  if (args_[0].isInt32()) {
    MOZ_ASSERT(resultIsInt32);

    // We don't need an indirect truncation barrier here, because Math.trunc
    // always truncates, but never rounds its input away from zero.
    Int32OperandId intId = writer.guardToInt32(argumentId);
    writer.loadInt32Result(intId);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);

    if (resultIsInt32) {
      writer.mathTruncToInt32Result(numberId);
    } else {
      writer.mathTruncNumberResult(numberId);
    }
  }

  writer.returnFromIC();

  trackAttached("MathTrunc");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathRound(HandleFunction callee) {
  // Need one (number) argument.
  if (argc_ != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Check if the result fits in int32.
  double res = math_round_impl(args_[0].toNumber());
  int32_t unused;
  bool resultIsInt32 = mozilla::NumberIsInt32(res, &unused);

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'round' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argumentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  if (args_[0].isInt32()) {
    MOZ_ASSERT(resultIsInt32);

    // Use an indirect truncation to inform the optimizer it needs to preserve
    // a bailout when the input can't be represented as an int32, even if the
    // final result is fully truncated.
    Int32OperandId intId = writer.guardToInt32(argumentId);
    writer.indirectTruncateInt32Result(intId);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);

    if (resultIsInt32) {
      writer.mathRoundToInt32Result(numberId);
    } else {
      writer.mathFunctionNumberResult(numberId, UnaryMathFunction::Round);
    }
  }

  writer.returnFromIC();

  trackAttached("MathRound");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathSqrt(HandleFunction callee) {
  // Need one (number) argument.
  if (argc_ != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'sqrt' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argumentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  NumberOperandId numberId = writer.guardIsNumber(argumentId);
  writer.mathSqrtNumberResult(numberId);
  writer.returnFromIC();

  trackAttached("MathSqrt");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathFRound(HandleFunction callee) {
  // Need one (number) argument.
  if (argc_ != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'fround' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argumentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  NumberOperandId numberId = writer.guardIsNumber(argumentId);
  writer.mathFRoundNumberResult(numberId);
  writer.returnFromIC();

  trackAttached("MathFRound");
  return AttachDecision::Attach;
}

static bool CanAttachInt32Pow(const Value& baseVal, const Value& powerVal) {
  MOZ_ASSERT(baseVal.isInt32() || baseVal.isBoolean());
  MOZ_ASSERT(powerVal.isInt32() || powerVal.isBoolean());

  int32_t base = baseVal.isInt32() ? baseVal.toInt32() : baseVal.toBoolean();
  int32_t power =
      powerVal.isInt32() ? powerVal.toInt32() : powerVal.toBoolean();

  // x^y where y < 0 is most of the time not an int32, except when x is 1 or y
  // gets large enough. It's hard to determine when exactly y is "large enough",
  // so we don't use Int32PowResult when x != 1 and y < 0.
  // Note: it's important for this condition to match the code generated by
  // MacroAssembler::pow32 to prevent failure loops.
  if (power < 0) {
    return base == 1;
  }

  double res = powi(base, power);
  int32_t unused;
  return mozilla::NumberIsInt32(res, &unused);
}

AttachDecision CallIRGenerator::tryAttachMathPow(HandleFunction callee) {
  // Need two number arguments.
  if (argc_ != 2 || !args_[0].isNumber() || !args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'pow' function.
  emitNativeCalleeGuard(callee);

  ValOperandId baseId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ValOperandId exponentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);

  if (args_[0].isInt32() && args_[1].isInt32() &&
      CanAttachInt32Pow(args_[0], args_[1])) {
    Int32OperandId baseInt32Id = writer.guardToInt32(baseId);
    Int32OperandId exponentInt32Id = writer.guardToInt32(exponentId);
    writer.int32PowResult(baseInt32Id, exponentInt32Id);
  } else {
    NumberOperandId baseNumberId = writer.guardIsNumber(baseId);
    NumberOperandId exponentNumberId = writer.guardIsNumber(exponentId);
    writer.doublePowResult(baseNumberId, exponentNumberId);
  }

  writer.returnFromIC();

  trackAttached("MathPow");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathHypot(HandleFunction callee) {
  // Only optimize if there are 2-4 arguments.
  if (argc_ < 2 || argc_ > 4) {
    return AttachDecision::NoAction;
  }

  for (size_t i = 0; i < argc_; i++) {
    if (!args_[i].isNumber()) {
      return AttachDecision::NoAction;
    }
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'hypot' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId firstId = writer.loadStandardCallArgument(0, argc_);
  ValOperandId secondId = writer.loadStandardCallArgument(1, argc_);

  NumberOperandId firstNumId = writer.guardIsNumber(firstId);
  NumberOperandId secondNumId = writer.guardIsNumber(secondId);

  ValOperandId thirdId;
  ValOperandId fourthId;
  NumberOperandId thirdNumId;
  NumberOperandId fourthNumId;

  switch (argc_) {
    case 2:
      writer.mathHypot2NumberResult(firstNumId, secondNumId);
      break;
    case 3:
      thirdId = writer.loadStandardCallArgument(2, argc_);
      thirdNumId = writer.guardIsNumber(thirdId);
      writer.mathHypot3NumberResult(firstNumId, secondNumId, thirdNumId);
      break;
    case 4:
      thirdId = writer.loadStandardCallArgument(2, argc_);
      fourthId = writer.loadStandardCallArgument(3, argc_);
      thirdNumId = writer.guardIsNumber(thirdId);
      fourthNumId = writer.guardIsNumber(fourthId);
      writer.mathHypot4NumberResult(firstNumId, secondNumId, thirdNumId,
                                    fourthNumId);
      break;
    default:
      MOZ_CRASH("Unexpected number of arguments to hypot function.");
  }

  writer.returnFromIC();

  trackAttached("MathHypot");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathATan2(HandleFunction callee) {
  // Requires two numbers as arguments.
  if (argc_ != 2 || !args_[0].isNumber() || !args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'atan2' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId yId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ValOperandId xId = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);

  NumberOperandId yNumberId = writer.guardIsNumber(yId);
  NumberOperandId xNumberId = writer.guardIsNumber(xId);

  writer.mathAtan2NumberResult(yNumberId, xNumberId);
  writer.returnFromIC();

  trackAttached("MathAtan2");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathMinMax(HandleFunction callee,
                                                    bool isMax) {
  // For now only optimize if there are 1-4 arguments.
  if (argc_ < 1 || argc_ > 4) {
    return AttachDecision::NoAction;
  }

  // Ensure all arguments are numbers.
  bool allInt32 = true;
  for (size_t i = 0; i < argc_; i++) {
    if (!args_[i].isNumber()) {
      return AttachDecision::NoAction;
    }
    if (!args_[i].isInt32()) {
      allInt32 = false;
    }
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is this Math function.
  emitNativeCalleeGuard(callee);

  if (allInt32) {
    ValOperandId valId = writer.loadStandardCallArgument(0, argc_);
    Int32OperandId resId = writer.guardToInt32(valId);
    for (size_t i = 1; i < argc_; i++) {
      ValOperandId argId = writer.loadStandardCallArgument(i, argc_);
      Int32OperandId argInt32Id = writer.guardToInt32(argId);
      resId = writer.int32MinMax(isMax, resId, argInt32Id);
    }
    writer.loadInt32Result(resId);
  } else {
    ValOperandId valId = writer.loadStandardCallArgument(0, argc_);
    NumberOperandId resId = writer.guardIsNumber(valId);
    for (size_t i = 1; i < argc_; i++) {
      ValOperandId argId = writer.loadStandardCallArgument(i, argc_);
      NumberOperandId argNumId = writer.guardIsNumber(argId);
      resId = writer.numberMinMax(isMax, resId, argNumId);
    }
    writer.loadDoubleResult(resId);
  }

  writer.returnFromIC();

  trackAttached(isMax ? "MathMax" : "MathMin");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachSpreadMathMinMax(HandleFunction callee,
                                                          bool isMax) {
  MOZ_ASSERT(op_ == JSOp::SpreadCall);

  // The result will be an int32 if there is at least one argument,
  // and all the arguments are int32.
  bool int32Result = args_.length() > 0;
  for (size_t i = 0; i < args_.length(); i++) {
    if (!args_[i].isNumber()) {
      return AttachDecision::NoAction;
    }
    if (!args_[i].isInt32()) {
      int32Result = false;
    }
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is this Math function.
  emitNativeCalleeGuard(callee);

  // Load the argument array
  ObjOperandId argsId = writer.loadSpreadArgs();

  if (int32Result) {
    writer.int32MinMaxArrayResult(argsId, isMax);
  } else {
    writer.numberMinMaxArrayResult(argsId, isMax);
  }

  writer.returnFromIC();

  trackAttached(isMax ? "MathMax" : "MathMin");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachMathFunction(HandleFunction callee,
                                                      UnaryMathFunction fun) {
  // Need one argument.
  if (argc_ != 1) {
    return AttachDecision::NoAction;
  }

  if (!args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is this Math function.
  emitNativeCalleeGuard(callee);

  ValOperandId argumentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  NumberOperandId numberId = writer.guardIsNumber(argumentId);
  writer.mathFunctionNumberResult(numberId, fun);
  writer.returnFromIC();

  trackAttached("MathFunction");
  return AttachDecision::Attach;
}

static StringOperandId EmitToStringGuard(CacheIRWriter& writer, ValOperandId id,
                                         const Value& v) {
  if (v.isString()) {
    return writer.guardToString(id);
  }
  if (v.isInt32()) {
    Int32OperandId intId = writer.guardToInt32(id);
    return writer.callInt32ToString(intId);
  }
  // At this point we are creating an IC that will handle
  // both Int32 and Double cases.
  MOZ_ASSERT(v.isNumber());
  NumberOperandId numId = writer.guardIsNumber(id);
  return writer.callNumberToString(numId);
}

AttachDecision CallIRGenerator::tryAttachNumberToString(HandleFunction callee) {
  // Expecting no arguments, which means base 10.
  if (argc_ != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive number value.
  if (!thisval_.isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'toString' native function.
  emitNativeCalleeGuard(callee);

  // Initialize the |this| operand.
  ValOperandId thisValId =
      writer.loadArgumentFixedSlot(ArgumentKind::This, argc_);

  // Guard on number and convert to string.
  StringOperandId strId = EmitToStringGuard(writer, thisValId, thisval_);

  // Return the string.
  writer.loadStringResult(strId);
  writer.returnFromIC();

  trackAttached("NumberToString");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachReflectGetPrototypeOf(
    HandleFunction callee) {
  // Need one argument.
  if (argc_ != 1) {
    return AttachDecision::NoAction;
  }

  if (!args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'getPrototypeOf' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId argumentId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(argumentId);

  writer.reflectGetPrototypeOfResult(objId);
  writer.returnFromIC();

  trackAttached("ReflectGetPrototypeOf");
  return AttachDecision::Attach;
}

static bool AtomicsMeetsPreconditions(TypedArrayObject* typedArray,
                                      const Value& index) {
  switch (typedArray->type()) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      break;

    case Scalar::Float32:
    case Scalar::Float64:
    case Scalar::Uint8Clamped:
      // Exclude floating types and Uint8Clamped.
      return false;

    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      MOZ_CRASH("Unsupported TypedArray type");
  }

  // Bounds check the index argument.
  int64_t indexInt64;
  if (!ValueIsInt64Index(index, &indexInt64)) {
    return false;
  }
  if (indexInt64 < 0 || uint64_t(indexInt64) >= typedArray->length()) {
    return false;
  }

  return true;
}

AttachDecision CallIRGenerator::tryAttachAtomicsCompareExchange(
    HandleFunction callee) {
  if (!JitSupportsAtomics()) {
    return AttachDecision::NoAction;
  }

  // Need four arguments.
  if (argc_ != 4) {
    return AttachDecision::NoAction;
  }

  // Arguments: typedArray, index (number), expected, replacement.
  if (!args_[0].isObject() || !args_[0].toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }
  if (!args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  if (!AtomicsMeetsPreconditions(typedArray, args_[1])) {
    return AttachDecision::NoAction;
  }

  Scalar::Type elementType = typedArray->type();
  if (!ValueIsNumeric(elementType, args_[2])) {
    return AttachDecision::NoAction;
  }
  if (!ValueIsNumeric(elementType, args_[3])) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the `compareExchange` native function.
  emitNativeCalleeGuard(callee);

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(arg0Id);
  writer.guardShapeForClass(objId, typedArray->shape());

  // Convert index to intPtr.
  ValOperandId indexId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(args_[1], indexId, /* supportOOB = */ false);

  // Convert expected value to int32/BigInt.
  ValOperandId expectedId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_);
  OperandId numericExpectedId = emitNumericGuard(expectedId, elementType);

  // Convert replacement value to int32/BigInt.
  ValOperandId replacementId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg3, argc_);
  OperandId numericReplacementId = emitNumericGuard(replacementId, elementType);

  writer.atomicsCompareExchangeResult(objId, intPtrIndexId, numericExpectedId,
                                      numericReplacementId, typedArray->type());
  writer.returnFromIC();

  trackAttached("AtomicsCompareExchange");
  return AttachDecision::Attach;
}

bool CallIRGenerator::canAttachAtomicsReadWriteModify() {
  if (!JitSupportsAtomics()) {
    return false;
  }

  // Need three arguments.
  if (argc_ != 3) {
    return false;
  }

  // Arguments: typedArray, index (number), value.
  if (!args_[0].isObject() || !args_[0].toObject().is<TypedArrayObject>()) {
    return false;
  }
  if (!args_[1].isNumber()) {
    return false;
  }

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  if (!AtomicsMeetsPreconditions(typedArray, args_[1])) {
    return false;
  }
  if (!ValueIsNumeric(typedArray->type(), args_[2])) {
    return false;
  }
  return true;
}

CallIRGenerator::AtomicsReadWriteModifyOperands
CallIRGenerator::emitAtomicsReadWriteModifyOperands(HandleFunction callee) {
  MOZ_ASSERT(canAttachAtomicsReadWriteModify());

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is this Atomics function.
  emitNativeCalleeGuard(callee);

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(arg0Id);
  writer.guardShapeForClass(objId, typedArray->shape());

  // Convert index to intPtr.
  ValOperandId indexId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(args_[1], indexId, /* supportOOB = */ false);

  // Convert value to int32/BigInt.
  ValOperandId valueId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_);
  OperandId numericValueId = emitNumericGuard(valueId, typedArray->type());

  return {objId, intPtrIndexId, numericValueId};
}

AttachDecision CallIRGenerator::tryAttachAtomicsExchange(
    HandleFunction callee) {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands(callee);

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();

  writer.atomicsExchangeResult(objId, intPtrIndexId, numericValueId,
                               typedArray->type());
  writer.returnFromIC();

  trackAttached("AtomicsExchange");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAtomicsAdd(HandleFunction callee) {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands(callee);

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = (op_ == JSOp::CallIgnoresRv);

  writer.atomicsAddResult(objId, intPtrIndexId, numericValueId,
                          typedArray->type(), forEffect);
  writer.returnFromIC();

  trackAttached("AtomicsAdd");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAtomicsSub(HandleFunction callee) {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands(callee);

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = (op_ == JSOp::CallIgnoresRv);

  writer.atomicsSubResult(objId, intPtrIndexId, numericValueId,
                          typedArray->type(), forEffect);
  writer.returnFromIC();

  trackAttached("AtomicsSub");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAtomicsAnd(HandleFunction callee) {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands(callee);

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = (op_ == JSOp::CallIgnoresRv);

  writer.atomicsAndResult(objId, intPtrIndexId, numericValueId,
                          typedArray->type(), forEffect);
  writer.returnFromIC();

  trackAttached("AtomicsAnd");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAtomicsOr(HandleFunction callee) {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands(callee);

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = (op_ == JSOp::CallIgnoresRv);

  writer.atomicsOrResult(objId, intPtrIndexId, numericValueId,
                         typedArray->type(), forEffect);
  writer.returnFromIC();

  trackAttached("AtomicsOr");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAtomicsXor(HandleFunction callee) {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands(callee);

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = (op_ == JSOp::CallIgnoresRv);

  writer.atomicsXorResult(objId, intPtrIndexId, numericValueId,
                          typedArray->type(), forEffect);
  writer.returnFromIC();

  trackAttached("AtomicsXor");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAtomicsLoad(HandleFunction callee) {
  if (!JitSupportsAtomics()) {
    return AttachDecision::NoAction;
  }

  // Need two arguments.
  if (argc_ != 2) {
    return AttachDecision::NoAction;
  }

  // Arguments: typedArray, index (number).
  if (!args_[0].isObject() || !args_[0].toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }
  if (!args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  if (!AtomicsMeetsPreconditions(typedArray, args_[1])) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the `load` native function.
  emitNativeCalleeGuard(callee);

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(arg0Id);
  writer.guardShapeForClass(objId, typedArray->shape());

  // Convert index to intPtr.
  ValOperandId indexId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(args_[1], indexId, /* supportOOB = */ false);

  writer.atomicsLoadResult(objId, intPtrIndexId, typedArray->type());
  writer.returnFromIC();

  trackAttached("AtomicsLoad");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAtomicsStore(HandleFunction callee) {
  if (!JitSupportsAtomics()) {
    return AttachDecision::NoAction;
  }

  // Need three arguments.
  if (argc_ != 3) {
    return AttachDecision::NoAction;
  }

  // Atomics.store() is annoying because it returns the result of converting the
  // value by ToInteger(), not the input value, nor the result of converting the
  // value by ToInt32(). It is especially annoying because almost nobody uses
  // the result value.
  //
  // As an expedient compromise, therefore, we inline only if the result is
  // obviously unused or if the argument is already Int32 and thus requires no
  // conversion.

  // Arguments: typedArray, index (number), value.
  if (!args_[0].isObject() || !args_[0].toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }
  if (!args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  if (!AtomicsMeetsPreconditions(typedArray, args_[1])) {
    return AttachDecision::NoAction;
  }

  Scalar::Type elementType = typedArray->type();
  if (!ValueIsNumeric(elementType, args_[2])) {
    return AttachDecision::NoAction;
  }

  bool guardIsInt32 =
      !Scalar::isBigIntType(elementType) && op_ != JSOp::CallIgnoresRv;

  if (guardIsInt32 && !args_[2].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the `store` native function.
  emitNativeCalleeGuard(callee);

  ValOperandId arg0Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objId = writer.guardToObject(arg0Id);
  writer.guardShapeForClass(objId, typedArray->shape());

  // Convert index to intPtr.
  ValOperandId indexId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(args_[1], indexId, /* supportOOB = */ false);

  // Ensure value is int32 or BigInt.
  ValOperandId valueId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_);
  OperandId numericValueId;
  if (guardIsInt32) {
    numericValueId = writer.guardToInt32(valueId);
  } else {
    numericValueId = emitNumericGuard(valueId, elementType);
  }

  writer.atomicsStoreResult(objId, intPtrIndexId, numericValueId,
                            typedArray->type());
  writer.returnFromIC();

  trackAttached("AtomicsStore");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAtomicsIsLockFree(
    HandleFunction callee) {
  // Need one argument.
  if (argc_ != 1) {
    return AttachDecision::NoAction;
  }

  if (!args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the `isLockFree` native function.
  emitNativeCalleeGuard(callee);

  // Ensure value is int32.
  ValOperandId valueId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  Int32OperandId int32ValueId = writer.guardToInt32(valueId);

  writer.atomicsIsLockFreeResult(int32ValueId);
  writer.returnFromIC();

  trackAttached("AtomicsIsLockFree");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachBoolean(HandleFunction callee) {
  // Need zero or one argument.
  if (argc_ > 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'Boolean' native function.
  emitNativeCalleeGuard(callee);

  if (argc_ == 0) {
    writer.loadBooleanResult(false);
  } else {
    ValOperandId valId =
        writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

    writer.loadValueTruthyResult(valId);
  }

  writer.returnFromIC();

  trackAttached("Boolean");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachBailout(HandleFunction callee) {
  // Expecting no arguments.
  if (argc_ != 0) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'bailout' native function.
  emitNativeCalleeGuard(callee);

  writer.bailout();
  writer.loadUndefinedResult();
  writer.returnFromIC();

  trackAttached("Bailout");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAssertFloat32(HandleFunction callee) {
  // Expecting two arguments.
  if (argc_ != 2) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'assertFloat32' native function.
  emitNativeCalleeGuard(callee);

  // TODO: Warp doesn't yet optimize Float32 (bug 1655773).

  // NOP when not in IonMonkey.
  writer.loadUndefinedResult();
  writer.returnFromIC();

  trackAttached("AssertFloat32");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachAssertRecoveredOnBailout(
    HandleFunction callee) {
  // Expecting two arguments.
  if (argc_ != 2) {
    return AttachDecision::NoAction;
  }

  // (Fuzzing unsafe) testing function which must be called with a constant
  // boolean as its second argument.
  bool mustBeRecovered = args_[1].toBoolean();

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'assertRecoveredOnBailout' native function.
  emitNativeCalleeGuard(callee);

  ValOperandId valId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  writer.assertRecoveredOnBailoutResult(valId, mustBeRecovered);
  writer.returnFromIC();

  trackAttached("AssertRecoveredOnBailout");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachObjectIs(HandleFunction callee) {
  // Need two arguments.
  if (argc_ != 2) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the `is` native function.
  emitNativeCalleeGuard(callee);

  ValOperandId lhsId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ValOperandId rhsId = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);

  HandleValue lhs = args_[0];
  HandleValue rhs = args_[1];

  if (!isFirstStub_) {
    writer.sameValueResult(lhsId, rhsId);
  } else if (lhs.isNumber() && rhs.isNumber() &&
             !(lhs.isInt32() && rhs.isInt32())) {
    NumberOperandId lhsNumId = writer.guardIsNumber(lhsId);
    NumberOperandId rhsNumId = writer.guardIsNumber(rhsId);
    writer.compareDoubleSameValueResult(lhsNumId, rhsNumId);
  } else if (!SameType(lhs, rhs)) {
    // Compare tags for strictly different types.
    ValueTagOperandId lhsTypeId = writer.loadValueTag(lhsId);
    ValueTagOperandId rhsTypeId = writer.loadValueTag(rhsId);
    writer.guardTagNotEqual(lhsTypeId, rhsTypeId);
    writer.loadBooleanResult(false);
  } else {
    MOZ_ASSERT(lhs.type() == rhs.type());
    MOZ_ASSERT(lhs.type() != JS::ValueType::Double);

    switch (lhs.type()) {
      case JS::ValueType::Int32: {
        Int32OperandId lhsIntId = writer.guardToInt32(lhsId);
        Int32OperandId rhsIntId = writer.guardToInt32(rhsId);
        writer.compareInt32Result(JSOp::StrictEq, lhsIntId, rhsIntId);
        break;
      }
      case JS::ValueType::Boolean: {
        Int32OperandId lhsIntId = writer.guardBooleanToInt32(lhsId);
        Int32OperandId rhsIntId = writer.guardBooleanToInt32(rhsId);
        writer.compareInt32Result(JSOp::StrictEq, lhsIntId, rhsIntId);
        break;
      }
      case JS::ValueType::Undefined: {
        writer.guardIsUndefined(lhsId);
        writer.guardIsUndefined(rhsId);
        writer.loadBooleanResult(true);
        break;
      }
      case JS::ValueType::Null: {
        writer.guardIsNull(lhsId);
        writer.guardIsNull(rhsId);
        writer.loadBooleanResult(true);
        break;
      }
      case JS::ValueType::String: {
        StringOperandId lhsStrId = writer.guardToString(lhsId);
        StringOperandId rhsStrId = writer.guardToString(rhsId);
        writer.compareStringResult(JSOp::StrictEq, lhsStrId, rhsStrId);
        break;
      }
      case JS::ValueType::Symbol: {
        SymbolOperandId lhsSymId = writer.guardToSymbol(lhsId);
        SymbolOperandId rhsSymId = writer.guardToSymbol(rhsId);
        writer.compareSymbolResult(JSOp::StrictEq, lhsSymId, rhsSymId);
        break;
      }
      case JS::ValueType::BigInt: {
        BigIntOperandId lhsBigIntId = writer.guardToBigInt(lhsId);
        BigIntOperandId rhsBigIntId = writer.guardToBigInt(rhsId);
        writer.compareBigIntResult(JSOp::StrictEq, lhsBigIntId, rhsBigIntId);
        break;
      }
      case JS::ValueType::Object: {
        ObjOperandId lhsObjId = writer.guardToObject(lhsId);
        ObjOperandId rhsObjId = writer.guardToObject(rhsId);
        writer.compareObjectResult(JSOp::StrictEq, lhsObjId, rhsObjId);
        break;
      }

      case JS::ValueType::Double:
      case JS::ValueType::Magic:
      case JS::ValueType::PrivateGCThing:
        MOZ_CRASH("Unexpected type");
    }
  }

  writer.returnFromIC();

  trackAttached("ObjectIs");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachObjectIsPrototypeOf(
    HandleFunction callee) {
  // Ensure |this| is an object.
  if (!thisval_.isObject()) {
    return AttachDecision::NoAction;
  }

  // Need a single argument.
  if (argc_ != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the `isPrototypeOf` native function.
  emitNativeCalleeGuard(callee);

  // Guard that |this| is an object.
  ValOperandId thisValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::This, argcId);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);

  writer.loadInstanceOfObjectResult(argId, thisObjId);
  writer.returnFromIC();

  trackAttached("ObjectIsPrototypeOf");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachObjectToString(HandleFunction callee) {
  // Expecting no arguments.
  if (argc_ != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is an object.
  if (!thisval_.isObject()) {
    return AttachDecision::NoAction;
  }

  // Don't attach if the object has @@toStringTag or is a proxy.
  if (!ObjectClassToString(cx_, &thisval_.toObject())) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'toString' native function.
  emitNativeCalleeGuard(callee);

  // Guard that |this| is an object.
  ValOperandId thisValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::This, argcId);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);

  writer.objectToStringResult(thisObjId);
  writer.returnFromIC();

  trackAttached("ObjectToString");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachBigIntAsIntN(HandleFunction callee) {
  // Need two arguments (Int32, BigInt).
  if (argc_ != 2 || !args_[0].isInt32() || !args_[1].isBigInt()) {
    return AttachDecision::NoAction;
  }

  // Negative bits throws an error.
  if (args_[0].toInt32() < 0) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'BigInt.asIntN' native function.
  emitNativeCalleeGuard(callee);

  // Convert bits to int32.
  ValOperandId bitsId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  Int32OperandId int32BitsId = writer.guardToInt32Index(bitsId);

  // Number of bits mustn't be negative.
  writer.guardInt32IsNonNegative(int32BitsId);

  ValOperandId arg1Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  BigIntOperandId bigIntId = writer.guardToBigInt(arg1Id);

  writer.bigIntAsIntNResult(int32BitsId, bigIntId);
  writer.returnFromIC();

  trackAttached("BigIntAsIntN");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachBigIntAsUintN(HandleFunction callee) {
  // Need two arguments (Int32, BigInt).
  if (argc_ != 2 || !args_[0].isInt32() || !args_[1].isBigInt()) {
    return AttachDecision::NoAction;
  }

  // Negative bits throws an error.
  if (args_[0].toInt32() < 0) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'BigInt.asUintN' native function.
  emitNativeCalleeGuard(callee);

  // Convert bits to int32.
  ValOperandId bitsId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  Int32OperandId int32BitsId = writer.guardToInt32Index(bitsId);

  // Number of bits mustn't be negative.
  writer.guardInt32IsNonNegative(int32BitsId);

  ValOperandId arg1Id = writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  BigIntOperandId bigIntId = writer.guardToBigInt(arg1Id);

  writer.bigIntAsUintNResult(int32BitsId, bigIntId);
  writer.returnFromIC();

  trackAttached("BigIntAsUintN");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachFunCall(HandleFunction callee) {
  if (!callee->isNativeWithoutJitEntry() || callee->native() != fun_call) {
    return AttachDecision::NoAction;
  }

  if (!thisval_.isObject() || !thisval_.toObject().is<JSFunction>()) {
    return AttachDecision::NoAction;
  }
  auto* target = &thisval_.toObject().as<JSFunction>();

  bool isScripted = target->hasJitEntry();
  MOZ_ASSERT_IF(!isScripted, target->isNativeWithoutJitEntry());

  if (target->isClassConstructor()) {
    return AttachDecision::NoAction;
  }
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard that callee is the |fun_call| native function.
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);
  writer.guardSpecificFunction(calleeObjId, callee);

  // Guard that |this| is an object.
  ValOperandId thisValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::This, argcId);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);

  CallFlags targetFlags(CallFlags::FunCall);
  if (mode_ == ICState::Mode::Specialized) {
    // Ensure that |this| is the expected target function.
    emitCalleeGuard(thisObjId, target);

    if (cx_->realm() == target->realm()) {
      targetFlags.setIsSameRealm();
    }

    if (isScripted) {
      writer.callScriptedFunction(thisObjId, argcId, targetFlags);
    } else {
      writer.callNativeFunction(thisObjId, argcId, op_, target, targetFlags);
    }
  } else {
    // Guard that |this| is a function.
    writer.guardClass(thisObjId, GuardClassKind::JSFunction);

    // Guard that function is not a class constructor.
    writer.guardNotClassConstructor(thisObjId);

    if (isScripted) {
      writer.guardFunctionHasJitEntry(thisObjId, /*isConstructing =*/false);
      writer.callScriptedFunction(thisObjId, argcId, targetFlags);
    } else {
      writer.guardFunctionHasNoJitEntry(thisObjId);
      writer.callAnyNativeFunction(thisObjId, argcId, targetFlags);
    }
  }

  writer.returnFromIC();

  if (isScripted) {
    trackAttached("Scripted fun_call");
  } else {
    trackAttached("Native fun_call");
  }

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachIsTypedArray(HandleFunction callee,
                                                      bool isPossiblyWrapped) {
  // Self-hosted code calls this with a single object argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objArgId = writer.guardToObject(argId);
  writer.isTypedArrayResult(objArgId, isPossiblyWrapped);
  writer.returnFromIC();

  trackAttached(isPossiblyWrapped ? "IsPossiblyWrappedTypedArray"
                                  : "IsTypedArray");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachIsTypedArrayConstructor(
    HandleFunction callee) {
  // Self-hosted code calls this with a single object argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objArgId = writer.guardToObject(argId);
  writer.isTypedArrayConstructorResult(objArgId);
  writer.returnFromIC();

  trackAttached("IsTypedArrayConstructor");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachTypedArrayByteOffset(
    HandleFunction callee) {
  // Self-hosted code calls this with a single TypedArrayObject argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[0].toObject().is<TypedArrayObject>());

  auto* tarr = &args_[0].toObject().as<TypedArrayObject>();

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objArgId = writer.guardToObject(argId);
  if (tarr->byteOffset() <= INT32_MAX) {
    writer.arrayBufferViewByteOffsetInt32Result(objArgId);
  } else {
    writer.arrayBufferViewByteOffsetDoubleResult(objArgId);
  }
  writer.returnFromIC();

  trackAttached("TypedArrayByteOffset");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachTypedArrayElementSize(
    HandleFunction callee) {
  // Self-hosted code calls this with a single TypedArrayObject argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[0].toObject().is<TypedArrayObject>());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objArgId = writer.guardToObject(argId);
  writer.typedArrayElementSizeResult(objArgId);
  writer.returnFromIC();

  trackAttached("TypedArrayElementSize");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachTypedArrayLength(
    HandleFunction callee, bool isPossiblyWrapped) {
  // Self-hosted code calls this with a single, possibly wrapped,
  // TypedArrayObject argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Only optimize when the object isn't a wrapper.
  if (isPossiblyWrapped && IsWrapper(&args_[0].toObject())) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(args_[0].toObject().is<TypedArrayObject>());

  auto* tarr = &args_[0].toObject().as<TypedArrayObject>();

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objArgId = writer.guardToObject(argId);

  if (isPossiblyWrapped) {
    writer.guardIsNotProxy(objArgId);
  }

  if (tarr->length() <= INT32_MAX) {
    writer.loadArrayBufferViewLengthInt32Result(objArgId);
  } else {
    writer.loadArrayBufferViewLengthDoubleResult(objArgId);
  }
  writer.returnFromIC();

  trackAttached("TypedArrayLength");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachArrayBufferByteLength(
    HandleFunction callee, bool isPossiblyWrapped) {
  // Self-hosted code calls this with a single, possibly wrapped,
  // ArrayBufferObject argument.
  MOZ_ASSERT(argc_ == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Only optimize when the object isn't a wrapper.
  if (isPossiblyWrapped && IsWrapper(&args_[0].toObject())) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(args_[0].toObject().is<ArrayBufferObject>());

  auto* buffer = &args_[0].toObject().as<ArrayBufferObject>();

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objArgId = writer.guardToObject(argId);

  if (isPossiblyWrapped) {
    writer.guardIsNotProxy(objArgId);
  }

  if (buffer->byteLength() <= INT32_MAX) {
    writer.loadArrayBufferByteLengthInt32Result(objArgId);
  } else {
    writer.loadArrayBufferByteLengthDoubleResult(objArgId);
  }
  writer.returnFromIC();

  trackAttached("ArrayBufferByteLength");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachIsConstructing(HandleFunction callee) {
  // Self-hosted code calls this with no arguments in function scripts.
  MOZ_ASSERT(argc_ == 0);
  MOZ_ASSERT(script_->isFunction());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.frameIsConstructingResult();
  writer.returnFromIC();

  trackAttached("IsConstructing");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachGetNextMapSetEntryForIterator(
    HandleFunction callee, bool isMap) {
  // Self-hosted code calls this with two objects.
  MOZ_ASSERT(argc_ == 2);
  if (isMap) {
    MOZ_ASSERT(args_[0].toObject().is<MapIteratorObject>());
  } else {
    MOZ_ASSERT(args_[0].toObject().is<SetIteratorObject>());
  }
  MOZ_ASSERT(args_[1].toObject().is<ArrayObject>());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId iterId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objIterId = writer.guardToObject(iterId);

  ValOperandId resultArrId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  ObjOperandId objResultArrId = writer.guardToObject(resultArrId);

  writer.getNextMapSetEntryForIteratorResult(objIterId, objResultArrId, isMap);
  writer.returnFromIC();

  trackAttached("GetNextMapSetEntryForIterator");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachFinishBoundFunctionInit(
    HandleFunction callee) {
  // Self-hosted code calls this with (boundFunction, targetObj, argCount)
  // arguments.
  MOZ_ASSERT(argc_ == 3);
  MOZ_ASSERT(args_[0].toObject().is<JSFunction>());
  MOZ_ASSERT(args_[1].isObject());
  MOZ_ASSERT(args_[2].isInt32());

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId boundId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  ObjOperandId objBoundId = writer.guardToObject(boundId);

  ValOperandId targetId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);
  ObjOperandId objTargetId = writer.guardToObject(targetId);

  ValOperandId argCountId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_);
  Int32OperandId int32ArgCountId = writer.guardToInt32(argCountId);

  writer.finishBoundFunctionInitResult(objBoundId, objTargetId,
                                       int32ArgCountId);
  writer.returnFromIC();

  trackAttached("FinishBoundFunctionInit");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachNewArrayIterator(
    HandleFunction callee) {
  // Self-hosted code calls this without any arguments
  MOZ_ASSERT(argc_ == 0);

  JSObject* templateObj = NewArrayIteratorTemplate(cx_);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.newArrayIteratorResult(templateObj);
  writer.returnFromIC();

  trackAttached("NewArrayIterator");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachNewStringIterator(
    HandleFunction callee) {
  // Self-hosted code calls this without any arguments
  MOZ_ASSERT(argc_ == 0);

  JSObject* templateObj = NewStringIteratorTemplate(cx_);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.newStringIteratorResult(templateObj);
  writer.returnFromIC();

  trackAttached("NewStringIterator");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachNewRegExpStringIterator(
    HandleFunction callee) {
  // Self-hosted code calls this without any arguments
  MOZ_ASSERT(argc_ == 0);

  JSObject* templateObj = NewRegExpStringIteratorTemplate(cx_);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.newRegExpStringIteratorResult(templateObj);
  writer.returnFromIC();

  trackAttached("NewRegExpStringIterator");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachArrayIteratorPrototypeOptimizable(
    HandleFunction callee) {
  // Self-hosted code calls this without any arguments
  MOZ_ASSERT(argc_ == 0);

  if (!isFirstStub_) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  NativeObject* arrayIteratorProto;
  uint32_t slot;
  JSFunction* nextFun;
  if (!IsArrayIteratorPrototypeOptimizable(cx_, &arrayIteratorProto, &slot,
                                           &nextFun)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ObjOperandId protoId = writer.loadObject(arrayIteratorProto);
  ObjOperandId nextId = writer.loadObject(nextFun);

  writer.guardShape(protoId, arrayIteratorProto->shape());

  // Ensure that proto[slot] == nextFun.
  writer.guardDynamicSlotIsSpecificObject(protoId, nextId, slot);
  writer.loadBooleanResult(true);
  writer.returnFromIC();

  trackAttached("ArrayIteratorPrototypeOptimizable");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachObjectCreate(HandleFunction callee) {
  // Need a single object-or-null argument.
  if (argc_ != 1 || !args_[0].isObjectOrNull()) {
    return AttachDecision::NoAction;
  }

  if (!isFirstStub_) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  RootedObject proto(cx_, args_[0].toObjectOrNull());
  JSObject* templateObj = ObjectCreateImpl(cx_, proto, TenuredObject);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee is the 'create' native function.
  emitNativeCalleeGuard(callee);

  // Guard on the proto argument.
  ValOperandId argId = writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_);
  if (proto) {
    ObjOperandId protoId = writer.guardToObject(argId);
    writer.guardSpecificObject(protoId, proto);
  } else {
    writer.guardIsNull(argId);
  }

  writer.objectCreateResult(templateObj);
  writer.returnFromIC();

  trackAttached("ObjectCreate");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachArrayConstructor(
    HandleFunction callee) {
  // Only optimize the |Array()| and |Array(n)| cases (with or without |new|)
  // for now. Note that self-hosted code calls this without |new| via std_Array.
  if (argc_ > 1) {
    return AttachDecision::NoAction;
  }
  if (argc_ == 1 && !args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  int32_t length = (argc_ == 1) ? args_[0].toInt32() : 0;
  if (length < 0 || uint32_t(length) > ArrayObject::EagerAllocationMaxLength) {
    return AttachDecision::NoAction;
  }

  // We allow inlining this function across realms so make sure the template
  // object is allocated in that realm. See CanInlineNativeCrossRealm.
  JSObject* templateObj;
  {
    AutoRealm ar(cx_, callee);
    templateObj = NewDenseFullyAllocatedArray(
        cx_, length, /* proto = */ nullptr, TenuredObject);
    if (!templateObj) {
      cx_->recoverFromOutOfMemory();
      return AttachDecision::NoAction;
    }
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee and newTarget (if constructing) are this Array constructor
  // function.
  emitNativeCalleeGuard(callee);

  CallFlags flags(IsConstructPC(pc_), IsSpreadPC(pc_));

  Int32OperandId lengthId;
  if (argc_ == 1) {
    ValOperandId arg0Id =
        writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_, flags);
    lengthId = writer.guardToInt32(arg0Id);
  } else {
    MOZ_ASSERT(argc_ == 0);
    lengthId = writer.loadInt32Constant(0);
  }

  writer.newArrayFromLengthResult(templateObj, lengthId);
  writer.returnFromIC();

  trackAttached("ArrayConstructor");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachTypedArrayConstructor(
    HandleFunction callee) {
  MOZ_ASSERT(IsConstructPC(pc_));

  if (argc_ == 0 || argc_ > 3) {
    return AttachDecision::NoAction;
  }

  if (!isFirstStub_) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  // The first argument must be int32 or a non-proxy object.
  if (!args_[0].isInt32() && !args_[0].isObject()) {
    return AttachDecision::NoAction;
  }
  if (args_[0].isObject() && args_[0].toObject().is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

#ifdef JS_CODEGEN_X86
  // Unfortunately NewTypedArrayFromArrayBufferResult needs more registers than
  // we can easily support on 32-bit x86 for now.
  if (args_[0].isObject() &&
      args_[0].toObject().is<ArrayBufferObjectMaybeShared>()) {
    return AttachDecision::NoAction;
  }
#endif

  RootedObject templateObj(cx_);
  if (!TypedArrayObject::GetTemplateObjectForNative(cx_, callee->native(),
                                                    args_, &templateObj)) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  if (!templateObj) {
    // This can happen for large length values.
    MOZ_ASSERT(args_[0].isInt32());
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard callee and newTarget are this TypedArray constructor function.
  emitNativeCalleeGuard(callee);

  CallFlags flags(IsConstructPC(pc_), IsSpreadPC(pc_));
  ValOperandId arg0Id =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg0, argc_, flags);

  if (args_[0].isInt32()) {
    // From length.
    Int32OperandId lengthId = writer.guardToInt32(arg0Id);
    writer.newTypedArrayFromLengthResult(templateObj, lengthId);
  } else {
    JSObject* obj = &args_[0].toObject();
    ObjOperandId objId = writer.guardToObject(arg0Id);

    if (obj->is<ArrayBufferObjectMaybeShared>()) {
      // From ArrayBuffer.
      if (obj->is<ArrayBufferObject>()) {
        writer.guardClass(objId, GuardClassKind::ArrayBuffer);
      } else {
        MOZ_ASSERT(obj->is<SharedArrayBufferObject>());
        writer.guardClass(objId, GuardClassKind::SharedArrayBuffer);
      }
      ValOperandId byteOffsetId;
      if (argc_ > 1) {
        byteOffsetId =
            writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_, flags);
      } else {
        byteOffsetId = writer.loadUndefined();
      }
      ValOperandId lengthId;
      if (argc_ > 2) {
        lengthId =
            writer.loadArgumentFixedSlot(ArgumentKind::Arg2, argc_, flags);
      } else {
        lengthId = writer.loadUndefined();
      }
      writer.newTypedArrayFromArrayBufferResult(templateObj, objId,
                                                byteOffsetId, lengthId);
    } else {
      // From Array-like.
      writer.guardIsNotArrayBufferMaybeShared(objId);
      writer.guardIsNotProxy(objId);
      writer.newTypedArrayFromArrayResult(templateObj, objId);
    }
  }

  writer.returnFromIC();

  trackAttached("TypedArrayConstructor");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachFunApply(HandleFunction calleeFunc) {
  if (!calleeFunc->isNativeWithoutJitEntry() ||
      calleeFunc->native() != fun_apply) {
    return AttachDecision::NoAction;
  }

  if (argc_ != 2) {
    return AttachDecision::NoAction;
  }

  if (!thisval_.isObject() || !thisval_.toObject().is<JSFunction>()) {
    return AttachDecision::NoAction;
  }
  auto* target = &thisval_.toObject().as<JSFunction>();

  bool isScripted = target->hasJitEntry();
  MOZ_ASSERT_IF(!isScripted, target->isNativeWithoutJitEntry());

  if (target->isClassConstructor()) {
    return AttachDecision::NoAction;
  }

  CallFlags::ArgFormat format = CallFlags::Standard;
  if (args_[1].isObject() && args_[1].toObject().is<ArgumentsObject>()) {
    auto* argsObj = &args_[1].toObject().as<ArgumentsObject>();
    if (argsObj->hasOverriddenElement() || argsObj->anyArgIsForwarded() ||
        argsObj->hasOverriddenLength() ||
        argsObj->initialLength() > JIT_ARGS_LENGTH_MAX) {
      return AttachDecision::NoAction;
    }
    format = CallFlags::FunApplyArgsObj;
  } else if (args_[1].isObject() && args_[1].toObject().is<ArrayObject>() &&
             args_[1].toObject().as<ArrayObject>().length() <=
                 JIT_ARGS_LENGTH_MAX) {
    format = CallFlags::FunApplyArray;
  } else {
    return AttachDecision::NoAction;
  }

  Int32OperandId argcId(writer.setInputOperandId(0));

  // Guard that callee is the |fun_apply| native function.
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);
  writer.guardSpecificFunction(calleeObjId, calleeFunc);

  // Guard that |this| is an object.
  ValOperandId thisValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::This, argcId);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);

  ValOperandId argValId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_);

  if (format == CallFlags::FunApplyArgsObj) {
    ObjOperandId argObjId = writer.guardToObject(argValId);
    if (args_[1].toObject().is<MappedArgumentsObject>()) {
      writer.guardClass(argObjId, GuardClassKind::MappedArguments);
    } else {
      MOZ_ASSERT(args_[1].toObject().is<UnmappedArgumentsObject>());
      writer.guardClass(argObjId, GuardClassKind::UnmappedArguments);
    }
    uint8_t flags = ArgumentsObject::ELEMENT_OVERRIDDEN_BIT |
                    ArgumentsObject::FORWARDED_ARGUMENTS_BIT;
    writer.guardArgumentsObjectFlags(argObjId, flags);
  } else {
    MOZ_ASSERT(format == CallFlags::FunApplyArray);
    ObjOperandId argObjId = writer.guardToObject(argValId);
    writer.guardClass(argObjId, GuardClassKind::Array);
    writer.guardArrayIsPacked(argObjId);
  }

  CallFlags targetFlags(format);
  if (mode_ == ICState::Mode::Specialized) {
    // Ensure that |this| is the expected target function.
    emitCalleeGuard(thisObjId, target);

    if (cx_->realm() == target->realm()) {
      targetFlags.setIsSameRealm();
    }

    if (isScripted) {
      writer.callScriptedFunction(thisObjId, argcId, targetFlags);
    } else {
      writer.callNativeFunction(thisObjId, argcId, op_, target, targetFlags);
    }
  } else {
    // Guard that |this| is a function.
    writer.guardClass(thisObjId, GuardClassKind::JSFunction);

    // Guard that function is not a class constructor.
    writer.guardNotClassConstructor(thisObjId);

    if (isScripted) {
      // Guard that function is scripted.
      writer.guardFunctionHasJitEntry(thisObjId, /*constructing =*/false);
      writer.callScriptedFunction(thisObjId, argcId, targetFlags);
    } else {
      // Guard that function is native.
      writer.guardFunctionHasNoJitEntry(thisObjId);
      writer.callAnyNativeFunction(thisObjId, argcId, targetFlags);
    }
  }

  writer.returnFromIC();

  if (isScripted) {
    trackAttached("Scripted fun_apply");
  } else {
    trackAttached("Native fun_apply");
  }

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachWasmCall(HandleFunction calleeFunc) {
  // Try to optimize calls into Wasm code by emitting the CallWasmFunction
  // CacheIR op. Baseline ICs currently treat this as a CallScriptedFunction op
  // (calling Wasm's JitEntry stub) but Warp transpiles it to a more direct call
  // into Wasm code.
  //
  // Note: some code refers to these optimized Wasm calls as "inlined" calls.

  MOZ_ASSERT(calleeFunc->isWasmWithJitEntry());

  if (!JitOptions.enableWasmIonFastCalls) {
    return AttachDecision::NoAction;
  }
  if (!isFirstStub_) {
    return AttachDecision::NoAction;
  }
  if (JSOp(*pc_) != JSOp::Call && JSOp(*pc_) != JSOp::CallIgnoresRv) {
    return AttachDecision::NoAction;
  }
  if (cx_->realm() != calleeFunc->realm()) {
    return AttachDecision::NoAction;
  }

  wasm::Instance& inst = wasm::ExportedFunctionToInstance(calleeFunc);
  uint32_t funcIndex = inst.code().getFuncIndex(calleeFunc);

  auto bestTier = inst.code().bestTier();
  const wasm::FuncExport& funcExport =
      inst.metadata(bestTier).lookupFuncExport(funcIndex);

  MOZ_ASSERT(!IsInsideNursery(inst.object()));
  MOZ_ASSERT(funcExport.canHaveJitEntry(),
             "Function should allow a Wasm JitEntry");

  const wasm::FuncType& sig = funcExport.funcType();

  // If there are too many arguments, don't optimize (we won't be able to store
  // the arguments in the LIR node).
  static_assert(wasm::MaxArgsForJitInlineCall <= ArgumentKindArgIndexLimit);
  if (sig.args().length() > wasm::MaxArgsForJitInlineCall ||
      argc_ > ArgumentKindArgIndexLimit) {
    return AttachDecision::NoAction;
  }

  // If there are too many results, don't optimize as Warp currently doesn't
  // have code to handle this.
  if (sig.results().length() > wasm::MaxResultsForJitInlineCall) {
    return AttachDecision::NoAction;
  }

  // Bug 1631656 - Don't try to optimize with I64 args on 32-bit platforms
  // because it is more difficult (because it requires multiple LIR arguments
  // per I64).
  //
  // Bug 1631650 - On 64-bit platforms, we also give up optimizing for I64 args
  // spilled to the stack because it causes problems with register allocation.
#ifdef JS_64BIT
  constexpr bool optimizeWithI64 = true;
#else
  constexpr bool optimizeWithI64 = false;
#endif
  ABIArgGenerator abi;
  for (const auto& valType : sig.args()) {
    MIRType mirType = ToMIRType(valType);
    ABIArg abiArg = abi.next(mirType);
    if (mirType != MIRType::Int64) {
      continue;
    }
    if (!optimizeWithI64 || abiArg.kind() == ABIArg::Stack) {
      return AttachDecision::NoAction;
    }
  }

  // Check that all arguments can be converted to the Wasm type in Warp code
  // without bailing out.
  for (size_t i = 0; i < sig.args().length(); i++) {
    Value argVal = i < argc_ ? args_[i] : UndefinedValue();
    switch (sig.args()[i].kind()) {
      case wasm::ValType::I32:
      case wasm::ValType::F32:
      case wasm::ValType::F64:
        if (!argVal.isNumber() && !argVal.isBoolean() &&
            !argVal.isUndefined()) {
          return AttachDecision::NoAction;
        }
        break;
      case wasm::ValType::I64:
        if (!argVal.isBigInt() && !argVal.isBoolean() && !argVal.isString()) {
          return AttachDecision::NoAction;
        }
        break;
      case wasm::ValType::Rtt:
      case wasm::ValType::V128:
        MOZ_CRASH("Function should not have a Wasm JitEntry");
      case wasm::ValType::Ref:
        // All values can be boxed as AnyRef.
        MOZ_ASSERT(sig.args()[i].refTypeKind() == wasm::RefType::Extern,
                   "Unexpected type for Wasm JitEntry");
        break;
    }
  }

  CallFlags flags(/* isConstructing = */ false, /* isSpread = */ false,
                  /* isSameRealm = */ true);

  // Load argc.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Load the callee and ensure it is an object
  ValOperandId calleeValId =
      writer.loadArgumentFixedSlot(ArgumentKind::Callee, argc_, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);

  // Ensure the callee is this Wasm function.
  emitCalleeGuard(calleeObjId, calleeFunc);

  // Guard the argument types.
  uint32_t guardedArgs = std::min<uint32_t>(sig.args().length(), argc_);
  for (uint32_t i = 0; i < guardedArgs; i++) {
    ArgumentKind argKind = ArgumentKindForArgIndex(i);
    ValOperandId argId = writer.loadArgumentFixedSlot(argKind, argc_, flags);
    writer.guardWasmArg(argId, sig.args()[i].kind());
  }

  writer.callWasmFunction(calleeObjId, argcId, flags, &funcExport,
                          inst.object());
  writer.returnFromIC();

  trackAttached("WasmCall");

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachInlinableNative(
    HandleFunction callee) {
  MOZ_ASSERT(mode_ == ICState::Mode::Specialized);
  MOZ_ASSERT(callee->isNativeWithoutJitEntry());

  // Special case functions are only optimized for normal calls.
  if (op_ != JSOp::Call && op_ != JSOp::New && op_ != JSOp::CallIgnoresRv &&
      op_ != JSOp::SpreadCall) {
    return AttachDecision::NoAction;
  }

  if (!callee->hasJitInfo() ||
      callee->jitInfo()->type() != JSJitInfo::InlinableNative) {
    return AttachDecision::NoAction;
  }

  InlinableNative native = callee->jitInfo()->inlinableNative;

  // Not all natives can be inlined cross-realm.
  if (cx_->realm() != callee->realm() && !CanInlineNativeCrossRealm(native)) {
    return AttachDecision::NoAction;
  }

  // Check for special-cased native constructors.
  if (op_ == JSOp::New) {
    // newTarget must match the callee. CacheIR for this is emitted in
    // emitNativeCalleeGuard.
    if (callee_ != newTarget_) {
      return AttachDecision::NoAction;
    }
    switch (native) {
      case InlinableNative::Array:
        return tryAttachArrayConstructor(callee);
      case InlinableNative::TypedArrayConstructor:
        return tryAttachTypedArrayConstructor(callee);
      case InlinableNative::String:
        return tryAttachStringConstructor(callee);
      default:
        break;
    }
    return AttachDecision::NoAction;
  }

  // Check for special-cased native spread calls.
  if (op_ == JSOp::SpreadCall) {
    switch (native) {
      case InlinableNative::MathMin:
        return tryAttachSpreadMathMinMax(callee, /*isMax = */ false);
      case InlinableNative::MathMax:
        return tryAttachSpreadMathMinMax(callee, /*isMax = */ true);
      default:
        break;
    }
    return AttachDecision::NoAction;
  }

  // Check for special-cased native functions.
  switch (native) {
    // Array natives.
    case InlinableNative::Array:
      return tryAttachArrayConstructor(callee);
    case InlinableNative::ArrayPush:
      return tryAttachArrayPush(callee);
    case InlinableNative::ArrayPop:
    case InlinableNative::ArrayShift:
      return tryAttachArrayPopShift(callee, native);
    case InlinableNative::ArrayJoin:
      return tryAttachArrayJoin(callee);
    case InlinableNative::ArraySlice:
      return tryAttachArraySlice(callee);
    case InlinableNative::ArrayIsArray:
      return tryAttachArrayIsArray(callee);

    // DataView natives.
    case InlinableNative::DataViewGetInt8:
      return tryAttachDataViewGet(callee, Scalar::Int8);
    case InlinableNative::DataViewGetUint8:
      return tryAttachDataViewGet(callee, Scalar::Uint8);
    case InlinableNative::DataViewGetInt16:
      return tryAttachDataViewGet(callee, Scalar::Int16);
    case InlinableNative::DataViewGetUint16:
      return tryAttachDataViewGet(callee, Scalar::Uint16);
    case InlinableNative::DataViewGetInt32:
      return tryAttachDataViewGet(callee, Scalar::Int32);
    case InlinableNative::DataViewGetUint32:
      return tryAttachDataViewGet(callee, Scalar::Uint32);
    case InlinableNative::DataViewGetFloat32:
      return tryAttachDataViewGet(callee, Scalar::Float32);
    case InlinableNative::DataViewGetFloat64:
      return tryAttachDataViewGet(callee, Scalar::Float64);
    case InlinableNative::DataViewGetBigInt64:
      return tryAttachDataViewGet(callee, Scalar::BigInt64);
    case InlinableNative::DataViewGetBigUint64:
      return tryAttachDataViewGet(callee, Scalar::BigUint64);
    case InlinableNative::DataViewSetInt8:
      return tryAttachDataViewSet(callee, Scalar::Int8);
    case InlinableNative::DataViewSetUint8:
      return tryAttachDataViewSet(callee, Scalar::Uint8);
    case InlinableNative::DataViewSetInt16:
      return tryAttachDataViewSet(callee, Scalar::Int16);
    case InlinableNative::DataViewSetUint16:
      return tryAttachDataViewSet(callee, Scalar::Uint16);
    case InlinableNative::DataViewSetInt32:
      return tryAttachDataViewSet(callee, Scalar::Int32);
    case InlinableNative::DataViewSetUint32:
      return tryAttachDataViewSet(callee, Scalar::Uint32);
    case InlinableNative::DataViewSetFloat32:
      return tryAttachDataViewSet(callee, Scalar::Float32);
    case InlinableNative::DataViewSetFloat64:
      return tryAttachDataViewSet(callee, Scalar::Float64);
    case InlinableNative::DataViewSetBigInt64:
      return tryAttachDataViewSet(callee, Scalar::BigInt64);
    case InlinableNative::DataViewSetBigUint64:
      return tryAttachDataViewSet(callee, Scalar::BigUint64);

    // Intl natives.
    case InlinableNative::IntlGuardToCollator:
    case InlinableNative::IntlGuardToDateTimeFormat:
    case InlinableNative::IntlGuardToDisplayNames:
    case InlinableNative::IntlGuardToListFormat:
    case InlinableNative::IntlGuardToNumberFormat:
    case InlinableNative::IntlGuardToPluralRules:
    case InlinableNative::IntlGuardToRelativeTimeFormat:
      return tryAttachGuardToClass(callee, native);

    // Slot intrinsics.
    case InlinableNative::IntrinsicUnsafeGetReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetObjectFromReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetInt32FromReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetStringFromReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetBooleanFromReservedSlot:
      return tryAttachUnsafeGetReservedSlot(callee, native);
    case InlinableNative::IntrinsicUnsafeSetReservedSlot:
      return tryAttachUnsafeSetReservedSlot(callee);

    // Intrinsics.
    case InlinableNative::IntrinsicIsSuspendedGenerator:
      return tryAttachIsSuspendedGenerator(callee);
    case InlinableNative::IntrinsicToObject:
      return tryAttachToObject(callee, native);
    case InlinableNative::IntrinsicToInteger:
      return tryAttachToInteger(callee);
    case InlinableNative::IntrinsicToLength:
      return tryAttachToLength(callee);
    case InlinableNative::IntrinsicIsObject:
      return tryAttachIsObject(callee);
    case InlinableNative::IntrinsicIsPackedArray:
      return tryAttachIsPackedArray(callee);
    case InlinableNative::IntrinsicIsCallable:
      return tryAttachIsCallable(callee);
    case InlinableNative::IntrinsicIsConstructor:
      return tryAttachIsConstructor(callee);
    case InlinableNative::IntrinsicIsCrossRealmArrayConstructor:
      return tryAttachIsCrossRealmArrayConstructor(callee);
    case InlinableNative::IntrinsicGuardToArrayIterator:
    case InlinableNative::IntrinsicGuardToMapIterator:
    case InlinableNative::IntrinsicGuardToSetIterator:
    case InlinableNative::IntrinsicGuardToStringIterator:
    case InlinableNative::IntrinsicGuardToRegExpStringIterator:
    case InlinableNative::IntrinsicGuardToWrapForValidIterator:
    case InlinableNative::IntrinsicGuardToIteratorHelper:
    case InlinableNative::IntrinsicGuardToAsyncIteratorHelper:
      return tryAttachGuardToClass(callee, native);
    case InlinableNative::IntrinsicSubstringKernel:
      return tryAttachSubstringKernel(callee);
    case InlinableNative::IntrinsicIsConstructing:
      return tryAttachIsConstructing(callee);
    case InlinableNative::IntrinsicFinishBoundFunctionInit:
      return tryAttachFinishBoundFunctionInit(callee);
    case InlinableNative::IntrinsicNewArrayIterator:
      return tryAttachNewArrayIterator(callee);
    case InlinableNative::IntrinsicNewStringIterator:
      return tryAttachNewStringIterator(callee);
    case InlinableNative::IntrinsicNewRegExpStringIterator:
      return tryAttachNewRegExpStringIterator(callee);
    case InlinableNative::IntrinsicArrayIteratorPrototypeOptimizable:
      return tryAttachArrayIteratorPrototypeOptimizable(callee);
    case InlinableNative::IntrinsicObjectHasPrototype:
      return tryAttachObjectHasPrototype(callee);

    // RegExp natives.
    case InlinableNative::IsRegExpObject:
      return tryAttachHasClass(callee, &RegExpObject::class_,
                               /* isPossiblyWrapped = */ false);
    case InlinableNative::IsPossiblyWrappedRegExpObject:
      return tryAttachHasClass(callee, &RegExpObject::class_,
                               /* isPossiblyWrapped = */ true);
    case InlinableNative::RegExpMatcher:
    case InlinableNative::RegExpSearcher:
    case InlinableNative::RegExpTester:
      return tryAttachRegExpMatcherSearcherTester(callee, native);
    case InlinableNative::RegExpPrototypeOptimizable:
      return tryAttachRegExpPrototypeOptimizable(callee);
    case InlinableNative::RegExpInstanceOptimizable:
      return tryAttachRegExpInstanceOptimizable(callee);
    case InlinableNative::GetFirstDollarIndex:
      return tryAttachGetFirstDollarIndex(callee);

    // String natives.
    case InlinableNative::String:
      return tryAttachString(callee);
    case InlinableNative::StringToString:
    case InlinableNative::StringValueOf:
      return tryAttachStringToStringValueOf(callee);
    case InlinableNative::StringCharCodeAt:
      return tryAttachStringCharCodeAt(callee);
    case InlinableNative::StringCharAt:
      return tryAttachStringCharAt(callee);
    case InlinableNative::StringFromCharCode:
      return tryAttachStringFromCharCode(callee);
    case InlinableNative::StringFromCodePoint:
      return tryAttachStringFromCodePoint(callee);
    case InlinableNative::StringToLowerCase:
      return tryAttachStringToLowerCase(callee);
    case InlinableNative::StringToUpperCase:
      return tryAttachStringToUpperCase(callee);
    case InlinableNative::IntrinsicStringReplaceString:
      return tryAttachStringReplaceString(callee);
    case InlinableNative::IntrinsicStringSplitString:
      return tryAttachStringSplitString(callee);

    // Math natives.
    case InlinableNative::MathRandom:
      return tryAttachMathRandom(callee);
    case InlinableNative::MathAbs:
      return tryAttachMathAbs(callee);
    case InlinableNative::MathClz32:
      return tryAttachMathClz32(callee);
    case InlinableNative::MathSign:
      return tryAttachMathSign(callee);
    case InlinableNative::MathImul:
      return tryAttachMathImul(callee);
    case InlinableNative::MathFloor:
      return tryAttachMathFloor(callee);
    case InlinableNative::MathCeil:
      return tryAttachMathCeil(callee);
    case InlinableNative::MathTrunc:
      return tryAttachMathTrunc(callee);
    case InlinableNative::MathRound:
      return tryAttachMathRound(callee);
    case InlinableNative::MathSqrt:
      return tryAttachMathSqrt(callee);
    case InlinableNative::MathFRound:
      return tryAttachMathFRound(callee);
    case InlinableNative::MathHypot:
      return tryAttachMathHypot(callee);
    case InlinableNative::MathATan2:
      return tryAttachMathATan2(callee);
    case InlinableNative::MathSin:
      return tryAttachMathFunction(callee, UnaryMathFunction::Sin);
    case InlinableNative::MathTan:
      return tryAttachMathFunction(callee, UnaryMathFunction::Tan);
    case InlinableNative::MathCos:
      return tryAttachMathFunction(callee, UnaryMathFunction::Cos);
    case InlinableNative::MathExp:
      return tryAttachMathFunction(callee, UnaryMathFunction::Exp);
    case InlinableNative::MathLog:
      return tryAttachMathFunction(callee, UnaryMathFunction::Log);
    case InlinableNative::MathASin:
      return tryAttachMathFunction(callee, UnaryMathFunction::ASin);
    case InlinableNative::MathATan:
      return tryAttachMathFunction(callee, UnaryMathFunction::ATan);
    case InlinableNative::MathACos:
      return tryAttachMathFunction(callee, UnaryMathFunction::ACos);
    case InlinableNative::MathLog10:
      return tryAttachMathFunction(callee, UnaryMathFunction::Log10);
    case InlinableNative::MathLog2:
      return tryAttachMathFunction(callee, UnaryMathFunction::Log2);
    case InlinableNative::MathLog1P:
      return tryAttachMathFunction(callee, UnaryMathFunction::Log1P);
    case InlinableNative::MathExpM1:
      return tryAttachMathFunction(callee, UnaryMathFunction::ExpM1);
    case InlinableNative::MathCosH:
      return tryAttachMathFunction(callee, UnaryMathFunction::CosH);
    case InlinableNative::MathSinH:
      return tryAttachMathFunction(callee, UnaryMathFunction::SinH);
    case InlinableNative::MathTanH:
      return tryAttachMathFunction(callee, UnaryMathFunction::TanH);
    case InlinableNative::MathACosH:
      return tryAttachMathFunction(callee, UnaryMathFunction::ACosH);
    case InlinableNative::MathASinH:
      return tryAttachMathFunction(callee, UnaryMathFunction::ASinH);
    case InlinableNative::MathATanH:
      return tryAttachMathFunction(callee, UnaryMathFunction::ATanH);
    case InlinableNative::MathCbrt:
      return tryAttachMathFunction(callee, UnaryMathFunction::Cbrt);
    case InlinableNative::MathPow:
      return tryAttachMathPow(callee);
    case InlinableNative::MathMin:
      return tryAttachMathMinMax(callee, /* isMax = */ false);
    case InlinableNative::MathMax:
      return tryAttachMathMinMax(callee, /* isMax = */ true);

    // Map intrinsics.
    case InlinableNative::IntrinsicGuardToMapObject:
      return tryAttachGuardToClass(callee, native);
    case InlinableNative::IntrinsicGetNextMapEntryForIterator:
      return tryAttachGetNextMapSetEntryForIterator(callee, /* isMap = */ true);

    // Number natives.
    case InlinableNative::NumberToString:
      return tryAttachNumberToString(callee);

    // Object natives.
    case InlinableNative::Object:
      return tryAttachToObject(callee, native);
    case InlinableNative::ObjectCreate:
      return tryAttachObjectCreate(callee);
    case InlinableNative::ObjectIs:
      return tryAttachObjectIs(callee);
    case InlinableNative::ObjectIsPrototypeOf:
      return tryAttachObjectIsPrototypeOf(callee);
    case InlinableNative::ObjectToString:
      return tryAttachObjectToString(callee);

    // Set intrinsics.
    case InlinableNative::IntrinsicGuardToSetObject:
      return tryAttachGuardToClass(callee, native);
    case InlinableNative::IntrinsicGetNextSetEntryForIterator:
      return tryAttachGetNextMapSetEntryForIterator(callee,
                                                    /* isMap = */ false);

    // ArrayBuffer intrinsics.
    case InlinableNative::IntrinsicGuardToArrayBuffer:
      return tryAttachGuardToClass(callee, native);
    case InlinableNative::IntrinsicArrayBufferByteLength:
      return tryAttachArrayBufferByteLength(callee,
                                            /* isPossiblyWrapped = */ false);
    case InlinableNative::IntrinsicPossiblyWrappedArrayBufferByteLength:
      return tryAttachArrayBufferByteLength(callee,
                                            /* isPossiblyWrapped = */ true);

    // SharedArrayBuffer intrinsics.
    case InlinableNative::IntrinsicGuardToSharedArrayBuffer:
      return tryAttachGuardToClass(callee, native);

    // TypedArray intrinsics.
    case InlinableNative::TypedArrayConstructor:
      return AttachDecision::NoAction;  // Not callable.
    case InlinableNative::IntrinsicIsTypedArray:
      return tryAttachIsTypedArray(callee, /* isPossiblyWrapped = */ false);
    case InlinableNative::IntrinsicIsPossiblyWrappedTypedArray:
      return tryAttachIsTypedArray(callee, /* isPossiblyWrapped = */ true);
    case InlinableNative::IntrinsicIsTypedArrayConstructor:
      return tryAttachIsTypedArrayConstructor(callee);
    case InlinableNative::IntrinsicTypedArrayByteOffset:
      return tryAttachTypedArrayByteOffset(callee);
    case InlinableNative::IntrinsicTypedArrayElementSize:
      return tryAttachTypedArrayElementSize(callee);
    case InlinableNative::IntrinsicTypedArrayLength:
      return tryAttachTypedArrayLength(callee, /* isPossiblyWrapped = */ false);
    case InlinableNative::IntrinsicPossiblyWrappedTypedArrayLength:
      return tryAttachTypedArrayLength(callee, /* isPossiblyWrapped = */ true);

    // Reflect natives.
    case InlinableNative::ReflectGetPrototypeOf:
      return tryAttachReflectGetPrototypeOf(callee);

    // Atomics intrinsics:
    case InlinableNative::AtomicsCompareExchange:
      return tryAttachAtomicsCompareExchange(callee);
    case InlinableNative::AtomicsExchange:
      return tryAttachAtomicsExchange(callee);
    case InlinableNative::AtomicsAdd:
      return tryAttachAtomicsAdd(callee);
    case InlinableNative::AtomicsSub:
      return tryAttachAtomicsSub(callee);
    case InlinableNative::AtomicsAnd:
      return tryAttachAtomicsAnd(callee);
    case InlinableNative::AtomicsOr:
      return tryAttachAtomicsOr(callee);
    case InlinableNative::AtomicsXor:
      return tryAttachAtomicsXor(callee);
    case InlinableNative::AtomicsLoad:
      return tryAttachAtomicsLoad(callee);
    case InlinableNative::AtomicsStore:
      return tryAttachAtomicsStore(callee);
    case InlinableNative::AtomicsIsLockFree:
      return tryAttachAtomicsIsLockFree(callee);

    // BigInt natives.
    case InlinableNative::BigIntAsIntN:
      return tryAttachBigIntAsIntN(callee);
    case InlinableNative::BigIntAsUintN:
      return tryAttachBigIntAsUintN(callee);

    // Boolean natives.
    case InlinableNative::Boolean:
      return tryAttachBoolean(callee);

    // Testing functions.
    case InlinableNative::TestBailout:
      return tryAttachBailout(callee);
    case InlinableNative::TestAssertFloat32:
      return tryAttachAssertFloat32(callee);
    case InlinableNative::TestAssertRecoveredOnBailout:
      return tryAttachAssertRecoveredOnBailout(callee);

    case InlinableNative::Limit:
      break;
  }

  MOZ_CRASH("Shouldn't get here");
}

// Remember the template object associated with any script being called
// as a constructor, for later use during Ion compilation.
ScriptedThisResult CallIRGenerator::getThisForScripted(
    HandleFunction calleeFunc, MutableHandleObject result) {
  // Some constructors allocate their own |this| object.
  if (calleeFunc->constructorNeedsUninitializedThis()) {
    return ScriptedThisResult::UninitializedThis;
  }

  // Only attach a stub if the newTarget is a function with a
  // nonconfigurable prototype.
  RootedValue protov(cx_);
  RootedObject newTarget(cx_, &newTarget_.toObject());
  if (!newTarget->is<JSFunction>() ||
      !newTarget->as<JSFunction>().hasNonConfigurablePrototypeDataProperty()) {
    return ScriptedThisResult::NoAction;
  }

  if (!GetProperty(cx_, newTarget, newTarget, cx_->names().prototype,
                   &protov)) {
    cx_->clearPendingException();
    return ScriptedThisResult::NoAction;
  }

  if (!protov.isObject()) {
    return ScriptedThisResult::NoAction;
  }

  AutoRealm ar(cx_, calleeFunc);
  PlainObject* thisObject =
      CreateThisForFunction(cx_, calleeFunc, newTarget, TenuredObject);
  if (!thisObject) {
    cx_->clearPendingException();
    return ScriptedThisResult::NoAction;
  }

  MOZ_ASSERT(thisObject->nonCCWRealm() == calleeFunc->realm());
  result.set(thisObject);
  return ScriptedThisResult::TemplateObject;
}

AttachDecision CallIRGenerator::tryAttachCallScripted(
    HandleFunction calleeFunc) {
  MOZ_ASSERT(calleeFunc->hasJitEntry());

  if (calleeFunc->isWasmWithJitEntry()) {
    TRY_ATTACH(tryAttachWasmCall(calleeFunc));
  }

  bool isSpecialized = mode_ == ICState::Mode::Specialized;

  bool isConstructing = IsConstructPC(pc_);
  bool isSpread = IsSpreadPC(pc_);
  bool isSameRealm = isSpecialized && cx_->realm() == calleeFunc->realm();
  CallFlags flags(isConstructing, isSpread, isSameRealm);

  // If callee is not an interpreted constructor, we have to throw.
  if (isConstructing && !calleeFunc->isConstructor()) {
    return AttachDecision::NoAction;
  }

  // Likewise, if the callee is a class constructor, we have to throw.
  if (!isConstructing && calleeFunc->isClassConstructor()) {
    return AttachDecision::NoAction;
  }

  if (isConstructing && !calleeFunc->hasJitScript()) {
    // If we're constructing, require the callee to have a JitScript. This isn't
    // required for correctness but avoids allocating a template object below
    // for constructors that aren't hot. See bug 1419758.
    return AttachDecision::TemporarilyUnoptimizable;
  }

  // Verify that spread calls have a reasonable number of arguments.
  if (isSpread && args_.length() > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  RootedObject templateObj(cx_);
  if (isConstructing && isSpecialized) {
    switch (getThisForScripted(calleeFunc, &templateObj)) {
      case ScriptedThisResult::TemplateObject:
        break;
      case ScriptedThisResult::UninitializedThis:
        flags.setNeedsUninitializedThis();
        break;
      case ScriptedThisResult::NoAction:
        return AttachDecision::NoAction;
    }
  }

  // Load argc.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Load the callee and ensure it is an object
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);

  if (isSpecialized) {
    MOZ_ASSERT_IF(isConstructing,
                  templateObj || flags.needsUninitializedThis());

    // Ensure callee matches this stub's callee
    emitCalleeGuard(calleeObjId, calleeFunc);
    if (templateObj) {
      // Emit guards to ensure the newTarget's .prototype property is what we
      // expect. Note that getThisForScripted checked newTarget is a function
      // with a non-configurable .prototype data property.
      JSFunction* newTarget = &newTarget_.toObject().as<JSFunction>();
      Maybe<PropertyInfo> prop = newTarget->lookupPure(cx_->names().prototype);
      MOZ_ASSERT(prop.isSome());
      MOZ_ASSERT(newTarget->numFixedSlots() == 0, "Stub code relies on this");
      uint32_t slot = prop->slot();
      JSObject* prototypeObject = &newTarget->getSlot(slot).toObject();

      ValOperandId newTargetValId = writer.loadArgumentDynamicSlot(
          ArgumentKind::NewTarget, argcId, flags);
      ObjOperandId newTargetObjId = writer.guardToObject(newTargetValId);
      writer.guardShape(newTargetObjId, newTarget->shape());
      ObjOperandId protoId = writer.loadObject(prototypeObject);
      writer.guardDynamicSlotIsSpecificObject(newTargetObjId, protoId, slot);

      // Call metaScriptedTemplateObject before emitting the call, so that Warp
      // can use this template object before transpiling the call.
      writer.metaScriptedTemplateObject(calleeFunc, templateObj);
    }
  } else {
    // Guard that object is a scripted function
    writer.guardClass(calleeObjId, GuardClassKind::JSFunction);
    writer.guardFunctionHasJitEntry(calleeObjId, isConstructing);

    if (isConstructing) {
      // If callee is not a constructor, we have to throw.
      writer.guardFunctionIsConstructor(calleeObjId);
    } else {
      // If callee is a class constructor, we have to throw.
      writer.guardNotClassConstructor(calleeObjId);
    }
  }

  writer.callScriptedFunction(calleeObjId, argcId, flags);
  writer.returnFromIC();

  if (isSpecialized) {
    trackAttached("Call scripted func");
  } else {
    trackAttached("Call any scripted func");
  }

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachCallNative(HandleFunction calleeFunc) {
  MOZ_ASSERT(calleeFunc->isNativeWithoutJitEntry());

  bool isSpecialized = mode_ == ICState::Mode::Specialized;

  bool isSpread = IsSpreadPC(pc_);
  bool isSameRealm = isSpecialized && cx_->realm() == calleeFunc->realm();
  bool isConstructing = IsConstructPC(pc_);
  CallFlags flags(isConstructing, isSpread, isSameRealm);

  if (isConstructing && !calleeFunc->isConstructor()) {
    return AttachDecision::NoAction;
  }

  // Verify that spread calls have a reasonable number of arguments.
  if (isSpread && args_.length() > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  // Check for specific native-function optimizations.
  if (isSpecialized) {
    TRY_ATTACH(tryAttachInlinableNative(calleeFunc));
  }

  // Load argc.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Load the callee and ensure it is an object
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);

  // DOM calls need an additional guard so only try optimizing the first stub.
  // Can only optimize normal (non-spread) calls.
  if (isFirstStub_ && !isSpread && thisval_.isObject() &&
      CanAttachDOMCall(cx_, JSJitInfo::Method, &thisval_.toObject(), calleeFunc,
                       mode_)) {
    MOZ_ASSERT(!isConstructing, "DOM functions are not constructors");

    // Guard that |this| is an object.
    ValOperandId thisValId =
        writer.loadArgumentDynamicSlot(ArgumentKind::This, argcId, flags);
    ObjOperandId thisObjId = writer.guardToObject(thisValId);

    // Guard on the |this| class to make sure it's the right instance.
    writer.guardAnyClass(thisObjId, thisval_.toObject().getClass());

    // Ensure callee matches this stub's callee
    writer.guardSpecificFunction(calleeObjId, calleeFunc);
    writer.callDOMFunction(calleeObjId, argcId, thisObjId, calleeFunc, flags);

    trackAttached("CallDOM");
  } else if (isSpecialized) {
    // Ensure callee matches this stub's callee
    writer.guardSpecificFunction(calleeObjId, calleeFunc);
    writer.callNativeFunction(calleeObjId, argcId, op_, calleeFunc, flags);

    trackAttached("CallNative");
  } else {
    // Guard that object is a native function
    writer.guardClass(calleeObjId, GuardClassKind::JSFunction);
    writer.guardFunctionHasNoJitEntry(calleeObjId);

    if (isConstructing) {
      // If callee is not a constructor, we have to throw.
      writer.guardFunctionIsConstructor(calleeObjId);
    } else {
      // If callee is a class constructor, we have to throw.
      writer.guardNotClassConstructor(calleeObjId);
    }
    writer.callAnyNativeFunction(calleeObjId, argcId, flags);

    trackAttached("CallAnyNative");
  }

  writer.returnFromIC();

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachCallHook(HandleObject calleeObj) {
  if (op_ == JSOp::FunCall || op_ == JSOp::FunApply) {
    return AttachDecision::NoAction;
  }

  if (mode_ != ICState::Mode::Specialized) {
    // We do not have megamorphic call hook stubs.
    // TODO: Should we attach specialized call hook stubs in
    // megamorphic mode to avoid going generic?
    return AttachDecision::NoAction;
  }

  bool isSpread = IsSpreadPC(pc_);
  bool isConstructing = IsConstructPC(pc_);
  CallFlags flags(isConstructing, isSpread);
  JSNative hook =
      isConstructing ? calleeObj->constructHook() : calleeObj->callHook();
  if (!hook) {
    return AttachDecision::NoAction;
  }

  // Verify that spread calls have a reasonable number of arguments.
  if (isSpread && args_.length() > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  // Load argc.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Load the callee and ensure it is an object
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);

  // Ensure the callee's class matches the one in this stub.
  writer.guardAnyClass(calleeObjId, calleeObj->getClass());

  writer.callClassHook(calleeObjId, argcId, hook, flags);
  writer.returnFromIC();

  trackAttached("Call native func");

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  // Some opcodes are not yet supported.
  switch (op_) {
    case JSOp::Call:
    case JSOp::CallIgnoresRv:
    case JSOp::CallIter:
    case JSOp::SpreadCall:
    case JSOp::New:
    case JSOp::SpreadNew:
    case JSOp::SuperCall:
    case JSOp::SpreadSuperCall:
    case JSOp::FunCall:
    case JSOp::FunApply:
      break;
    default:
      return AttachDecision::NoAction;
  }

  MOZ_ASSERT(mode_ != ICState::Mode::Generic);

  // Ensure callee is a function.
  if (!callee_.isObject()) {
    return AttachDecision::NoAction;
  }

  RootedObject calleeObj(cx_, &callee_.toObject());
  if (!calleeObj->is<JSFunction>()) {
    return tryAttachCallHook(calleeObj);
  }

  HandleFunction calleeFunc = calleeObj.as<JSFunction>();

  if (op_ == JSOp::FunCall) {
    return tryAttachFunCall(calleeFunc);
  }
  if (op_ == JSOp::FunApply) {
    return tryAttachFunApply(calleeFunc);
  }

  // Check for scripted optimizations.
  if (calleeFunc->hasJitEntry()) {
    return tryAttachCallScripted(calleeFunc);
  }

  // Check for native-function optimizations.
  MOZ_ASSERT(calleeFunc->isNativeWithoutJitEntry());

  return tryAttachCallNative(calleeFunc);
}

void CallIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("callee", callee_);
    sp.valueProperty("thisval", thisval_);
    sp.valueProperty("argc", Int32Value(argc_));

    // Try to log the first two arguments.
    if (args_.length() >= 1) {
      sp.valueProperty("arg0", args_[0]);
    }
    if (args_.length() >= 2) {
      sp.valueProperty("arg1", args_[1]);
    }
  }
#endif
}

// Class which holds a shape pointer for use when caches might reference data in
// other zones.
static const JSClass shapeContainerClass = {"ShapeContainer",
                                            JSCLASS_HAS_RESERVED_SLOTS(1)};

static const size_t SHAPE_CONTAINER_SLOT = 0;

JSObject* jit::NewWrapperWithObjectShape(JSContext* cx,
                                         HandleNativeObject obj) {
  MOZ_ASSERT(cx->compartment() != obj->compartment());

  RootedObject wrapper(cx);
  {
    AutoRealm ar(cx, obj);
    wrapper = NewBuiltinClassInstance(cx, &shapeContainerClass);
    if (!wrapper) {
      return nullptr;
    }
    wrapper->as<NativeObject>().setSlot(SHAPE_CONTAINER_SLOT,
                                        PrivateGCThingValue(obj->shape()));
  }
  if (!JS_WrapObject(cx, &wrapper)) {
    return nullptr;
  }
  MOZ_ASSERT(IsWrapper(wrapper));
  return wrapper;
}

void jit::LoadShapeWrapperContents(MacroAssembler& masm, Register obj,
                                   Register dst, Label* failure) {
  masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), dst);
  Address privateAddr(dst,
                      js::detail::ProxyReservedSlots::offsetOfPrivateSlot());
  masm.fallibleUnboxObject(privateAddr, dst, failure);
  masm.unboxNonDouble(
      Address(dst, NativeObject::getFixedSlotOffset(SHAPE_CONTAINER_SLOT)), dst,
      JSVAL_TYPE_PRIVATE_GCTHING);
}

CompareIRGenerator::CompareIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, ICState state, JSOp op,
                                       HandleValue lhsVal, HandleValue rhsVal)
    : IRGenerator(cx, script, pc, CacheKind::Compare, state),
      op_(op),
      lhsVal_(lhsVal),
      rhsVal_(rhsVal) {}

AttachDecision CompareIRGenerator::tryAttachString(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  if (!lhsVal_.isString() || !rhsVal_.isString()) {
    return AttachDecision::NoAction;
  }

  StringOperandId lhsStrId = writer.guardToString(lhsId);
  StringOperandId rhsStrId = writer.guardToString(rhsId);
  writer.compareStringResult(op_, lhsStrId, rhsStrId);
  writer.returnFromIC();

  trackAttached("String");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachObject(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  if (!lhsVal_.isObject() || !rhsVal_.isObject()) {
    return AttachDecision::NoAction;
  }

  ObjOperandId lhsObjId = writer.guardToObject(lhsId);
  ObjOperandId rhsObjId = writer.guardToObject(rhsId);
  writer.compareObjectResult(op_, lhsObjId, rhsObjId);
  writer.returnFromIC();

  trackAttached("Object");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachSymbol(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  if (!lhsVal_.isSymbol() || !rhsVal_.isSymbol()) {
    return AttachDecision::NoAction;
  }

  SymbolOperandId lhsSymId = writer.guardToSymbol(lhsId);
  SymbolOperandId rhsSymId = writer.guardToSymbol(rhsId);
  writer.compareSymbolResult(op_, lhsSymId, rhsSymId);
  writer.returnFromIC();

  trackAttached("Symbol");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachStrictDifferentTypes(
    ValOperandId lhsId, ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  if (op_ != JSOp::StrictEq && op_ != JSOp::StrictNe) {
    return AttachDecision::NoAction;
  }

  // Probably can't hit some of these.
  if (SameType(lhsVal_, rhsVal_) ||
      (lhsVal_.isNumber() && rhsVal_.isNumber())) {
    return AttachDecision::NoAction;
  }

  // Compare tags
  ValueTagOperandId lhsTypeId = writer.loadValueTag(lhsId);
  ValueTagOperandId rhsTypeId = writer.loadValueTag(rhsId);
  writer.guardTagNotEqual(lhsTypeId, rhsTypeId);

  // Now that we've passed the guard, we know differing types, so return the
  // bool result.
  writer.loadBooleanResult(op_ == JSOp::StrictNe ? true : false);
  writer.returnFromIC();

  trackAttached("StrictDifferentTypes");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachInt32(ValOperandId lhsId,
                                                  ValOperandId rhsId) {
  if ((!lhsVal_.isInt32() && !lhsVal_.isBoolean()) ||
      (!rhsVal_.isInt32() && !rhsVal_.isBoolean())) {
    return AttachDecision::NoAction;
  }

  Int32OperandId lhsIntId = lhsVal_.isBoolean()
                                ? writer.guardBooleanToInt32(lhsId)
                                : writer.guardToInt32(lhsId);
  Int32OperandId rhsIntId = rhsVal_.isBoolean()
                                ? writer.guardBooleanToInt32(rhsId)
                                : writer.guardToInt32(rhsId);

  // Strictly different types should have been handed by
  // tryAttachStrictDifferentTypes
  MOZ_ASSERT_IF(op_ == JSOp::StrictEq || op_ == JSOp::StrictNe,
                lhsVal_.isInt32() == rhsVal_.isInt32());

  writer.compareInt32Result(op_, lhsIntId, rhsIntId);
  writer.returnFromIC();

  trackAttached(lhsVal_.isBoolean() ? "Boolean" : "Int32");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachNumber(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  if (!lhsVal_.isNumber() || !rhsVal_.isNumber()) {
    return AttachDecision::NoAction;
  }

  NumberOperandId lhs = writer.guardIsNumber(lhsId);
  NumberOperandId rhs = writer.guardIsNumber(rhsId);
  writer.compareDoubleResult(op_, lhs, rhs);
  writer.returnFromIC();

  trackAttached("Number");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachBigInt(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  if (!lhsVal_.isBigInt() || !rhsVal_.isBigInt()) {
    return AttachDecision::NoAction;
  }

  BigIntOperandId lhs = writer.guardToBigInt(lhsId);
  BigIntOperandId rhs = writer.guardToBigInt(rhsId);

  writer.compareBigIntResult(op_, lhs, rhs);
  writer.returnFromIC();

  trackAttached("BigInt");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachNumberUndefined(
    ValOperandId lhsId, ValOperandId rhsId) {
  if (!(lhsVal_.isUndefined() && rhsVal_.isNumber()) &&
      !(rhsVal_.isUndefined() && lhsVal_.isNumber())) {
    return AttachDecision::NoAction;
  }

  // Should have been handled by tryAttachAnyNullUndefined.
  MOZ_ASSERT(!IsEqualityOp(op_));

  if (lhsVal_.isNumber()) {
    writer.guardIsNumber(lhsId);
  } else {
    writer.guardIsUndefined(lhsId);
  }

  if (rhsVal_.isNumber()) {
    writer.guardIsNumber(rhsId);
  } else {
    writer.guardIsUndefined(rhsId);
  }

  // Relational comparing a number with undefined will always be false.
  writer.loadBooleanResult(false);
  writer.returnFromIC();

  trackAttached("NumberUndefined");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachAnyNullUndefined(
    ValOperandId lhsId, ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  // Either RHS or LHS needs to be null/undefined.
  if (!lhsVal_.isNullOrUndefined() && !rhsVal_.isNullOrUndefined()) {
    return AttachDecision::NoAction;
  }

  // We assume that the side with null/undefined is usually constant, in
  // code like `if (x === undefined) { x = {}; }`.
  // That is why we don't attach when both sides are undefined/null,
  // because we would basically need to decide by chance which side is
  // the likely constant.
  // The actual generated code however handles null/undefined of course.
  if (lhsVal_.isNullOrUndefined() && rhsVal_.isNullOrUndefined()) {
    return AttachDecision::NoAction;
  }

  if (rhsVal_.isNullOrUndefined()) {
    if (rhsVal_.isNull()) {
      writer.guardIsNull(rhsId);
      writer.compareNullUndefinedResult(op_, /* isUndefined */ false, lhsId);
      trackAttached("AnyNull");
    } else {
      writer.guardIsUndefined(rhsId);
      writer.compareNullUndefinedResult(op_, /* isUndefined */ true, lhsId);
      trackAttached("AnyUndefined");
    }
  } else {
    if (lhsVal_.isNull()) {
      writer.guardIsNull(lhsId);
      writer.compareNullUndefinedResult(op_, /* isUndefined */ false, rhsId);
      trackAttached("NullAny");
    } else {
      writer.guardIsUndefined(lhsId);
      writer.compareNullUndefinedResult(op_, /* isUndefined */ true, rhsId);
      trackAttached("UndefinedAny");
    }
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

// Handle {null/undefined} x {null,undefined} equality comparisons
AttachDecision CompareIRGenerator::tryAttachNullUndefined(ValOperandId lhsId,
                                                          ValOperandId rhsId) {
  if (!lhsVal_.isNullOrUndefined() || !rhsVal_.isNullOrUndefined()) {
    return AttachDecision::NoAction;
  }

  if (op_ == JSOp::Eq || op_ == JSOp::Ne) {
    writer.guardIsNullOrUndefined(lhsId);
    writer.guardIsNullOrUndefined(rhsId);
    // Sloppy equality means we actually only care about the op:
    writer.loadBooleanResult(op_ == JSOp::Eq);
    trackAttached("SloppyNullUndefined");
  } else {
    // Strict equality only hits this branch, and only in the
    // undef {!,=}==  undef and null {!,=}== null cases.
    // The other cases should have hit tryAttachStrictDifferentTypes.
    MOZ_ASSERT(lhsVal_.isNull() == rhsVal_.isNull());
    lhsVal_.isNull() ? writer.guardIsNull(lhsId)
                     : writer.guardIsUndefined(lhsId);
    rhsVal_.isNull() ? writer.guardIsNull(rhsId)
                     : writer.guardIsUndefined(rhsId);
    writer.loadBooleanResult(op_ == JSOp::StrictEq);
    trackAttached("StrictNullUndefinedEquality");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachStringNumber(ValOperandId lhsId,
                                                         ValOperandId rhsId) {
  // Ensure String x Number
  if (!(lhsVal_.isString() && rhsVal_.isNumber()) &&
      !(rhsVal_.isString() && lhsVal_.isNumber())) {
    return AttachDecision::NoAction;
  }

  // Case should have been handled by tryAttachStrictDifferentTypes
  MOZ_ASSERT(op_ != JSOp::StrictEq && op_ != JSOp::StrictNe);

  auto createGuards = [&](const Value& v, ValOperandId vId) {
    if (v.isString()) {
      StringOperandId strId = writer.guardToString(vId);
      return writer.guardStringToNumber(strId);
    }
    MOZ_ASSERT(v.isNumber());
    NumberOperandId numId = writer.guardIsNumber(vId);
    return numId;
  };

  NumberOperandId lhsGuardedId = createGuards(lhsVal_, lhsId);
  NumberOperandId rhsGuardedId = createGuards(rhsVal_, rhsId);
  writer.compareDoubleResult(op_, lhsGuardedId, rhsGuardedId);
  writer.returnFromIC();

  trackAttached("StringNumber");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachPrimitiveSymbol(
    ValOperandId lhsId, ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  // The set of primitive cases we want to handle here (excluding null,
  // undefined, and symbol)
  auto isPrimitive = [](const Value& x) {
    return x.isString() || x.isBoolean() || x.isNumber() || x.isBigInt();
  };

  // Ensure Symbol x {String, Bool, Number, BigInt}.
  if (!(lhsVal_.isSymbol() && isPrimitive(rhsVal_)) &&
      !(rhsVal_.isSymbol() && isPrimitive(lhsVal_))) {
    return AttachDecision::NoAction;
  }

  auto guardPrimitive = [&](const Value& v, ValOperandId id) {
    MOZ_ASSERT(isPrimitive(v));
    if (v.isNumber()) {
      writer.guardIsNumber(id);
      return;
    }
    switch (v.extractNonDoubleType()) {
      case JSVAL_TYPE_STRING:
        writer.guardToString(id);
        return;
      case JSVAL_TYPE_BOOLEAN:
        writer.guardToBoolean(id);
        return;
      case JSVAL_TYPE_BIGINT:
        writer.guardToBigInt(id);
        return;
      default:
        MOZ_CRASH("unexpected type");
        return;
    }
  };

  if (lhsVal_.isSymbol()) {
    writer.guardToSymbol(lhsId);
    guardPrimitive(rhsVal_, rhsId);
  } else {
    guardPrimitive(lhsVal_, lhsId);
    writer.guardToSymbol(rhsId);
  }

  // Comparing a primitive with symbol will always be true for Ne/StrictNe, and
  // always be false for other compare ops.
  writer.loadBooleanResult(op_ == JSOp::Ne || op_ == JSOp::StrictNe);
  writer.returnFromIC();

  trackAttached("PrimitiveSymbol");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachBoolStringOrNumber(
    ValOperandId lhsId, ValOperandId rhsId) {
  // Ensure Boolean x {String, Number}.
  if (!(lhsVal_.isBoolean() && (rhsVal_.isString() || rhsVal_.isNumber())) &&
      !(rhsVal_.isBoolean() && (lhsVal_.isString() || lhsVal_.isNumber()))) {
    return AttachDecision::NoAction;
  }

  // Case should have been handled by tryAttachStrictDifferentTypes
  MOZ_ASSERT(op_ != JSOp::StrictEq && op_ != JSOp::StrictNe);

  // Case should have been handled by tryAttachInt32
  MOZ_ASSERT(!lhsVal_.isInt32() && !rhsVal_.isInt32());

  auto createGuards = [&](const Value& v, ValOperandId vId) {
    if (v.isBoolean()) {
      BooleanOperandId boolId = writer.guardToBoolean(vId);
      return writer.booleanToNumber(boolId);
    }
    if (v.isString()) {
      StringOperandId strId = writer.guardToString(vId);
      return writer.guardStringToNumber(strId);
    }
    MOZ_ASSERT(v.isNumber());
    return writer.guardIsNumber(vId);
  };

  NumberOperandId lhsGuardedId = createGuards(lhsVal_, lhsId);
  NumberOperandId rhsGuardedId = createGuards(rhsVal_, rhsId);
  writer.compareDoubleResult(op_, lhsGuardedId, rhsGuardedId);
  writer.returnFromIC();

  trackAttached("BoolStringOrNumber");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachBigIntInt32(ValOperandId lhsId,
                                                        ValOperandId rhsId) {
  // Ensure BigInt x {Int32, Boolean}.
  if (!(lhsVal_.isBigInt() && (rhsVal_.isInt32() || rhsVal_.isBoolean())) &&
      !(rhsVal_.isBigInt() && (lhsVal_.isInt32() || lhsVal_.isBoolean()))) {
    return AttachDecision::NoAction;
  }

  // Case should have been handled by tryAttachStrictDifferentTypes
  MOZ_ASSERT(op_ != JSOp::StrictEq && op_ != JSOp::StrictNe);

  auto createGuards = [&](const Value& v, ValOperandId vId) {
    if (v.isBoolean()) {
      return writer.guardBooleanToInt32(vId);
    }
    MOZ_ASSERT(v.isInt32());
    return writer.guardToInt32(vId);
  };

  if (lhsVal_.isBigInt()) {
    BigIntOperandId bigIntId = writer.guardToBigInt(lhsId);
    Int32OperandId intId = createGuards(rhsVal_, rhsId);

    writer.compareBigIntInt32Result(op_, bigIntId, intId);
  } else {
    Int32OperandId intId = createGuards(lhsVal_, lhsId);
    BigIntOperandId bigIntId = writer.guardToBigInt(rhsId);

    writer.compareBigIntInt32Result(ReverseCompareOp(op_), bigIntId, intId);
  }
  writer.returnFromIC();

  trackAttached("BigIntInt32");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachBigIntNumber(ValOperandId lhsId,
                                                         ValOperandId rhsId) {
  // Ensure BigInt x Number.
  if (!(lhsVal_.isBigInt() && rhsVal_.isNumber()) &&
      !(rhsVal_.isBigInt() && lhsVal_.isNumber())) {
    return AttachDecision::NoAction;
  }

  // Case should have been handled by tryAttachStrictDifferentTypes
  MOZ_ASSERT(op_ != JSOp::StrictEq && op_ != JSOp::StrictNe);

  if (lhsVal_.isBigInt()) {
    BigIntOperandId bigIntId = writer.guardToBigInt(lhsId);
    NumberOperandId numId = writer.guardIsNumber(rhsId);

    writer.compareBigIntNumberResult(op_, bigIntId, numId);
  } else {
    NumberOperandId numId = writer.guardIsNumber(lhsId);
    BigIntOperandId bigIntId = writer.guardToBigInt(rhsId);

    writer.compareBigIntNumberResult(ReverseCompareOp(op_), bigIntId, numId);
  }
  writer.returnFromIC();

  trackAttached("BigIntNumber");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachBigIntString(ValOperandId lhsId,
                                                         ValOperandId rhsId) {
  // Ensure BigInt x String.
  if (!(lhsVal_.isBigInt() && rhsVal_.isString()) &&
      !(rhsVal_.isBigInt() && lhsVal_.isString())) {
    return AttachDecision::NoAction;
  }

  // Case should have been handled by tryAttachStrictDifferentTypes
  MOZ_ASSERT(op_ != JSOp::StrictEq && op_ != JSOp::StrictNe);

  if (lhsVal_.isBigInt()) {
    BigIntOperandId bigIntId = writer.guardToBigInt(lhsId);
    StringOperandId strId = writer.guardToString(rhsId);

    writer.compareBigIntStringResult(op_, bigIntId, strId);
  } else {
    StringOperandId strId = writer.guardToString(lhsId);
    BigIntOperandId bigIntId = writer.guardToBigInt(rhsId);

    writer.compareBigIntStringResult(ReverseCompareOp(op_), bigIntId, strId);
  }
  writer.returnFromIC();

  trackAttached("BigIntString");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::Compare);
  MOZ_ASSERT(IsEqualityOp(op_) || IsRelationalOp(op_));

  AutoAssertNoPendingException aanpe(cx_);

  constexpr uint8_t lhsIndex = 0;
  constexpr uint8_t rhsIndex = 1;

  static_assert(lhsIndex == 0 && rhsIndex == 1,
                "Indexes relied upon by baseline inspector");

  ValOperandId lhsId(writer.setInputOperandId(lhsIndex));
  ValOperandId rhsId(writer.setInputOperandId(rhsIndex));

  // For sloppy equality ops, there are cases this IC does not handle:
  // - {Object} x {String, Symbol, Bool, Number, BigInt}.
  //
  // (The above lists omits the equivalent case {B} x {A} when {A} x {B} is
  // already present.)

  if (IsEqualityOp(op_)) {
    TRY_ATTACH(tryAttachObject(lhsId, rhsId));
    TRY_ATTACH(tryAttachSymbol(lhsId, rhsId));

    // Handles any (non null or undefined) comparison with null/undefined.
    TRY_ATTACH(tryAttachAnyNullUndefined(lhsId, rhsId));

    // This covers -strict- equality/inequality using a type tag check, so
    // catches all different type pairs outside of Numbers, which cannot be
    // checked on tags alone.
    TRY_ATTACH(tryAttachStrictDifferentTypes(lhsId, rhsId));

    TRY_ATTACH(tryAttachNullUndefined(lhsId, rhsId));

    TRY_ATTACH(tryAttachPrimitiveSymbol(lhsId, rhsId));
  }

  // This should preceed the Int32/Number cases to allow
  // them to not concern themselves with handling undefined
  // or null.
  TRY_ATTACH(tryAttachNumberUndefined(lhsId, rhsId));

  // We want these to be last, to allow us to bypass the
  // strictly-different-types cases in the below attachment code
  TRY_ATTACH(tryAttachInt32(lhsId, rhsId));
  TRY_ATTACH(tryAttachNumber(lhsId, rhsId));
  TRY_ATTACH(tryAttachBigInt(lhsId, rhsId));
  TRY_ATTACH(tryAttachString(lhsId, rhsId));

  TRY_ATTACH(tryAttachStringNumber(lhsId, rhsId));
  TRY_ATTACH(tryAttachBoolStringOrNumber(lhsId, rhsId));

  TRY_ATTACH(tryAttachBigIntInt32(lhsId, rhsId));
  TRY_ATTACH(tryAttachBigIntNumber(lhsId, rhsId));
  TRY_ATTACH(tryAttachBigIntString(lhsId, rhsId));

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

void CompareIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("lhs", lhsVal_);
    sp.valueProperty("rhs", rhsVal_);
    sp.opcodeProperty("op", op_);
  }
#endif
}

ToBoolIRGenerator::ToBoolIRGenerator(JSContext* cx, HandleScript script,
                                     jsbytecode* pc, ICState state,
                                     HandleValue val)
    : IRGenerator(cx, script, pc, CacheKind::ToBool, state), val_(val) {}

void ToBoolIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

AttachDecision ToBoolIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  writer.setTypeData(TypeData(JSValueType(val_.type())));

  TRY_ATTACH(tryAttachBool());
  TRY_ATTACH(tryAttachInt32());
  TRY_ATTACH(tryAttachNumber());
  TRY_ATTACH(tryAttachString());
  TRY_ATTACH(tryAttachNullOrUndefined());
  TRY_ATTACH(tryAttachObject());
  TRY_ATTACH(tryAttachSymbol());
  TRY_ATTACH(tryAttachBigInt());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision ToBoolIRGenerator::tryAttachBool() {
  if (!val_.isBoolean()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  writer.guardNonDoubleType(valId, ValueType::Boolean);
  writer.loadOperandResult(valId);
  writer.returnFromIC();
  trackAttached("ToBoolBool");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachInt32() {
  if (!val_.isInt32()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  writer.guardNonDoubleType(valId, ValueType::Int32);
  writer.loadInt32TruthyResult(valId);
  writer.returnFromIC();
  trackAttached("ToBoolInt32");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachNumber() {
  if (!val_.isNumber()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  NumberOperandId numId = writer.guardIsNumber(valId);
  writer.loadDoubleTruthyResult(numId);
  writer.returnFromIC();
  trackAttached("ToBoolNumber");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachSymbol() {
  if (!val_.isSymbol()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  writer.guardNonDoubleType(valId, ValueType::Symbol);
  writer.loadBooleanResult(true);
  writer.returnFromIC();
  trackAttached("ToBoolSymbol");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachString() {
  if (!val_.isString()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  StringOperandId strId = writer.guardToString(valId);
  writer.loadStringTruthyResult(strId);
  writer.returnFromIC();
  trackAttached("ToBoolString");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachNullOrUndefined() {
  if (!val_.isNullOrUndefined()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  writer.guardIsNullOrUndefined(valId);
  writer.loadBooleanResult(false);
  writer.returnFromIC();
  trackAttached("ToBoolNullOrUndefined");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachObject() {
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  ObjOperandId objId = writer.guardToObject(valId);
  writer.loadObjectTruthyResult(objId);
  writer.returnFromIC();
  trackAttached("ToBoolObject");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachBigInt() {
  if (!val_.isBigInt()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  BigIntOperandId bigIntId = writer.guardToBigInt(valId);
  writer.loadBigIntTruthyResult(bigIntId);
  writer.returnFromIC();
  trackAttached("ToBoolBigInt");
  return AttachDecision::Attach;
}

GetIntrinsicIRGenerator::GetIntrinsicIRGenerator(JSContext* cx,
                                                 HandleScript script,
                                                 jsbytecode* pc, ICState state,
                                                 HandleValue val)
    : IRGenerator(cx, script, pc, CacheKind::GetIntrinsic, state), val_(val) {}

void GetIntrinsicIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

AttachDecision GetIntrinsicIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  writer.loadValueResult(val_);
  writer.returnFromIC();
  trackAttached("GetIntrinsic");
  return AttachDecision::Attach;
}

UnaryArithIRGenerator::UnaryArithIRGenerator(JSContext* cx, HandleScript script,
                                             jsbytecode* pc, ICState state,
                                             JSOp op, HandleValue val,
                                             HandleValue res)
    : IRGenerator(cx, script, pc, CacheKind::UnaryArith, state),
      op_(op),
      val_(val),
      res_(res) {}

void UnaryArithIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
    sp.valueProperty("res", res_);
  }
#endif
}

AttachDecision UnaryArithIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  TRY_ATTACH(tryAttachInt32());
  TRY_ATTACH(tryAttachNumber());
  TRY_ATTACH(tryAttachBitwise());
  TRY_ATTACH(tryAttachBigInt());
  TRY_ATTACH(tryAttachStringInt32());
  TRY_ATTACH(tryAttachStringNumber());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision UnaryArithIRGenerator::tryAttachInt32() {
  if (op_ == JSOp::BitNot) {
    return AttachDecision::NoAction;
  }
  if (!val_.isInt32() || !res_.isInt32()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  Int32OperandId intId = writer.guardToInt32(valId);
  switch (op_) {
    case JSOp::Pos:
      writer.loadInt32Result(intId);
      trackAttached("UnaryArith.Int32Pos");
      break;
    case JSOp::Neg:
      writer.int32NegationResult(intId);
      trackAttached("UnaryArith.Int32Neg");
      break;
    case JSOp::Inc:
      writer.int32IncResult(intId);
      trackAttached("UnaryArith.Int32Inc");
      break;
    case JSOp::Dec:
      writer.int32DecResult(intId);
      trackAttached("UnaryArith.Int32Dec");
      break;
    case JSOp::ToNumeric:
      writer.loadInt32Result(intId);
      trackAttached("UnaryArith.Int32ToNumeric");
      break;
    default:
      MOZ_CRASH("unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision UnaryArithIRGenerator::tryAttachNumber() {
  if (op_ == JSOp::BitNot) {
    return AttachDecision::NoAction;
  }
  if (!val_.isNumber()) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(res_.isNumber());

  ValOperandId valId(writer.setInputOperandId(0));
  NumberOperandId numId = writer.guardIsNumber(valId);
  Int32OperandId truncatedId;
  switch (op_) {
    case JSOp::Pos:
      writer.loadDoubleResult(numId);
      trackAttached("UnaryArith.DoublePos");
      break;
    case JSOp::Neg:
      writer.doubleNegationResult(numId);
      trackAttached("UnaryArith.DoubleNeg");
      break;
    case JSOp::Inc:
      writer.doubleIncResult(numId);
      trackAttached("UnaryArith.DoubleInc");
      break;
    case JSOp::Dec:
      writer.doubleDecResult(numId);
      trackAttached("UnaryArith.DoubleDec");
      break;
    case JSOp::ToNumeric:
      writer.loadDoubleResult(numId);
      trackAttached("UnaryArith.DoubleToNumeric");
      break;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

static bool CanTruncateToInt32(const Value& val) {
  return val.isNumber() || val.isBoolean() || val.isNullOrUndefined() ||
         val.isString();
}

// Convert type into int32 for the bitwise/shift operands.
static Int32OperandId EmitTruncateToInt32Guard(CacheIRWriter& writer,
                                               ValOperandId id,
                                               const Value& val) {
  MOZ_ASSERT(CanTruncateToInt32(val));
  if (val.isInt32()) {
    return writer.guardToInt32(id);
  }
  if (val.isBoolean()) {
    return writer.guardBooleanToInt32(id);
  }
  if (val.isNullOrUndefined()) {
    writer.guardIsNullOrUndefined(id);
    return writer.loadInt32Constant(0);
  }
  NumberOperandId numId;
  if (val.isString()) {
    StringOperandId strId = writer.guardToString(id);
    numId = writer.guardStringToNumber(strId);
  } else {
    MOZ_ASSERT(val.isDouble());
    numId = writer.guardIsNumber(id);
  }
  return writer.truncateDoubleToUInt32(numId);
}

AttachDecision UnaryArithIRGenerator::tryAttachBitwise() {
  // Only bitwise operators.
  if (op_ != JSOp::BitNot) {
    return AttachDecision::NoAction;
  }

  // Check guard conditions
  if (!CanTruncateToInt32(val_)) {
    return AttachDecision::NoAction;
  }

  // Bitwise operators always produce Int32 values.
  MOZ_ASSERT(res_.isInt32());

  ValOperandId valId(writer.setInputOperandId(0));
  Int32OperandId intId = EmitTruncateToInt32Guard(writer, valId, val_);
  writer.int32NotResult(intId);
  trackAttached("BinaryArith.Bitwise.BitNot");

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision UnaryArithIRGenerator::tryAttachBigInt() {
  if (!val_.isBigInt()) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(res_.isBigInt());

  MOZ_ASSERT(op_ != JSOp::Pos,
             "Applying the unary + operator on BigInt values throws an error");

  ValOperandId valId(writer.setInputOperandId(0));
  BigIntOperandId bigIntId = writer.guardToBigInt(valId);
  switch (op_) {
    case JSOp::BitNot:
      writer.bigIntNotResult(bigIntId);
      trackAttached("UnaryArith.BigIntNot");
      break;
    case JSOp::Neg:
      writer.bigIntNegationResult(bigIntId);
      trackAttached("UnaryArith.BigIntNeg");
      break;
    case JSOp::Inc:
      writer.bigIntIncResult(bigIntId);
      trackAttached("UnaryArith.BigIntInc");
      break;
    case JSOp::Dec:
      writer.bigIntDecResult(bigIntId);
      trackAttached("UnaryArith.BigIntDec");
      break;
    case JSOp::ToNumeric:
      writer.loadBigIntResult(bigIntId);
      trackAttached("UnaryArith.BigIntToNumeric");
      break;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision UnaryArithIRGenerator::tryAttachStringInt32() {
  if (!val_.isString()) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(res_.isNumber());

  // Case should have been handled by tryAttachBitwise.
  MOZ_ASSERT(op_ != JSOp::BitNot);

  if (!res_.isInt32()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  StringOperandId stringId = writer.guardToString(valId);
  Int32OperandId intId = writer.guardStringToInt32(stringId);

  switch (op_) {
    case JSOp::Pos:
      writer.loadInt32Result(intId);
      trackAttached("UnaryArith.StringInt32Pos");
      break;
    case JSOp::Neg:
      writer.int32NegationResult(intId);
      trackAttached("UnaryArith.StringInt32Neg");
      break;
    case JSOp::Inc:
      writer.int32IncResult(intId);
      trackAttached("UnaryArith.StringInt32Inc");
      break;
    case JSOp::Dec:
      writer.int32DecResult(intId);
      trackAttached("UnaryArith.StringInt32Dec");
      break;
    case JSOp::ToNumeric:
      writer.loadInt32Result(intId);
      trackAttached("UnaryArith.StringInt32ToNumeric");
      break;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision UnaryArithIRGenerator::tryAttachStringNumber() {
  if (!val_.isString()) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(res_.isNumber());

  // Case should have been handled by tryAttachBitwise.
  MOZ_ASSERT(op_ != JSOp::BitNot);

  ValOperandId valId(writer.setInputOperandId(0));
  StringOperandId stringId = writer.guardToString(valId);
  NumberOperandId numId = writer.guardStringToNumber(stringId);

  Int32OperandId truncatedId;
  switch (op_) {
    case JSOp::Pos:
      writer.loadDoubleResult(numId);
      trackAttached("UnaryArith.StringNumberPos");
      break;
    case JSOp::Neg:
      writer.doubleNegationResult(numId);
      trackAttached("UnaryArith.StringNumberNeg");
      break;
    case JSOp::Inc:
      writer.doubleIncResult(numId);
      trackAttached("UnaryArith.StringNumberInc");
      break;
    case JSOp::Dec:
      writer.doubleDecResult(numId);
      trackAttached("UnaryArith.StringNumberDec");
      break;
    case JSOp::ToNumeric:
      writer.loadDoubleResult(numId);
      trackAttached("UnaryArith.StringNumberToNumeric");
      break;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

ToPropertyKeyIRGenerator::ToPropertyKeyIRGenerator(JSContext* cx,
                                                   HandleScript script,
                                                   jsbytecode* pc,
                                                   ICState state,
                                                   HandleValue val)
    : IRGenerator(cx, script, pc, CacheKind::ToPropertyKey, state), val_(val) {}

void ToPropertyKeyIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  TRY_ATTACH(tryAttachInt32());
  TRY_ATTACH(tryAttachNumber());
  TRY_ATTACH(tryAttachString());
  TRY_ATTACH(tryAttachSymbol());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachInt32() {
  if (!val_.isInt32()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  Int32OperandId intId = writer.guardToInt32(valId);
  writer.loadInt32Result(intId);
  writer.returnFromIC();

  trackAttached("ToPropertyKey.Int32");
  return AttachDecision::Attach;
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachNumber() {
  if (!val_.isNumber()) {
    return AttachDecision::NoAction;
  }

  // We allow negative zero here because ToPropertyKey(-0.0) is 0.
  int32_t unused;
  if (!mozilla::NumberEqualsInt32(val_.toNumber(), &unused)) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  Int32OperandId intId = writer.guardToInt32Index(valId);
  writer.loadInt32Result(intId);
  writer.returnFromIC();

  trackAttached("ToPropertyKey.Number");
  return AttachDecision::Attach;
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachString() {
  if (!val_.isString()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  StringOperandId strId = writer.guardToString(valId);
  writer.loadStringResult(strId);
  writer.returnFromIC();

  trackAttached("ToPropertyKey.String");
  return AttachDecision::Attach;
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachSymbol() {
  if (!val_.isSymbol()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  SymbolOperandId strId = writer.guardToSymbol(valId);
  writer.loadSymbolResult(strId);
  writer.returnFromIC();

  trackAttached("ToPropertyKey.Symbol");
  return AttachDecision::Attach;
}

BinaryArithIRGenerator::BinaryArithIRGenerator(JSContext* cx,
                                               HandleScript script,
                                               jsbytecode* pc, ICState state,
                                               JSOp op, HandleValue lhs,
                                               HandleValue rhs, HandleValue res)
    : IRGenerator(cx, script, pc, CacheKind::BinaryArith, state),
      op_(op),
      lhs_(lhs),
      rhs_(rhs),
      res_(res) {}

void BinaryArithIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.opcodeProperty("op", op_);
    sp.valueProperty("rhs", rhs_);
    sp.valueProperty("lhs", lhs_);
  }
#endif
}

AttachDecision BinaryArithIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  // Arithmetic operations with Int32 operands
  TRY_ATTACH(tryAttachInt32());

  // Bitwise operations with Int32/Double/Boolean/Null/Undefined/String
  // operands.
  TRY_ATTACH(tryAttachBitwise());

  // Arithmetic operations with Double operands. This needs to come after
  // tryAttachInt32, as the guards overlap, and we'd prefer to attach the
  // more specialized Int32 IC if it is possible.
  TRY_ATTACH(tryAttachDouble());

  // String x String
  TRY_ATTACH(tryAttachStringConcat());

  // String x Object
  TRY_ATTACH(tryAttachStringObjectConcat());

  TRY_ATTACH(tryAttachStringNumberConcat());

  // String + Boolean
  TRY_ATTACH(tryAttachStringBooleanConcat());

  // Arithmetic operations or bitwise operations with BigInt operands
  TRY_ATTACH(tryAttachBigInt());

  // Arithmetic operations (without addition) with String x Int32.
  TRY_ATTACH(tryAttachStringInt32Arith());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision BinaryArithIRGenerator::tryAttachBitwise() {
  // Only bit-wise and shifts.
  if (op_ != JSOp::BitOr && op_ != JSOp::BitXor && op_ != JSOp::BitAnd &&
      op_ != JSOp::Lsh && op_ != JSOp::Rsh && op_ != JSOp::Ursh) {
    return AttachDecision::NoAction;
  }

  // Check guard conditions
  if (!CanTruncateToInt32(lhs_) || !CanTruncateToInt32(rhs_)) {
    return AttachDecision::NoAction;
  }

  // All ops, with the exception of Ursh, produce Int32 values.
  MOZ_ASSERT_IF(op_ != JSOp::Ursh, res_.isInt32());

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  Int32OperandId lhsIntId = EmitTruncateToInt32Guard(writer, lhsId, lhs_);
  Int32OperandId rhsIntId = EmitTruncateToInt32Guard(writer, rhsId, rhs_);

  switch (op_) {
    case JSOp::BitOr:
      writer.int32BitOrResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Bitwise.BitOr");
      break;
    case JSOp::BitXor:
      writer.int32BitXorResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Bitwise.BitXor");
      break;
    case JSOp::BitAnd:
      writer.int32BitAndResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Bitwise.BitAnd");
      break;
    case JSOp::Lsh:
      writer.int32LeftShiftResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Bitwise.LeftShift");
      break;
    case JSOp::Rsh:
      writer.int32RightShiftResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Bitwise.RightShift");
      break;
    case JSOp::Ursh:
      writer.int32URightShiftResult(lhsIntId, rhsIntId, res_.isDouble());
      trackAttached("BinaryArith.Bitwise.UnsignedRightShift");
      break;
    default:
      MOZ_CRASH("Unhandled op in tryAttachBitwise");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachDouble() {
  // Check valid opcodes
  if (op_ != JSOp::Add && op_ != JSOp::Sub && op_ != JSOp::Mul &&
      op_ != JSOp::Div && op_ != JSOp::Mod && op_ != JSOp::Pow) {
    return AttachDecision::NoAction;
  }

  // Check guard conditions
  if (!lhs_.isNumber() || !rhs_.isNumber()) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  NumberOperandId lhs = writer.guardIsNumber(lhsId);
  NumberOperandId rhs = writer.guardIsNumber(rhsId);

  switch (op_) {
    case JSOp::Add:
      writer.doubleAddResult(lhs, rhs);
      trackAttached("BinaryArith.Double.Add");
      break;
    case JSOp::Sub:
      writer.doubleSubResult(lhs, rhs);
      trackAttached("BinaryArith.Double.Sub");
      break;
    case JSOp::Mul:
      writer.doubleMulResult(lhs, rhs);
      trackAttached("BinaryArith.Double.Mul");
      break;
    case JSOp::Div:
      writer.doubleDivResult(lhs, rhs);
      trackAttached("BinaryArith.Double.Div");
      break;
    case JSOp::Mod:
      writer.doubleModResult(lhs, rhs);
      trackAttached("BinaryArith.Double.Mod");
      break;
    case JSOp::Pow:
      writer.doublePowResult(lhs, rhs);
      trackAttached("BinaryArith.Double.Pow");
      break;
    default:
      MOZ_CRASH("Unhandled Op");
  }
  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachInt32() {
  // Check guard conditions
  if (!(lhs_.isInt32() || lhs_.isBoolean()) ||
      !(rhs_.isInt32() || rhs_.isBoolean())) {
    return AttachDecision::NoAction;
  }

  // These ICs will failure() if result can't be encoded in an Int32:
  // If sample result is not Int32, we should avoid IC.
  if (!res_.isInt32()) {
    return AttachDecision::NoAction;
  }

  if (op_ != JSOp::Add && op_ != JSOp::Sub && op_ != JSOp::Mul &&
      op_ != JSOp::Div && op_ != JSOp::Mod && op_ != JSOp::Pow) {
    return AttachDecision::NoAction;
  }

  if (op_ == JSOp::Pow && !CanAttachInt32Pow(lhs_, rhs_)) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  auto guardToInt32 = [&](ValOperandId id, const Value& v) {
    if (v.isInt32()) {
      return writer.guardToInt32(id);
    }
    MOZ_ASSERT(v.isBoolean());
    return writer.guardBooleanToInt32(id);
  };

  Int32OperandId lhsIntId = guardToInt32(lhsId, lhs_);
  Int32OperandId rhsIntId = guardToInt32(rhsId, rhs_);

  switch (op_) {
    case JSOp::Add:
      writer.int32AddResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32.Add");
      break;
    case JSOp::Sub:
      writer.int32SubResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32.Sub");
      break;
    case JSOp::Mul:
      writer.int32MulResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32.Mul");
      break;
    case JSOp::Div:
      writer.int32DivResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32.Div");
      break;
    case JSOp::Mod:
      writer.int32ModResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32.Mod");
      break;
    case JSOp::Pow:
      writer.int32PowResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32.Pow");
      break;
    default:
      MOZ_CRASH("Unhandled op in tryAttachInt32");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachStringNumberConcat() {
  // Only Addition
  if (op_ != JSOp::Add) {
    return AttachDecision::NoAction;
  }

  if (!(lhs_.isString() && rhs_.isNumber()) &&
      !(lhs_.isNumber() && rhs_.isString())) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  StringOperandId lhsStrId = EmitToStringGuard(writer, lhsId, lhs_);
  StringOperandId rhsStrId = EmitToStringGuard(writer, rhsId, rhs_);

  writer.callStringConcatResult(lhsStrId, rhsStrId);

  writer.returnFromIC();
  trackAttached("BinaryArith.StringNumberConcat");
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachStringBooleanConcat() {
  // Only Addition
  if (op_ != JSOp::Add) {
    return AttachDecision::NoAction;
  }

  if ((!lhs_.isString() || !rhs_.isBoolean()) &&
      (!lhs_.isBoolean() || !rhs_.isString())) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  auto guardToString = [&](ValOperandId id, const Value& v) {
    if (v.isString()) {
      return writer.guardToString(id);
    }
    MOZ_ASSERT(v.isBoolean());
    BooleanOperandId boolId = writer.guardToBoolean(id);
    return writer.booleanToString(boolId);
  };

  StringOperandId lhsStrId = guardToString(lhsId, lhs_);
  StringOperandId rhsStrId = guardToString(rhsId, rhs_);

  writer.callStringConcatResult(lhsStrId, rhsStrId);

  writer.returnFromIC();
  trackAttached("BinaryArith.StringBooleanConcat");
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachStringConcat() {
  // Only Addition
  if (op_ != JSOp::Add) {
    return AttachDecision::NoAction;
  }

  // Check guards
  if (!lhs_.isString() || !rhs_.isString()) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  StringOperandId lhsStrId = writer.guardToString(lhsId);
  StringOperandId rhsStrId = writer.guardToString(rhsId);

  writer.callStringConcatResult(lhsStrId, rhsStrId);

  writer.returnFromIC();
  trackAttached("BinaryArith.StringConcat");
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachStringObjectConcat() {
  // Only Addition
  if (op_ != JSOp::Add) {
    return AttachDecision::NoAction;
  }

  // Check Guards
  if (!(lhs_.isObject() && rhs_.isString()) &&
      !(lhs_.isString() && rhs_.isObject()))
    return AttachDecision::NoAction;

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  // This guard is actually overly tight, as the runtime
  // helper can handle lhs or rhs being a string, so long
  // as the other is an object.
  if (lhs_.isString()) {
    writer.guardToString(lhsId);
    writer.guardToObject(rhsId);
  } else {
    writer.guardToObject(lhsId);
    writer.guardToString(rhsId);
  }

  writer.callStringObjectConcatResult(lhsId, rhsId);

  writer.returnFromIC();
  trackAttached("BinaryArith.StringObjectConcat");
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachBigInt() {
  // Check Guards
  if (!lhs_.isBigInt() || !rhs_.isBigInt()) {
    return AttachDecision::NoAction;
  }

  switch (op_) {
    case JSOp::Add:
    case JSOp::Sub:
    case JSOp::Mul:
    case JSOp::Div:
    case JSOp::Mod:
    case JSOp::Pow:
      // Arithmetic operations.
      break;

    case JSOp::BitOr:
    case JSOp::BitXor:
    case JSOp::BitAnd:
    case JSOp::Lsh:
    case JSOp::Rsh:
      // Bitwise operations.
      break;

    default:
      return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  BigIntOperandId lhsBigIntId = writer.guardToBigInt(lhsId);
  BigIntOperandId rhsBigIntId = writer.guardToBigInt(rhsId);

  switch (op_) {
    case JSOp::Add:
      writer.bigIntAddResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.Add");
      break;
    case JSOp::Sub:
      writer.bigIntSubResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.Sub");
      break;
    case JSOp::Mul:
      writer.bigIntMulResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.Mul");
      break;
    case JSOp::Div:
      writer.bigIntDivResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.Div");
      break;
    case JSOp::Mod:
      writer.bigIntModResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.Mod");
      break;
    case JSOp::Pow:
      writer.bigIntPowResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.Pow");
      break;
    case JSOp::BitOr:
      writer.bigIntBitOrResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.BitOr");
      break;
    case JSOp::BitXor:
      writer.bigIntBitXorResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.BitXor");
      break;
    case JSOp::BitAnd:
      writer.bigIntBitAndResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.BitAnd");
      break;
    case JSOp::Lsh:
      writer.bigIntLeftShiftResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.LeftShift");
      break;
    case JSOp::Rsh:
      writer.bigIntRightShiftResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigInt.RightShift");
      break;
    default:
      MOZ_CRASH("Unhandled op in tryAttachBigInt");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachStringInt32Arith() {
  // Check for either int32 x string or string x int32.
  if (!(lhs_.isInt32() && rhs_.isString()) &&
      !(lhs_.isString() && rhs_.isInt32())) {
    return AttachDecision::NoAction;
  }

  // The created ICs will fail if the result can't be encoded as as int32.
  // Thus skip this IC, if the sample result is not an int32.
  if (!res_.isInt32()) {
    return AttachDecision::NoAction;
  }

  // Must _not_ support Add, because it would be string concatenation instead.
  // For Pow we can't easily determine the CanAttachInt32Pow conditions so we
  // reject that as well.
  if (op_ != JSOp::Sub && op_ != JSOp::Mul && op_ != JSOp::Div &&
      op_ != JSOp::Mod) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  auto guardToInt32 = [&](ValOperandId id, const Value& v) {
    if (v.isInt32()) {
      return writer.guardToInt32(id);
    }

    MOZ_ASSERT(v.isString());
    StringOperandId strId = writer.guardToString(id);
    return writer.guardStringToInt32(strId);
  };

  Int32OperandId lhsIntId = guardToInt32(lhsId, lhs_);
  Int32OperandId rhsIntId = guardToInt32(rhsId, rhs_);

  switch (op_) {
    case JSOp::Sub:
      writer.int32SubResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringInt32.Sub");
      break;
    case JSOp::Mul:
      writer.int32MulResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringInt32.Mul");
      break;
    case JSOp::Div:
      writer.int32DivResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringInt32.Div");
      break;
    case JSOp::Mod:
      writer.int32ModResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringInt32.Mod");
      break;
    default:
      MOZ_CRASH("Unhandled op in tryAttachStringInt32Arith");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

NewArrayIRGenerator::NewArrayIRGenerator(JSContext* cx, HandleScript script,
                                         jsbytecode* pc, ICState state, JSOp op,
                                         HandleObject templateObj,
                                         BaselineFrame* frame)
    : IRGenerator(cx, script, pc, CacheKind::NewArray, state),
#ifdef JS_CACHEIR_SPEW
      op_(op),
#endif
      templateObject_(templateObj),
      frame_(frame) {
  MOZ_ASSERT(templateObject_);
}

void NewArrayIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.opcodeProperty("op", op_);
  }
#endif
}

// Allocation sites are usually created during baseline compilation, but we also
// need to created them when an IC stub is added to a baseline compiled script
// and when trial inlining.
static gc::AllocSite* MaybeCreateAllocSite(jsbytecode* pc,
                                           BaselineFrame* frame) {
  MOZ_ASSERT(BytecodeOpCanHaveAllocSite(JSOp(*pc)));

  JSScript* outerScript = frame->outerScript();
  bool inInterpreter = frame->runningInInterpreter();
  bool isInlined = frame->icScript()->isInlined();

  if (inInterpreter && !isInlined) {
    return outerScript->zone()->unknownAllocSite();
  }

  return outerScript->createAllocSite();
}

AttachDecision NewArrayIRGenerator::tryAttachArrayObject() {
  ArrayObject* arrayObj = &templateObject_->as<ArrayObject>();

  MOZ_ASSERT(arrayObj->numUsedFixedSlots() == 0);
  MOZ_ASSERT(arrayObj->numDynamicSlots() == 0);
  MOZ_ASSERT(!arrayObj->hasPrivate());
  MOZ_ASSERT(!arrayObj->isSharedMemory());

  // The macro assembler only supports creating arrays with fixed elements.
  if (arrayObj->hasDynamicElements()) {
    return AttachDecision::NoAction;
  }

  // Stub doesn't support metadata builder
  if (cx_->realm()->hasAllocationMetadataBuilder()) {
    return AttachDecision::NoAction;
  }

  writer.guardNoAllocationMetadataBuilder(
      cx_->realm()->addressOfMetadataBuilder());

  gc::AllocSite* site = MaybeCreateAllocSite(pc_, frame_);
  if (!site) {
    return AttachDecision::NoAction;
  }

  Shape* shape = arrayObj->shape();
  uint32_t length = arrayObj->length();

  writer.newArrayObjectResult(length, shape, site);

  writer.returnFromIC();

  trackAttached("NewArrayObject");
  return AttachDecision::Attach;
}

AttachDecision NewArrayIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachArrayObject());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

NewObjectIRGenerator::NewObjectIRGenerator(JSContext* cx, HandleScript script,
                                           jsbytecode* pc, ICState state,
                                           JSOp op, HandleObject templateObj,
                                           BaselineFrame* frame)
    : IRGenerator(cx, script, pc, CacheKind::NewObject, state),
#ifdef JS_CACHEIR_SPEW
      op_(op),
#endif
      templateObject_(templateObj),
      frame_(frame) {
  MOZ_ASSERT(templateObject_);
}

void NewObjectIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.opcodeProperty("op", op_);
  }
#endif
}

AttachDecision NewObjectIRGenerator::tryAttachPlainObject() {
  // Don't optimize allocations with too many dynamic slots. We use an unrolled
  // loop when initializing slots and this avoids generating too much code.
  static const uint32_t MaxDynamicSlotsToOptimize = 64;

  NativeObject* nativeObj = &templateObject_->as<NativeObject>();
  MOZ_ASSERT(nativeObj->is<PlainObject>());

  // Stub doesn't support metadata builder
  if (cx_->realm()->hasAllocationMetadataBuilder()) {
    return AttachDecision::NoAction;
  }

  if (nativeObj->numDynamicSlots() > MaxDynamicSlotsToOptimize) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(!nativeObj->hasPrivate());
  MOZ_ASSERT(!nativeObj->hasDynamicElements());
  MOZ_ASSERT(!nativeObj->isSharedMemory());

  gc::AllocSite* site = MaybeCreateAllocSite(pc_, frame_);
  if (!site) {
    return AttachDecision::NoAction;
  }

  uint32_t numFixedSlots = nativeObj->numUsedFixedSlots();
  uint32_t numDynamicSlots = nativeObj->numDynamicSlots();
  gc::AllocKind allocKind = nativeObj->allocKindForTenure();
  Shape* shape = nativeObj->shape();

  writer.guardNoAllocationMetadataBuilder(
      cx_->realm()->addressOfMetadataBuilder());
  writer.newPlainObjectResult(numFixedSlots, numDynamicSlots, allocKind, shape,
                              site);

  writer.returnFromIC();

  trackAttached("NewPlainObject");
  return AttachDecision::Attach;
}

AttachDecision NewObjectIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachPlainObject());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

#ifdef JS_SIMULATOR
bool js::jit::CallAnyNative(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSObject* calleeObj = &args.callee();

  MOZ_ASSERT(calleeObj->is<JSFunction>());
  auto* calleeFunc = &calleeObj->as<JSFunction>();
  MOZ_ASSERT(calleeFunc->isNativeWithoutJitEntry());

  JSNative native = calleeFunc->native();
  return native(cx, args.length(), args.base());
}
#endif
