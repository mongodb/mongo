/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/CacheIR.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"

#include "jit/BaselineCacheIRCompiler.h"
#include "jit/BaselineIC.h"
#include "jit/CacheIRSpewer.h"
#include "vm/SelfHosting.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/UnboxedObject-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;
using mozilla::Maybe;

const char* js::jit::CacheKindNames[] = {
#define DEFINE_KIND(kind) #kind,
    CACHE_IR_KINDS(DEFINE_KIND)
#undef DEFINE_KIND
};

void
CacheIRWriter::assertSameCompartment(JSObject* obj) {
    assertSameCompartmentDebugOnly(cx_, obj);
}

IRGenerator::IRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, CacheKind cacheKind,
                         ICState::Mode mode)
  : writer(cx),
    cx_(cx),
    script_(script),
    pc_(pc),
    cacheKind_(cacheKind),
    mode_(mode)
{}

GetPropIRGenerator::GetPropIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                                       CacheKind cacheKind, ICState::Mode mode,
                                       bool* isTemporarilyUnoptimizable, HandleValue val,
                                       HandleValue idVal, HandleValue receiver,
                                       GetPropertyResultFlags resultFlags)
  : IRGenerator(cx, script, pc, cacheKind, mode),
    val_(val),
    idVal_(idVal),
    receiver_(receiver),
    isTemporarilyUnoptimizable_(isTemporarilyUnoptimizable),
    resultFlags_(resultFlags),
    preliminaryObjectAction_(PreliminaryObjectAction::None)
{}

static void
EmitLoadSlotResult(CacheIRWriter& writer, ObjOperandId holderOp, NativeObject* holder,
                   Shape* shape)
{
    if (holder->isFixedSlot(shape->slot())) {
        writer.loadFixedSlotResult(holderOp, NativeObject::getFixedSlotOffset(shape->slot()));
    } else {
        size_t dynamicSlotOffset = holder->dynamicSlotIndex(shape->slot()) * sizeof(Value);
        writer.loadDynamicSlotResult(holderOp, dynamicSlotOffset);
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

static ProxyStubType
GetProxyStubType(JSContext* cx, HandleObject obj, HandleId id)
{
    if (!obj->is<ProxyObject>())
        return ProxyStubType::None;

    if (!IsCacheableDOMProxy(obj))
        return ProxyStubType::Generic;

    DOMProxyShadowsResult shadows = GetDOMProxyShadowsCheck()(cx, obj, id);
    if (shadows == ShadowCheckFailed) {
        cx->clearPendingException();
        return ProxyStubType::None;
    }

    if (DOMProxyIsShadowing(shadows)) {
        if (shadows == ShadowsViaDirectExpando || shadows == ShadowsViaIndirectExpando)
            return ProxyStubType::DOMExpando;
        return ProxyStubType::DOMShadowed;
    }

    MOZ_ASSERT(shadows == DoesntShadow || shadows == DoesntShadowUnique);
    return ProxyStubType::DOMUnshadowed;
}

static bool
ValueToNameOrSymbolId(JSContext* cx, HandleValue idval, MutableHandleId id,
                      bool* nameOrSymbol)
{
    *nameOrSymbol = false;

    if (!idval.isString() && !idval.isSymbol())
        return true;

    if (!ValueToId<CanGC>(cx, idval, id))
        return false;

    if (!JSID_IS_STRING(id) && !JSID_IS_SYMBOL(id)) {
        id.set(JSID_VOID);
        return true;
    }

    uint32_t dummy;
    if (JSID_IS_STRING(id) && JSID_TO_ATOM(id)->isIndex(&dummy)) {
        id.set(JSID_VOID);
        return true;
    }

    *nameOrSymbol = true;
    return true;
}

bool
GetPropIRGenerator::tryAttachStub()
{
    // Idempotent ICs should call tryAttachIdempotentStub instead.
    MOZ_ASSERT(!idempotent());

    AutoAssertNoPendingException aanpe(cx_);

    // Non-object receivers are a degenerate case, so don't try to attach
    // stubs. The stubs we do emit will still perform runtime checks and
    // fallback as needed.
    if (isSuper() && !receiver_.isObject())
        return false;

    ValOperandId valId(writer.setInputOperandId(0));
    if (cacheKind_ != CacheKind::GetProp) {
        MOZ_ASSERT_IF(cacheKind_ == CacheKind::GetPropSuper, getSuperReceiverValueId().id() == 1);
        MOZ_ASSERT_IF(cacheKind_ != CacheKind::GetPropSuper, getElemKeyValueId().id() == 1);
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
        return false;
    }

    if (val_.isObject()) {
        RootedObject obj(cx_, &val_.toObject());
        ObjOperandId objId = writer.guardIsObject(valId);
        if (nameOrSymbol) {
            if (tryAttachObjectLength(obj, objId, id))
                return true;
            if (tryAttachNative(obj, objId, id))
                return true;
            if (tryAttachUnboxed(obj, objId, id))
                return true;
            if (tryAttachUnboxedExpando(obj, objId, id))
                return true;
            if (tryAttachTypedObject(obj, objId, id))
                return true;
            if (tryAttachModuleNamespace(obj, objId, id))
                return true;
            if (tryAttachWindowProxy(obj, objId, id))
                return true;
            if (tryAttachCrossCompartmentWrapper(obj, objId, id))
                return true;
            if (tryAttachXrayCrossCompartmentWrapper(obj, objId, id))
                return true;
            if (tryAttachFunction(obj, objId, id))
                return true;
            if (tryAttachProxy(obj, objId, id))
                return true;

            trackAttached(IRGenerator::NotAttached);
            return false;
        }

        MOZ_ASSERT(cacheKind_ == CacheKind::GetElem || cacheKind_ == CacheKind::GetElemSuper);

        if (tryAttachProxyElement(obj, objId))
            return true;

        uint32_t index;
        Int32OperandId indexId;
        if (maybeGuardInt32Index(idVal_, getElemKeyValueId(), &index, &indexId)) {
            if (tryAttachTypedElement(obj, objId, index, indexId))
                return true;
            if (tryAttachDenseElement(obj, objId, index, indexId))
                return true;
            if (tryAttachDenseElementHole(obj, objId, index, indexId))
                return true;
            if (tryAttachArgumentsObjectArg(obj, objId, indexId))
                return true;

            trackAttached(IRGenerator::NotAttached);
            return false;
        }

        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    if (nameOrSymbol) {
        if (tryAttachPrimitive(valId, id))
            return true;
        if (tryAttachStringLength(valId, id))
            return true;
        if (tryAttachMagicArgumentsName(valId, id))
            return true;

        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    if (idVal_.isInt32()) {
        ValOperandId indexId = getElemKeyValueId();
        if (tryAttachStringChar(valId, indexId))
            return true;
        if (tryAttachMagicArgument(valId, indexId))
            return true;

        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    trackAttached(IRGenerator::NotAttached);
    return false;
}

bool
GetPropIRGenerator::tryAttachIdempotentStub()
{
    // For idempotent ICs, only attach stubs which we can be sure have no side
    // effects and produce a result which the MIR in the calling code is able
    // to handle, since we do not have a pc to explicitly monitor the result.

    MOZ_ASSERT(idempotent());

    RootedObject obj(cx_, &val_.toObject());
    RootedId id(cx_, NameToId(idVal_.toString()->asAtom().asPropertyName()));

    ValOperandId valId(writer.setInputOperandId(0));
    ObjOperandId objId = writer.guardIsObject(valId);
    if (tryAttachNative(obj, objId, id))
        return true;

    // Object lengths are supported only if int32 results are allowed.
    if (tryAttachObjectLength(obj, objId, id))
        return true;

    // Also support native data properties on DOMProxy prototypes.
    if (GetProxyStubType(cx_, obj, id) == ProxyStubType::DOMUnshadowed)
        return tryAttachDOMProxyUnshadowed(obj, objId, id);

    return false;
}

static bool
IsCacheableProtoChain(JSObject* obj, JSObject* holder)
{
    while (obj != holder) {
        /*
         * We cannot assume that we find the holder object on the prototype
         * chain and must check for null proto. The prototype chain can be
         * altered during the lookupProperty call.
         */
        JSObject* proto = obj->staticPrototype();
        if (!proto || !proto->isNative())
            return false;
        obj = proto;
    }
    return true;
}

static bool
IsCacheableGetPropReadSlot(JSObject* obj, JSObject* holder, PropertyResult prop)
{
    if (!prop || !IsCacheableProtoChain(obj, holder))
        return false;

    Shape* shape = prop.shape();
    if (!shape->isDataProperty())
        return false;

    return true;
}

static bool
IsCacheableGetPropCallNative(JSObject* obj, JSObject* holder, Shape* shape)
{
    if (!shape || !IsCacheableProtoChain(obj, holder))
        return false;

    if (!shape->hasGetterValue() || !shape->getterValue().isObject())
        return false;

    if (!shape->getterValue().toObject().is<JSFunction>())
        return false;

    JSFunction& getter = shape->getterValue().toObject().as<JSFunction>();
    if (!getter.isNativeWithCppEntry())
        return false;

    if (getter.isClassConstructor())
        return false;

    // Check for a getter that has jitinfo and whose jitinfo says it's
    // OK with both inner and outer objects.
    if (getter.hasJitInfo() && !getter.jitInfo()->needsOuterizedThisObject())
        return true;

    // For getters that need the WindowProxy (instead of the Window) as this
    // object, don't cache if obj is the Window, since our cache will pass that
    // instead of the WindowProxy.
    return !IsWindow(obj);
}

static bool
IsCacheableGetPropCallScripted(JSObject* obj, JSObject* holder, Shape* shape,
                               bool* isTemporarilyUnoptimizable = nullptr)
{
    if (!shape || !IsCacheableProtoChain(obj, holder))
        return false;

    if (!shape->hasGetterValue() || !shape->getterValue().isObject())
        return false;

    if (!shape->getterValue().toObject().is<JSFunction>())
        return false;

    // See IsCacheableGetPropCallNative.
    if (IsWindow(obj))
        return false;

    JSFunction& getter = shape->getterValue().toObject().as<JSFunction>();
    if (getter.isNativeWithCppEntry())
        return false;

    // Natives with jit entry can use the scripted path.
    if (getter.isNativeWithJitEntry())
        return true;

    if (!getter.hasScript()) {
        if (isTemporarilyUnoptimizable)
            *isTemporarilyUnoptimizable = true;
        return false;
    }

    if (getter.isClassConstructor())
        return false;

    return true;
}

static bool
CheckHasNoSuchOwnProperty(JSContext* cx, JSObject* obj, jsid id)
{
    if (obj->isNative()) {
        // Don't handle proto chains with resolve hooks.
        if (ClassMayResolveId(cx->names(), obj->getClass(), id, obj))
            return false;
        if (obj->as<NativeObject>().contains(cx, id))
            return false;
    } else if (obj->is<UnboxedPlainObject>()) {
        if (obj->as<UnboxedPlainObject>().containsUnboxedOrExpandoProperty(cx, id))
            return false;
    } else if (obj->is<TypedObject>()) {
        if (obj->as<TypedObject>().typeDescr().hasProperty(cx->names(), id))
            return false;
    } else {
        return false;
    }

    return true;
}

static bool
CheckHasNoSuchProperty(JSContext* cx, JSObject* obj, jsid id)
{
    JSObject* curObj = obj;
    do {
        if (!CheckHasNoSuchOwnProperty(cx, curObj, id))
            return false;

        if (!curObj->isNative()) {
            // Non-native objects are only handled as the original receiver.
            if (curObj != obj)
                return false;
        }

        curObj = curObj->staticPrototype();
    } while (curObj);

    return true;
}

// Return whether obj is in some PreliminaryObjectArray and has a structure
// that might change in the future.
static bool
IsPreliminaryObject(JSObject* obj)
{
    if (obj->isSingleton())
        return false;

    TypeNewScript* newScript = obj->group()->newScript();
    if (newScript && !newScript->analyzed())
        return true;

    if (obj->group()->maybePreliminaryObjects())
        return true;

    return false;
}

static bool
IsCacheableNoProperty(JSContext* cx, JSObject* obj, JSObject* holder, Shape* shape, jsid id,
                      jsbytecode* pc, GetPropertyResultFlags resultFlags)
{
    if (shape)
        return false;

    MOZ_ASSERT(!holder);

    // Idempotent ICs may only attach missing-property stubs if undefined
    // results are explicitly allowed, since no monitoring is done of the
    // cache result.
    if (!pc && !(resultFlags & GetPropertyResultFlags::AllowUndefined))
        return false;

    // If we're doing a name lookup, we have to throw a ReferenceError. If
    // extra warnings are enabled, we may have to report a warning.
    // Note that Ion does not generate idempotent caches for JSOP_GETBOUNDNAME.
    if ((pc && *pc == JSOP_GETBOUNDNAME) || cx->compartment()->behaviors().extraWarnings(cx))
        return false;

    return CheckHasNoSuchProperty(cx, obj, id);
}

enum NativeGetPropCacheability {
    CanAttachNone,
    CanAttachReadSlot,
    CanAttachCallGetter,
};

static NativeGetPropCacheability
CanAttachNativeGetProp(JSContext* cx, HandleObject obj, HandleId id,
                       MutableHandleNativeObject holder, MutableHandleShape shape,
                       jsbytecode* pc, GetPropertyResultFlags resultFlags,
                       bool* isTemporarilyUnoptimizable)
{
    MOZ_ASSERT(JSID_IS_STRING(id) || JSID_IS_SYMBOL(id));

    // The lookup needs to be universally pure, otherwise we risk calling hooks out
    // of turn. We don't mind doing this even when purity isn't required, because we
    // only miss out on shape hashification, which is only a temporary perf cost.
    // The limits were arbitrarily set, anyways.
    JSObject* baseHolder = nullptr;
    PropertyResult prop;
    if (!LookupPropertyPure(cx, obj, id, &baseHolder, &prop))
        return CanAttachNone;

    MOZ_ASSERT(!holder);
    if (baseHolder) {
        if (!baseHolder->isNative())
            return CanAttachNone;
        holder.set(&baseHolder->as<NativeObject>());
    }
    shape.set(prop.maybeShape());

    if (IsCacheableGetPropReadSlot(obj, holder, prop))
        return CanAttachReadSlot;

    if (IsCacheableNoProperty(cx, obj, holder, shape, id, pc, resultFlags))
        return CanAttachReadSlot;

    // Idempotent ICs cannot call getters, see tryAttachIdempotentStub.
    if (pc && (resultFlags & GetPropertyResultFlags::Monitored)) {
        if (IsCacheableGetPropCallScripted(obj, holder, shape, isTemporarilyUnoptimizable))
            return CanAttachCallGetter;

        if (IsCacheableGetPropCallNative(obj, holder, shape))
            return CanAttachCallGetter;
    }

    return CanAttachNone;
}

static void
GuardGroupProto(CacheIRWriter& writer, JSObject* obj, ObjOperandId objId)
{
    // Uses the group to determine if the prototype is unchanged. If the
    // group's prototype is mutable, we must check the actual prototype,
    // otherwise checking the group is sufficient. This can be used if object
    // is not ShapedObject or if Shape has UNCACHEABLE_PROTO flag set.

    ObjectGroup* group = obj->groupRaw();

    if (group->hasUncacheableProto())
        writer.guardProto(objId, obj->staticPrototype());
    else
        writer.guardGroupForProto(objId, group);
}

// Guard that a given object has same class and same OwnProperties (excluding
// dense elements and dynamic properties). Returns an OperandId for the unboxed
// expando if it exists.
static void
TestMatchingReceiver(CacheIRWriter& writer, JSObject* obj, ObjOperandId objId,
                     Maybe<ObjOperandId>* expandoId)
{
    if (obj->is<UnboxedPlainObject>()) {
        writer.guardGroupForLayout(objId, obj->group());

        if (UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando()) {
            expandoId->emplace(writer.guardAndLoadUnboxedExpando(objId));
            writer.guardShapeForOwnProperties(expandoId->ref(), expando->lastProperty());
        } else {
            writer.guardNoUnboxedExpando(objId);
        }
    } else if (obj->is<TypedObject>()) {
        writer.guardGroupForLayout(objId, obj->group());
    } else if (obj->is<ProxyObject>()) {
        writer.guardShapeForClass(objId, obj->as<ProxyObject>().shape());
    } else {
        MOZ_ASSERT(obj->is<NativeObject>());
        writer.guardShapeForOwnProperties(objId, obj->as<NativeObject>().lastProperty());
    }
}

// Similar to |TestMatchingReceiver|, but specialized for NativeObject.
static void
TestMatchingNativeReceiver(CacheIRWriter& writer, NativeObject* obj, ObjOperandId objId)
{
    writer.guardShapeForOwnProperties(objId, obj->lastProperty());
}

// Similar to |TestMatchingReceiver|, but specialized for ProxyObject.
static void
TestMatchingProxyReceiver(CacheIRWriter& writer, ProxyObject* obj, ObjOperandId objId)
{
    writer.guardShapeForClass(objId, obj->shape());
}

// Adds additional guards if TestMatchingReceiver* does not also imply the
// prototype.
static void
GeneratePrototypeGuardsForReceiver(CacheIRWriter& writer, JSObject* obj, ObjOperandId objId)
{
    // If receiver was marked UNCACHEABLE_PROTO, the previous shape guard
    // doesn't ensure the prototype is unchanged. In this case we must use the
    // group to check the prototype.
    if (obj->hasUncacheableProto()) {
        MOZ_ASSERT(obj->is<NativeObject>());
        GuardGroupProto(writer, obj, objId);
    }

#ifdef DEBUG
    // The following cases already guaranteed the prototype is unchanged.
    if (obj->is<UnboxedPlainObject>())
        MOZ_ASSERT(!obj->group()->hasUncacheableProto());
    else if (obj->is<TypedObject>())
        MOZ_ASSERT(!obj->group()->hasUncacheableProto());
    else if (obj->is<ProxyObject>())
        MOZ_ASSERT(!obj->hasUncacheableProto());
#endif // DEBUG
}

static void
GeneratePrototypeGuards(CacheIRWriter& writer, JSObject* obj, JSObject* holder, ObjOperandId objId)
{
    // Assuming target property is on |holder|, generate appropriate guards to
    // ensure |holder| is still on the prototype chain of |obj| and we haven't
    // introduced any shadowing definitions.
    //
    // For each item in the proto chain before holder, we must ensure that
    // [[GetPrototypeOf]] still has the expected result, and that
    // [[GetOwnProperty]] has no definition of the target property.
    //
    //
    // Shape Teleporting Optimization
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
    // the chain. This means the same two shape checks as above are sufficient.
    //
    // Unfortunately we don't stop there and add further caveats. We may set
    // the UNCACHEABLE_PROTO flag on the shape of an object to indicate that it
    // will not generate a new shape if its prototype link is modified. If the
    // object is itself a prototype we follow the shape chain and regenerate
    // shapes (if they aren't themselves uncacheable).
    //
    // Let's consider the effect of the UNCACHEABLE_PROTO flag on our example:
    // - D is uncacheable: Add check that D still links to C
    // - C is uncacheable: Modifying C.__proto__ will still reshape B (if B is
    //                     not uncacheable)
    // - B is uncacheable: Add shape check C since B will not reshape OR check
    //                     proto of D and C
    //
    // See:
    //  - ReshapeForProtoMutation
    //  - ReshapeForShadowedProp

    MOZ_ASSERT(holder);
    MOZ_ASSERT(obj != holder);

    // Only DELEGATE objects participate in teleporting so peel off the first
    // object in the chain if needed and handle it directly.
    JSObject* pobj = obj;
    if (!obj->isDelegate()) {
        // TestMatchingReceiver does not always ensure the prototype is
        // unchanged, so generate extra guards as needed.
        GeneratePrototypeGuardsForReceiver(writer, obj, objId);

        pobj = obj->staticPrototype();
    }
    MOZ_ASSERT(pobj->isDelegate());

    // In the common case, holder has a cacheable prototype and will regenerate
    // its shape if any (delegate) objects in the proto chain are updated.
    if (!holder->hasUncacheableProto())
        return;

    // If already at the holder, no further proto checks are needed.
    if (pobj == holder)
        return;

    // NOTE: We could be clever and look for a middle prototype to shape check
    //       and elide some (but not all) of the group checks. Unless we have
    //       real-world examples, let's avoid the complexity.

    // Synchronize pobj and protoId.
    MOZ_ASSERT(pobj == obj || pobj == obj->staticPrototype());
    ObjOperandId protoId = (pobj == obj) ? objId
                                         : writer.loadProto(objId);

    // Guard prototype links from |pobj| to |holder|.
    while (pobj != holder) {
        pobj = pobj->staticPrototype();
        protoId = writer.loadProto(protoId);

        writer.guardSpecificObject(protoId, pobj);
    }
}

static void
GeneratePrototypeHoleGuards(CacheIRWriter& writer, JSObject* obj, ObjOperandId objId)
{
    if (obj->hasUncacheableProto())
        GuardGroupProto(writer, obj, objId);

    JSObject* pobj = obj->staticPrototype();
    while (pobj) {
        ObjOperandId protoId = writer.loadObject(pobj);

        // If shape doesn't imply proto, additional guards are needed.
        if (pobj->hasUncacheableProto())
            GuardGroupProto(writer, pobj, protoId);

        // Make sure the shape matches, to avoid non-dense elements or anything
        // else that is being checked by CanAttachDenseElementHole.
        writer.guardShape(protoId, pobj->as<NativeObject>().lastProperty());

        // Also make sure there are no dense elements.
        writer.guardNoDenseElements(protoId);

        pobj = pobj->staticPrototype();
    }
}

// Similar to |TestMatchingReceiver|, but for the holder object (when it
// differs from the receiver). The holder may also be the expando of the
// receiver if it exists.
static void
TestMatchingHolder(CacheIRWriter& writer, JSObject* obj, ObjOperandId objId)
{
    // The GeneratePrototypeGuards + TestMatchingHolder checks only support
    // prototype chains composed of NativeObject (excluding the receiver
    // itself).
    MOZ_ASSERT(obj->is<NativeObject>());

    writer.guardShapeForOwnProperties(objId, obj->as<NativeObject>().lastProperty());
}

static bool
UncacheableProtoOnChain(JSObject* obj)
{
    while (true) {
        if (obj->hasUncacheableProto())
            return true;

        obj = obj->staticPrototype();
        if (!obj)
            return false;
    }
}

static void
ShapeGuardProtoChain(CacheIRWriter& writer, JSObject* obj, ObjOperandId objId)
{
    while (true) {
        // Guard on the proto if the shape does not imply the proto.
        bool guardProto = obj->hasUncacheableProto();

        obj = obj->staticPrototype();
        if (!obj)
            return;

        objId = writer.loadProto(objId);
        if (guardProto)
            writer.guardSpecificObject(objId, obj);
        writer.guardShape(objId, obj->as<NativeObject>().shape());
    }
}

// For cross compartment guards we shape-guard the prototype chain to avoid
// referencing the holder object.
//
// This peels off the first layer because it's guarded against obj == holder.
static void
ShapeGuardProtoChainForCrossCompartmentHolder(CacheIRWriter& writer, JSObject* obj,
                                              ObjOperandId objId, JSObject* holder,
                                              Maybe<ObjOperandId>* holderId)
{
    MOZ_ASSERT(obj != holder);
    MOZ_ASSERT(holder);
    while (true) {
        obj = obj->staticPrototype();
        MOZ_ASSERT(obj);

        objId = writer.loadProto(objId);
        if (obj == holder) {
            TestMatchingHolder(writer, obj, objId);
            holderId->emplace(objId);
            return;
        } else {
            writer.guardShapeForOwnProperties(objId, obj->as<NativeObject>().shape());
        }
    }
}

enum class SlotReadType {
    Normal,
    CrossCompartment
};

template <SlotReadType MaybeCrossCompartment = SlotReadType::Normal>
static void
EmitReadSlotGuard(CacheIRWriter& writer, JSObject* obj, JSObject* holder,
                  ObjOperandId objId, Maybe<ObjOperandId>* holderId)
{
    Maybe<ObjOperandId> expandoId;
    TestMatchingReceiver(writer, obj, objId, &expandoId);

    if (obj != holder) {
        if (holder) {
            if (MaybeCrossCompartment == SlotReadType::CrossCompartment) {
                // Guard proto chain integrity.
                // We use a variant of guards that avoid baking in any cross-compartment
                // object pointers.
                ShapeGuardProtoChainForCrossCompartmentHolder(writer, obj, objId, holder,
                                                              holderId);
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
    } else if (obj->is<UnboxedPlainObject>()) {
        holderId->emplace(*expandoId);
    } else {
        holderId->emplace(objId);
    }
}

template <SlotReadType MaybeCrossCompartment = SlotReadType::Normal>
static void
EmitReadSlotResult(CacheIRWriter& writer, JSObject* obj, JSObject* holder,
                   Shape* shape, ObjOperandId objId)
{
    Maybe<ObjOperandId> holderId;
    EmitReadSlotGuard<MaybeCrossCompartment>(writer, obj, holder, objId, &holderId);

    if (obj == holder && obj->is<UnboxedPlainObject>())
        holder = obj->as<UnboxedPlainObject>().maybeExpando();

    // Slot access.
    if (holder) {
        MOZ_ASSERT(holderId->valid());
        EmitLoadSlotResult(writer, *holderId, &holder->as<NativeObject>(), shape);
    } else {
        MOZ_ASSERT(holderId.isNothing());
        writer.loadUndefinedResult();
    }
}

static void
EmitReadSlotReturn(CacheIRWriter& writer, JSObject*, JSObject* holder, Shape* shape,
                   bool wrapResult = false)
{
    // Slot access.
    if (holder) {
        MOZ_ASSERT(shape);
        if (wrapResult)
            writer.wrapResult();
        writer.typeMonitorResult();
    } else {
        // Normally for this op, the result would have to be monitored by TI.
        // However, since this stub ALWAYS returns UndefinedValue(), and we can be sure
        // that undefined is already registered with the type-set, this can be avoided.
        writer.returnFromIC();
    }
}

static void
EmitCallGetterResultNoGuards(CacheIRWriter& writer, JSObject* obj, JSObject* holder,
                             Shape* shape, ObjOperandId receiverId)
{
    if (IsCacheableGetPropCallNative(obj, holder, shape)) {
        JSFunction* target = &shape->getterValue().toObject().as<JSFunction>();
        MOZ_ASSERT(target->isNativeWithCppEntry());
        writer.callNativeGetterResult(receiverId, target);
        writer.typeMonitorResult();
        return;
    }

    MOZ_ASSERT(IsCacheableGetPropCallScripted(obj, holder, shape));

    JSFunction* target = &shape->getterValue().toObject().as<JSFunction>();
    MOZ_ASSERT(target->hasJitEntry());
    writer.callScriptedGetterResult(receiverId, target);
    writer.typeMonitorResult();
}

static void
EmitCallGetterResult(CacheIRWriter& writer, JSObject* obj, JSObject* holder, Shape* shape,
                     ObjOperandId objId, ObjOperandId receiverId, ICState::Mode mode)
{
    // Use the megamorphic guard if we're in megamorphic mode, except if |obj|
    // is a Window as GuardHasGetterSetter doesn't support this yet (Window may
    // require outerizing).
    if (mode == ICState::Mode::Specialized || IsWindow(obj)) {
        Maybe<ObjOperandId> expandoId;
        TestMatchingReceiver(writer, obj, objId, &expandoId);

        if (obj != holder) {
            GeneratePrototypeGuards(writer, obj, holder, objId);

            // Guard on the holder's shape.
            ObjOperandId holderId = writer.loadObject(holder);
            TestMatchingHolder(writer, holder, holderId);
        }
    } else {
        writer.guardHasGetterSetter(objId, shape);
    }

    EmitCallGetterResultNoGuards(writer, obj, holder, shape, receiverId);
}

static void
EmitCallGetterResult(CacheIRWriter& writer, JSObject* obj, JSObject* holder,
                     Shape* shape, ObjOperandId objId, ICState::Mode mode)
{
    EmitCallGetterResult(writer, obj, holder, shape, objId, objId, mode);
}

void
GetPropIRGenerator::attachMegamorphicNativeSlot(ObjOperandId objId, jsid id, bool handleMissing)
{
    MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);

    // The stub handles the missing-properties case only if we're seeing one
    // now, to make sure Ion ICs correctly monitor the undefined type.

    if (cacheKind_ == CacheKind::GetProp || cacheKind_ == CacheKind::GetPropSuper) {
        writer.megamorphicLoadSlotResult(objId, JSID_TO_ATOM(id)->asPropertyName(),
                                         handleMissing);
    } else {
        MOZ_ASSERT(cacheKind_ == CacheKind::GetElem || cacheKind_ == CacheKind::GetElemSuper);
        writer.megamorphicLoadSlotByValueResult(objId, getElemKeyValueId(), handleMissing);
    }
    writer.typeMonitorResult();

    trackAttached(handleMissing ? "MegamorphicMissingNativeSlot" : "MegamorphicNativeSlot");
}

bool
GetPropIRGenerator::tryAttachNative(HandleObject obj, ObjOperandId objId, HandleId id)
{
    RootedShape shape(cx_);
    RootedNativeObject holder(cx_);

    NativeGetPropCacheability type = CanAttachNativeGetProp(cx_, obj, id, &holder, &shape, pc_,
                                                            resultFlags_,
                                                            isTemporarilyUnoptimizable_);
    switch (type) {
      case CanAttachNone:
        return false;
      case CanAttachReadSlot:
        if (mode_ == ICState::Mode::Megamorphic) {
            attachMegamorphicNativeSlot(objId, id, holder == nullptr);
            return true;
        }

        maybeEmitIdGuard(id);
        if (holder) {
            EnsureTrackPropertyTypes(cx_, holder, id);
            // See the comment in StripPreliminaryObjectStubs.
            if (IsPreliminaryObject(obj))
                preliminaryObjectAction_ = PreliminaryObjectAction::NotePreliminary;
            else
                preliminaryObjectAction_ = PreliminaryObjectAction::Unlink;
        }
        EmitReadSlotResult(writer, obj, holder, shape, objId);
        EmitReadSlotReturn(writer, obj, holder, shape);

        trackAttached("NativeSlot");
        return true;
      case CanAttachCallGetter: {
        // |super.prop| accesses use a |this| value that differs from lookup object
        MOZ_ASSERT(!idempotent());
        ObjOperandId receiverId = isSuper() ? writer.guardIsObject(getSuperReceiverValueId())
                                            : objId;
        maybeEmitIdGuard(id);
        EmitCallGetterResult(writer, obj, holder, shape, objId, receiverId, mode_);

        trackAttached("NativeGetter");
        return true;
      }
    }

    MOZ_CRASH("Bad NativeGetPropCacheability");
}

bool
GetPropIRGenerator::tryAttachWindowProxy(HandleObject obj, ObjOperandId objId, HandleId id)
{
    // Attach a stub when the receiver is a WindowProxy and we can do the lookup
    // on the Window (the global object).

    if (!IsWindowProxy(obj))
        return false;

    // If we're megamorphic prefer a generic proxy stub that handles a lot more
    // cases.
    if (mode_ == ICState::Mode::Megamorphic)
        return false;

    // This must be a WindowProxy for the current Window/global. Else it would
    // be a cross-compartment wrapper and IsWindowProxy returns false for
    // those.
    MOZ_ASSERT(obj->getClass() == cx_->runtime()->maybeWindowProxyClass());
    MOZ_ASSERT(ToWindowIfWindowProxy(obj) == cx_->global());

    // Now try to do the lookup on the Window (the current global).
    HandleObject windowObj = cx_->global();
    RootedShape shape(cx_);
    RootedNativeObject holder(cx_);
    NativeGetPropCacheability type = CanAttachNativeGetProp(cx_, windowObj, id, &holder, &shape, pc_,
                                                            resultFlags_,
                                                            isTemporarilyUnoptimizable_);
    switch (type) {
      case CanAttachNone:
        return false;

      case CanAttachReadSlot: {
        maybeEmitIdGuard(id);
        writer.guardClass(objId, GuardClassKind::WindowProxy);

        ObjOperandId windowObjId = writer.loadObject(windowObj);
        EmitReadSlotResult(writer, windowObj, holder, shape, windowObjId);
        EmitReadSlotReturn(writer, windowObj, holder, shape);

        trackAttached("WindowProxySlot");
        return true;
      }

      case CanAttachCallGetter: {
        if (!IsCacheableGetPropCallNative(windowObj, holder, shape))
            return false;

        // Make sure the native getter is okay with the IC passing the Window
        // instead of the WindowProxy as |this| value.
        JSFunction* callee = &shape->getterObject()->as<JSFunction>();
        MOZ_ASSERT(callee->isNative());
        if (!callee->hasJitInfo() || callee->jitInfo()->needsOuterizedThisObject())
            return false;

        // If a |super| access, it is not worth the complexity to attach an IC.
        if (isSuper())
            return false;

        // Guard the incoming object is a WindowProxy and inline a getter call based
        // on the Window object.
        maybeEmitIdGuard(id);
        writer.guardClass(objId, GuardClassKind::WindowProxy);
        ObjOperandId windowObjId = writer.loadObject(windowObj);
        EmitCallGetterResult(writer, windowObj, holder, shape, windowObjId, mode_);

        trackAttached("WindowProxyGetter");
        return true;
      }
    }

    MOZ_CRASH("Unreachable");
}

bool
GetPropIRGenerator::tryAttachCrossCompartmentWrapper(HandleObject obj, ObjOperandId objId,
                                                     HandleId id)
{
    // We can only optimize this very wrapper-handler, because others might
    // have a security policy.
    if (!IsWrapper(obj) || Wrapper::wrapperHandler(obj) != &CrossCompartmentWrapper::singleton)
        return false;

    // If we're megamorphic prefer a generic proxy stub that handles a lot more
    // cases.
    if (mode_ == ICState::Mode::Megamorphic)
        return false;

    RootedObject unwrapped(cx_, Wrapper::wrappedObject(obj));
    MOZ_ASSERT(unwrapped == UnwrapOneChecked(obj));

    // If we allowed different zones we would have to wrap strings.
    if (unwrapped->compartment()->zone() != cx_->compartment()->zone())
        return false;

    // Take the unwrapped object's global, and wrap in a
    // this-compartment wrapper. This is what will be stored in the IC
    // keep the compartment alive.
    RootedObject wrappedTargetGlobal(cx_, &unwrapped->global());
    if (!cx_->compartment()->wrap(cx_, &wrappedTargetGlobal))
        return false;

    bool isWindowProxy = false;
    RootedShape shape(cx_);
    RootedNativeObject holder(cx_);

    // Enter compartment of target since some checks have side-effects
    // such as de-lazifying type info.
    {
        AutoCompartment ac(cx_, unwrapped);

        // The first CCW for iframes is almost always wrapping another WindowProxy
        // so we optimize for that case as well.
        isWindowProxy = IsWindowProxy(unwrapped);
        if (isWindowProxy) {
            MOZ_ASSERT(ToWindowIfWindowProxy(unwrapped) == unwrapped->compartment()->maybeGlobal());
            unwrapped = cx_->global();
            MOZ_ASSERT(unwrapped);
        }

        NativeGetPropCacheability canCache =
            CanAttachNativeGetProp(cx_, unwrapped, id, &holder, &shape, pc_,
                                resultFlags_, isTemporarilyUnoptimizable_);
        if (canCache != CanAttachReadSlot)
            return false;

        if (holder) {
            // Need to be in the compartment of the holder to
            // call EnsureTrackPropertyTypes
            EnsureTrackPropertyTypes(cx_, holder, id);
            if (unwrapped == holder) {
                // See the comment in StripPreliminaryObjectStubs.
                if (IsPreliminaryObject(unwrapped))
                    preliminaryObjectAction_ = PreliminaryObjectAction::NotePreliminary;
                else
                    preliminaryObjectAction_ = PreliminaryObjectAction::Unlink;
            }
        } else {
            // UNCACHEABLE_PROTO may result in guards against specific (cross-compartment)
            // prototype objects, so don't try to attach IC if we see the flag at all.
            if (UncacheableProtoOnChain(unwrapped)) {
                return false;
            }
        }
    }

    maybeEmitIdGuard(id);
    writer.guardIsProxy(objId);
    writer.guardHasProxyHandler(objId, Wrapper::wrapperHandler(obj));

    // Load the object wrapped by the CCW
    ObjOperandId wrapperTargetId = writer.loadWrapperTarget(objId);

    // If the compartment of the wrapped object is different we should fail.
    writer.guardCompartment(wrapperTargetId, wrappedTargetGlobal, unwrapped->compartment());

    ObjOperandId unwrappedId = wrapperTargetId;
    if (isWindowProxy) {
        // For the WindowProxy case also unwrap the inner window.
        // We avoid loadObject, because storing cross compartment objects in
        // stubs / JIT code is tricky.
        writer.guardClass(wrapperTargetId, GuardClassKind::WindowProxy);
        unwrappedId = writer.loadWrapperTarget(wrapperTargetId);
    }

    EmitReadSlotResult<SlotReadType::CrossCompartment>(writer, unwrapped, holder, shape, unwrappedId);
    EmitReadSlotReturn(writer, unwrapped, holder, shape, /* wrapResult = */ true);

    trackAttached("CCWSlot");
    return true;
}

static bool
GetXrayExpandoShapeWrapper(JSContext* cx, HandleObject xray, MutableHandleObject wrapper)
{
    Value v = GetProxyReservedSlot(xray, GetXrayJitInfo()->xrayHolderSlot);
    if (v.isObject()) {
        NativeObject* holder = &v.toObject().as<NativeObject>();
        v = holder->getFixedSlot(GetXrayJitInfo()->holderExpandoSlot);
        if (v.isObject()) {
            RootedNativeObject expando(cx, &UncheckedUnwrap(&v.toObject())->as<NativeObject>());
            wrapper.set(NewWrapperWithObjectShape(cx, expando));
            return wrapper != nullptr;
        }
    }
    wrapper.set(nullptr);
    return true;
}

bool
GetPropIRGenerator::tryAttachXrayCrossCompartmentWrapper(HandleObject obj, ObjOperandId objId,
                                                         HandleId id)
{
    if (!IsProxy(obj))
        return false;

    XrayJitInfo* info = GetXrayJitInfo();
    if (!info || !info->isCrossCompartmentXray(GetProxyHandler(obj)))
        return false;

    if (!info->globalHasExclusiveExpandos(cx_->global()))
        return false;

    RootedObject target(cx_, UncheckedUnwrap(obj));

    RootedObject expandoShapeWrapper(cx_);
    if (!GetXrayExpandoShapeWrapper(cx_, obj, &expandoShapeWrapper)) {
        cx_->recoverFromOutOfMemory();
        return false;
    }

    // Look for a getter we can call on the xray or its prototype chain.
    Rooted<PropertyDescriptor> desc(cx_);
    RootedObject holder(cx_, obj);
    AutoObjectVector prototypes(cx_);
    AutoObjectVector prototypeExpandoShapeWrappers(cx_);
    while (true) {
        if (!GetOwnPropertyDescriptor(cx_, holder, id, &desc)) {
            cx_->clearPendingException();
            return false;
        }
        if (desc.object())
            break;
        if (!GetPrototype(cx_, holder, &holder)) {
            cx_->clearPendingException();
            return false;
        }
        if (!holder || !IsProxy(holder) || !info->isCrossCompartmentXray(GetProxyHandler(holder)))
            return false;
        RootedObject prototypeExpandoShapeWrapper(cx_);
        if (!GetXrayExpandoShapeWrapper(cx_, holder, &prototypeExpandoShapeWrapper) ||
            !prototypes.append(holder) ||
            !prototypeExpandoShapeWrappers.append(prototypeExpandoShapeWrapper))
        {
            cx_->recoverFromOutOfMemory();
            return false;
        }
    }
    if (!desc.isAccessorDescriptor())
        return false;

    RootedObject getter(cx_, desc.getterObject());
    if (!getter || !getter->is<JSFunction>() || !getter->as<JSFunction>().isNative())
        return false;

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
    writer.guardXrayExpandoShapeAndDefaultProto(objId, expandoShapeWrapper);
    for (size_t i = 0; i < prototypes.length(); i++) {
        JSObject* proto = prototypes[i];
        ObjOperandId protoId = writer.loadObject(proto);
        writer.guardXrayExpandoShapeAndDefaultProto(protoId, prototypeExpandoShapeWrappers[i]);
    }

    writer.callNativeGetterResult(objId, &getter->as<JSFunction>());
    writer.typeMonitorResult();

    trackAttached("XrayGetter");
    return true;
}

bool
GetPropIRGenerator::tryAttachGenericProxy(HandleObject obj, ObjOperandId objId, HandleId id,
                                          bool handleDOMProxies)
{
    MOZ_ASSERT(obj->is<ProxyObject>());

    writer.guardIsProxy(objId);

    if (!handleDOMProxies) {
        // Ensure that the incoming object is not a DOM proxy, so that we can get to
        // the specialized stubs
        writer.guardNotDOMProxy(objId);
    }

    if (cacheKind_ == CacheKind::GetProp || mode_ == ICState::Mode::Specialized) {
        MOZ_ASSERT(!isSuper());
        maybeEmitIdGuard(id);
        writer.callProxyGetResult(objId, id);
    } else {
        // Attach a stub that handles every id.
        MOZ_ASSERT(cacheKind_ == CacheKind::GetElem);
        MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);
        MOZ_ASSERT(!isSuper());
        writer.callProxyGetByValueResult(objId, getElemKeyValueId());
    }

    writer.typeMonitorResult();

    trackAttached("GenericProxy");
    return true;
}

ObjOperandId
IRGenerator::guardDOMProxyExpandoObjectAndShape(JSObject* obj, ObjOperandId objId,
                                                const Value& expandoVal, JSObject* expandoObj)
{
    MOZ_ASSERT(IsCacheableDOMProxy(obj));

    TestMatchingProxyReceiver(writer, &obj->as<ProxyObject>(), objId);

    // Shape determines Class, so now it must be a DOM proxy.
    ValOperandId expandoValId;
    if (expandoVal.isObject())
        expandoValId = writer.loadDOMExpandoValue(objId);
    else
        expandoValId = writer.loadDOMExpandoValueIgnoreGeneration(objId);

    // Guard the expando is an object and shape guard.
    ObjOperandId expandoObjId = writer.guardIsObject(expandoValId);
    TestMatchingHolder(writer, expandoObj, expandoObjId);
    return expandoObjId;
}

bool
GetPropIRGenerator::tryAttachDOMProxyExpando(HandleObject obj, ObjOperandId objId, HandleId id)
{
    MOZ_ASSERT(IsCacheableDOMProxy(obj));

    RootedValue expandoVal(cx_, GetProxyPrivate(obj));
    RootedObject expandoObj(cx_);
    if (expandoVal.isObject()) {
        expandoObj = &expandoVal.toObject();
    } else {
        MOZ_ASSERT(!expandoVal.isUndefined(),
                   "How did a missing expando manage to shadow things?");
        auto expandoAndGeneration = static_cast<ExpandoAndGeneration*>(expandoVal.toPrivate());
        MOZ_ASSERT(expandoAndGeneration);
        expandoObj = &expandoAndGeneration->expando.toObject();
    }

    // Try to do the lookup on the expando object.
    RootedNativeObject holder(cx_);
    RootedShape propShape(cx_);
    NativeGetPropCacheability canCache =
        CanAttachNativeGetProp(cx_, expandoObj, id, &holder, &propShape, pc_,
                               resultFlags_, isTemporarilyUnoptimizable_);
    if (canCache != CanAttachReadSlot && canCache != CanAttachCallGetter)
        return false;
    if (!holder)
        return false;

    MOZ_ASSERT(holder == expandoObj);

    maybeEmitIdGuard(id);
    ObjOperandId expandoObjId =
        guardDOMProxyExpandoObjectAndShape(obj, objId, expandoVal, expandoObj);

    if (canCache == CanAttachReadSlot) {
        // Load from the expando's slots.
        EmitLoadSlotResult(writer, expandoObjId, &expandoObj->as<NativeObject>(), propShape);
        writer.typeMonitorResult();
    } else {
        // Call the getter. Note that we pass objId, the DOM proxy, as |this|
        // and not the expando object.
        MOZ_ASSERT(canCache == CanAttachCallGetter);
        EmitCallGetterResultNoGuards(writer, expandoObj, expandoObj, propShape, objId);
    }

    trackAttached("DOMProxyExpando");
    return true;
}

bool
GetPropIRGenerator::tryAttachDOMProxyShadowed(HandleObject obj, ObjOperandId objId, HandleId id)
{
    MOZ_ASSERT(!isSuper());
    MOZ_ASSERT(IsCacheableDOMProxy(obj));

    maybeEmitIdGuard(id);
    TestMatchingProxyReceiver(writer, &obj->as<ProxyObject>(), objId);
    writer.callProxyGetResult(objId, id);
    writer.typeMonitorResult();

    trackAttached("DOMProxyShadowed");
    return true;
}

// Callers are expected to have already guarded on the shape of the
// object, which guarantees the object is a DOM proxy.
static void
CheckDOMProxyExpandoDoesNotShadow(CacheIRWriter& writer, JSObject* obj, jsid id,
                                  ObjOperandId objId)
{
    MOZ_ASSERT(IsCacheableDOMProxy(obj));

    Value expandoVal = GetProxyPrivate(obj);

    ValOperandId expandoId;
    if (!expandoVal.isObject() && !expandoVal.isUndefined()) {
        auto expandoAndGeneration = static_cast<ExpandoAndGeneration*>(expandoVal.toPrivate());
        expandoId = writer.loadDOMExpandoValueGuardGeneration(objId, expandoAndGeneration);
        expandoVal = expandoAndGeneration->expando;
    } else {
        expandoId = writer.loadDOMExpandoValue(objId);
    }

    if (expandoVal.isUndefined()) {
        // Guard there's no expando object.
        writer.guardType(expandoId, JSVAL_TYPE_UNDEFINED);
    } else if (expandoVal.isObject()) {
        // Guard the proxy either has no expando object or, if it has one, that
        // the shape matches the current expando object.
        NativeObject& expandoObj = expandoVal.toObject().as<NativeObject>();
        MOZ_ASSERT(!expandoObj.containsPure(id));
        writer.guardDOMExpandoMissingOrGuardShape(expandoId, expandoObj.lastProperty());
    } else {
        MOZ_CRASH("Invalid expando value");
    }
}

bool
GetPropIRGenerator::tryAttachDOMProxyUnshadowed(HandleObject obj, ObjOperandId objId, HandleId id)
{
    MOZ_ASSERT(IsCacheableDOMProxy(obj));

    RootedObject checkObj(cx_, obj->staticPrototype());
    if (!checkObj)
        return false;

    RootedNativeObject holder(cx_);
    RootedShape shape(cx_);
    NativeGetPropCacheability canCache = CanAttachNativeGetProp(cx_, checkObj, id, &holder, &shape,
                                                                pc_, resultFlags_,
                                                                isTemporarilyUnoptimizable_);
    if (canCache == CanAttachNone)
        return false;

    maybeEmitIdGuard(id);

    // Guard that our expando object hasn't started shadowing this property.
    TestMatchingProxyReceiver(writer, &obj->as<ProxyObject>(), objId);
    CheckDOMProxyExpandoDoesNotShadow(writer, obj, id, objId);

    if (holder) {
        // Found the property on the prototype chain. Treat it like a native
        // getprop.
        GeneratePrototypeGuards(writer, obj, holder, objId);

        // Guard on the holder of the property.
        ObjOperandId holderId = writer.loadObject(holder);
        TestMatchingHolder(writer, holder, holderId);

        if (canCache == CanAttachReadSlot) {
            EmitLoadSlotResult(writer, holderId, holder, shape);
            writer.typeMonitorResult();
        } else {
            // EmitCallGetterResultNoGuards expects |obj| to be the object the
            // property is on to do some checks. Since we actually looked at
            // checkObj, and no extra guards will be generated, we can just
            // pass that instead.
            MOZ_ASSERT(canCache == CanAttachCallGetter);
            MOZ_ASSERT(!isSuper());
            EmitCallGetterResultNoGuards(writer, checkObj, holder, shape, objId);
        }
    } else {
        // Property was not found on the prototype chain. Deoptimize down to
        // proxy get call.
        MOZ_ASSERT(!isSuper());
        writer.callProxyGetResult(objId, id);
        writer.typeMonitorResult();
    }

    trackAttached("DOMProxyUnshadowed");
    return true;
}

bool
GetPropIRGenerator::tryAttachProxy(HandleObject obj, ObjOperandId objId, HandleId id)
{
    ProxyStubType type = GetProxyStubType(cx_, obj, id);
    if (type == ProxyStubType::None)
        return false;

    // The proxy stubs don't currently support |super| access.
    if (isSuper())
        return false;

    if (mode_ == ICState::Mode::Megamorphic)
        return tryAttachGenericProxy(obj, objId, id, /* handleDOMProxies = */ true);

    switch (type) {
      case ProxyStubType::None:
        break;
      case ProxyStubType::DOMExpando:
        if (tryAttachDOMProxyExpando(obj, objId, id))
            return true;
        if (*isTemporarilyUnoptimizable_) {
            // Scripted getter without JIT code. Just wait.
            return false;
        }
        MOZ_FALLTHROUGH; // Fall through to the generic shadowed case.
      case ProxyStubType::DOMShadowed:
        return tryAttachDOMProxyShadowed(obj, objId, id);
      case ProxyStubType::DOMUnshadowed:
        if (tryAttachDOMProxyUnshadowed(obj, objId, id))
            return true;
        if (*isTemporarilyUnoptimizable_) {
            // Scripted getter without JIT code. Just wait.
            return false;
        }
        return tryAttachGenericProxy(obj, objId, id, /* handleDOMProxies = */ true);
      case ProxyStubType::Generic:
        return tryAttachGenericProxy(obj, objId, id, /* handleDOMProxies = */ false);
    }

    MOZ_CRASH("Unexpected ProxyStubType");
}

bool
GetPropIRGenerator::tryAttachUnboxed(HandleObject obj, ObjOperandId objId, HandleId id)
{
    if (!obj->is<UnboxedPlainObject>())
        return false;

    const UnboxedLayout::Property* property = obj->as<UnboxedPlainObject>().layout().lookup(id);
    if (!property)
        return false;

    if (!cx_->runtime()->jitSupportsFloatingPoint)
        return false;

    maybeEmitIdGuard(id);
    writer.guardGroupForLayout(objId, obj->group());
    writer.loadUnboxedPropertyResult(objId, property->type,
                                     UnboxedPlainObject::offsetOfData() + property->offset);
    if (property->type == JSVAL_TYPE_OBJECT)
        writer.typeMonitorResult();
    else
        writer.returnFromIC();

    preliminaryObjectAction_ = PreliminaryObjectAction::Unlink;

    trackAttached("Unboxed");
    return true;
}

bool
GetPropIRGenerator::tryAttachUnboxedExpando(HandleObject obj, ObjOperandId objId, HandleId id)
{
    if (!obj->is<UnboxedPlainObject>())
        return false;

    UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando();
    if (!expando)
        return false;

    Shape* shape = expando->lookup(cx_, id);
    if (!shape || !shape->isDataProperty())
        return false;

    maybeEmitIdGuard(id);
    EmitReadSlotResult(writer, obj, obj, shape, objId);
    EmitReadSlotReturn(writer, obj, obj, shape);

    trackAttached("UnboxedExpando");
    return true;
}

static TypedThingLayout
GetTypedThingLayout(const Class* clasp)
{
    if (IsTypedArrayClass(clasp))
        return Layout_TypedArray;
    if (IsOutlineTypedObjectClass(clasp))
        return Layout_OutlineTypedObject;
    if (IsInlineTypedObjectClass(clasp))
        return Layout_InlineTypedObject;
    MOZ_CRASH("Bad object class");
}

bool
GetPropIRGenerator::tryAttachTypedObject(HandleObject obj, ObjOperandId objId, HandleId id)
{
    if (!obj->is<TypedObject>())
        return false;

    if (!cx_->runtime()->jitSupportsFloatingPoint || cx_->compartment()->detachedTypedObjects)
        return false;

    TypedObject* typedObj = &obj->as<TypedObject>();
    if (!typedObj->typeDescr().is<StructTypeDescr>())
        return false;

    StructTypeDescr* structDescr = &typedObj->typeDescr().as<StructTypeDescr>();
    size_t fieldIndex;
    if (!structDescr->fieldIndex(id, &fieldIndex))
        return false;

    TypeDescr* fieldDescr = &structDescr->fieldDescr(fieldIndex);
    if (!fieldDescr->is<SimpleTypeDescr>())
        return false;

    TypedThingLayout layout = GetTypedThingLayout(obj->getClass());

    uint32_t fieldOffset = structDescr->fieldOffset(fieldIndex);
    uint32_t typeDescr = SimpleTypeDescrKey(&fieldDescr->as<SimpleTypeDescr>());

    maybeEmitIdGuard(id);
    writer.guardNoDetachedTypedObjects();
    writer.guardGroupForLayout(objId, obj->group());
    writer.loadTypedObjectResult(objId, fieldOffset, layout, typeDescr);

    // Only monitor the result if the type produced by this stub might vary.
    bool monitorLoad = false;
    if (SimpleTypeDescrKeyIsScalar(typeDescr)) {
        Scalar::Type type = ScalarTypeFromSimpleTypeDescrKey(typeDescr);
        monitorLoad = type == Scalar::Uint32;
    } else {
        ReferenceTypeDescr::Type type = ReferenceTypeFromSimpleTypeDescrKey(typeDescr);
        monitorLoad = type != ReferenceTypeDescr::TYPE_STRING;
    }

    if (monitorLoad)
        writer.typeMonitorResult();
    else
        writer.returnFromIC();

    trackAttached("TypedObject");
    return true;
}

bool
GetPropIRGenerator::tryAttachObjectLength(HandleObject obj, ObjOperandId objId, HandleId id)
{
    if (!JSID_IS_ATOM(id, cx_->names().length))
        return false;

    if (!(resultFlags_ & GetPropertyResultFlags::AllowInt32))
        return false;

    if (obj->is<ArrayObject>()) {
        // Make sure int32 is added to the TypeSet before we attach a stub, so
        // the stub can return int32 values without monitoring the result.
        if (obj->as<ArrayObject>().length() > INT32_MAX)
            return false;

        maybeEmitIdGuard(id);
        writer.guardClass(objId, GuardClassKind::Array);
        writer.loadInt32ArrayLengthResult(objId);
        writer.returnFromIC();

        trackAttached("ArrayLength");
        return true;
    }

    if (obj->is<ArgumentsObject>() && !obj->as<ArgumentsObject>().hasOverriddenLength()) {
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
        return true;
    }

    return false;
}

bool
GetPropIRGenerator::tryAttachFunction(HandleObject obj, ObjOperandId objId, HandleId id)
{
    // Function properties are lazily resolved so they might not be defined yet.
    // And we might end up in a situation where we always have a fresh function
    // object during the IC generation.
    if (!obj->is<JSFunction>())
        return false;

    JSObject* holder = nullptr;
    PropertyResult prop;
    // This property exists already, don't attach the stub.
    if (LookupPropertyPure(cx_, obj, id, &holder, &prop))
        return false;

    JSFunction* fun = &obj->as<JSFunction>();

    if (JSID_IS_ATOM(id, cx_->names().length)) {
        // length was probably deleted from the function.
        if (fun->hasResolvedLength())
            return false;

        // Lazy functions don't store the length.
        if (fun->isInterpretedLazy())
            return false;

        maybeEmitIdGuard(id);
        writer.guardClass(objId, GuardClassKind::JSFunction);
        writer.loadFunctionLengthResult(objId);
        writer.returnFromIC();

        trackAttached("FunctionLength");
        return true;
    }

    return false;
}

bool
GetPropIRGenerator::tryAttachModuleNamespace(HandleObject obj, ObjOperandId objId, HandleId id)
{
    if (!obj->is<ModuleNamespaceObject>())
        return false;

    Rooted<ModuleNamespaceObject*> ns(cx_, &obj->as<ModuleNamespaceObject>());
    RootedModuleEnvironmentObject env(cx_);
    RootedShape shape(cx_);
    if (!ns->bindings().lookup(id, env.address(), shape.address()))
        return false;

    // Don't emit a stub until the target binding has been initialized.
    if (env->getSlot(shape->slot()).isMagic(JS_UNINITIALIZED_LEXICAL))
        return false;

    if (IsIonEnabled(cx_))
        EnsureTrackPropertyTypes(cx_, env, shape->propid());

    // Check for the specific namespace object.
    maybeEmitIdGuard(id);
    writer.guardSpecificObject(objId, ns);

    ObjOperandId envId = writer.loadObject(env);
    EmitLoadSlotResult(writer, envId, env, shape);
    writer.typeMonitorResult();

    trackAttached("ModuleNamespace");
    return true;
}

bool
GetPropIRGenerator::tryAttachPrimitive(ValOperandId valId, HandleId id)
{
    JSValueType primitiveType;
    RootedNativeObject proto(cx_);
    if (val_.isString()) {
        if (JSID_IS_ATOM(id, cx_->names().length)) {
            // String length is special-cased, see js::GetProperty.
            return false;
        }
        primitiveType = JSVAL_TYPE_STRING;
        proto = MaybeNativeObject(cx_->global()->maybeGetPrototype(JSProto_String));
    } else if (val_.isNumber()) {
        primitiveType = JSVAL_TYPE_DOUBLE;
        proto = MaybeNativeObject(cx_->global()->maybeGetPrototype(JSProto_Number));
    } else if (val_.isBoolean()) {
        primitiveType = JSVAL_TYPE_BOOLEAN;
        proto = MaybeNativeObject(cx_->global()->maybeGetPrototype(JSProto_Boolean));
    } else if (val_.isSymbol()) {
        primitiveType = JSVAL_TYPE_SYMBOL;
        proto = MaybeNativeObject(cx_->global()->maybeGetPrototype(JSProto_Symbol));
    } else {
        MOZ_ASSERT(val_.isNullOrUndefined() || val_.isMagic());
        return false;
    }
    if (!proto)
        return false;

    RootedShape shape(cx_);
    RootedNativeObject holder(cx_);
    NativeGetPropCacheability type = CanAttachNativeGetProp(cx_, proto, id, &holder, &shape, pc_,
                                                            resultFlags_,
                                                            isTemporarilyUnoptimizable_);
    if (type != CanAttachReadSlot)
        return false;

    if (holder) {
        // Instantiate this property, for use during Ion compilation.
        if (IsIonEnabled(cx_))
            EnsureTrackPropertyTypes(cx_, holder, id);
    }

    writer.guardType(valId, primitiveType);
    maybeEmitIdGuard(id);

    ObjOperandId protoId = writer.loadObject(proto);
    EmitReadSlotResult(writer, proto, holder, shape, protoId);
    EmitReadSlotReturn(writer, proto, holder, shape);

    trackAttached("Primitive");
    return true;
}

bool
GetPropIRGenerator::tryAttachStringLength(ValOperandId valId, HandleId id)
{
    if (!val_.isString() || !JSID_IS_ATOM(id, cx_->names().length))
        return false;

    StringOperandId strId = writer.guardIsString(valId);
    maybeEmitIdGuard(id);
    writer.loadStringLengthResult(strId);
    writer.returnFromIC();

    trackAttached("StringLength");
    return true;
}

bool
GetPropIRGenerator::tryAttachStringChar(ValOperandId valId, ValOperandId indexId)
{
    MOZ_ASSERT(idVal_.isInt32());

    if (!val_.isString())
        return false;

    int32_t index = idVal_.toInt32();
    if (index < 0)
        return false;

    JSString* str = val_.toString();
    if (size_t(index) >= str->length())
        return false;

    // This follows JSString::getChar, otherwise we fail to attach getChar in a lot of cases.
    if (str->isRope()) {
        JSRope* rope = &str->asRope();

        // Make sure the left side contains the index.
        if (size_t(index) >= rope->leftChild()->length())
            return false;

        str = rope->leftChild();
    }

    if (!str->isLinear() ||
        str->asLinear().latin1OrTwoByteChar(index) >= StaticStrings::UNIT_STATIC_LIMIT)
    {
        return false;
    }

    StringOperandId strId = writer.guardIsString(valId);
    Int32OperandId int32IndexId = writer.guardIsInt32Index(indexId);
    writer.loadStringCharResult(strId, int32IndexId);
    writer.returnFromIC();

    trackAttached("StringChar");
    return true;
}

bool
GetPropIRGenerator::tryAttachMagicArgumentsName(ValOperandId valId, HandleId id)
{
    if (!val_.isMagic(JS_OPTIMIZED_ARGUMENTS))
        return false;

    if (!JSID_IS_ATOM(id, cx_->names().length) && !JSID_IS_ATOM(id, cx_->names().callee))
        return false;

    maybeEmitIdGuard(id);
    writer.guardMagicValue(valId, JS_OPTIMIZED_ARGUMENTS);
    writer.guardFrameHasNoArgumentsObject();

    if (JSID_IS_ATOM(id, cx_->names().length)) {
        writer.loadFrameNumActualArgsResult();
        writer.returnFromIC();
    } else {
        MOZ_ASSERT(JSID_IS_ATOM(id, cx_->names().callee));
        writer.loadFrameCalleeResult();
        writer.typeMonitorResult();
    }

    trackAttached("MagicArgumentsName");
    return true;
}

bool
GetPropIRGenerator::tryAttachMagicArgument(ValOperandId valId, ValOperandId indexId)
{
    MOZ_ASSERT(idVal_.isInt32());

    if (!val_.isMagic(JS_OPTIMIZED_ARGUMENTS))
        return false;

    writer.guardMagicValue(valId, JS_OPTIMIZED_ARGUMENTS);
    writer.guardFrameHasNoArgumentsObject();

    Int32OperandId int32IndexId = writer.guardIsInt32Index(indexId);
    writer.loadFrameArgumentResult(int32IndexId);
    writer.typeMonitorResult();

    trackAttached("MagicArgument");
    return true;
}

bool
GetPropIRGenerator::tryAttachArgumentsObjectArg(HandleObject obj, ObjOperandId objId,
                                                Int32OperandId indexId)
{
    if (!obj->is<ArgumentsObject>() || obj->as<ArgumentsObject>().hasOverriddenElement())
        return false;

    if (!(resultFlags_ & GetPropertyResultFlags::Monitored))
        return false;

    if (obj->is<MappedArgumentsObject>()) {
        writer.guardClass(objId, GuardClassKind::MappedArguments);
    } else {
        MOZ_ASSERT(obj->is<UnmappedArgumentsObject>());
        writer.guardClass(objId, GuardClassKind::UnmappedArguments);
    }

    writer.loadArgumentsObjectArgResult(objId, indexId);
    writer.typeMonitorResult();

    trackAttached("ArgumentsObjectArg");
    return true;
}

bool
GetPropIRGenerator::tryAttachDenseElement(HandleObject obj, ObjOperandId objId,
                                          uint32_t index, Int32OperandId indexId)
{
    if (!obj->isNative())
        return false;

    NativeObject* nobj = &obj->as<NativeObject>();
    if (!nobj->containsDenseElement(index))
        return false;

    TestMatchingNativeReceiver(writer, nobj, objId);
    writer.loadDenseElementResult(objId, indexId);
    writer.typeMonitorResult();

    trackAttached("DenseElement");
    return true;
}

static bool
CanAttachDenseElementHole(NativeObject* obj, bool ownProp, bool allowIndexedReceiver = false)
{
    // Make sure the objects on the prototype don't have any indexed properties
    // or that such properties can't appear without a shape change.
    // Otherwise returning undefined for holes would obviously be incorrect,
    // because we would have to lookup a property on the prototype instead.
    do {
        // The first two checks are also relevant to the receiver object.
        if (!allowIndexedReceiver && obj->isIndexed())
            return false;
        allowIndexedReceiver = false;

        if (ClassCanHaveExtraProperties(obj->getClass()))
            return false;

        // Don't need to check prototype for OwnProperty checks
        if (ownProp)
            return true;

        JSObject* proto = obj->staticPrototype();
        if (!proto)
            break;

        if (!proto->isNative())
            return false;

        // Make sure objects on the prototype don't have dense elements.
        if (proto->as<NativeObject>().getDenseInitializedLength() != 0)
            return false;

        obj = &proto->as<NativeObject>();
    } while (true);

    return true;
}

bool
GetPropIRGenerator::tryAttachDenseElementHole(HandleObject obj, ObjOperandId objId,
                                              uint32_t index, Int32OperandId indexId)
{
    if (!obj->isNative())
        return false;

    NativeObject* nobj = &obj->as<NativeObject>();
    if (nobj->containsDenseElement(index))
        return false;
    if (!CanAttachDenseElementHole(nobj, false))
        return false;

    // Guard on the shape, to prevent non-dense elements from appearing.
    TestMatchingNativeReceiver(writer, nobj, objId);
    GeneratePrototypeHoleGuards(writer, nobj, objId);
    writer.loadDenseElementHoleResult(objId, indexId);
    writer.typeMonitorResult();

    trackAttached("DenseElementHole");
    return true;
}

static bool
IsPrimitiveArrayTypedObject(JSObject* obj)
{
    if (!obj->is<TypedObject>())
        return false;
    TypeDescr& descr = obj->as<TypedObject>().typeDescr();
    return descr.is<ArrayTypeDescr>() &&
           descr.as<ArrayTypeDescr>().elementType().is<ScalarTypeDescr>();
}

static Scalar::Type
PrimitiveArrayTypedObjectType(JSObject* obj)
{
    MOZ_ASSERT(IsPrimitiveArrayTypedObject(obj));
    TypeDescr& descr = obj->as<TypedObject>().typeDescr();
    return descr.as<ArrayTypeDescr>().elementType().as<ScalarTypeDescr>().type();
}


static Scalar::Type
TypedThingElementType(JSObject* obj)
{
    return obj->is<TypedArrayObject>()
           ? obj->as<TypedArrayObject>().type()
           : PrimitiveArrayTypedObjectType(obj);
}

static bool
TypedThingRequiresFloatingPoint(JSObject* obj)
{
    Scalar::Type type = TypedThingElementType(obj);
    return type == Scalar::Uint32 ||
           type == Scalar::Float32 ||
           type == Scalar::Float64;
}

bool
GetPropIRGenerator::tryAttachTypedElement(HandleObject obj, ObjOperandId objId,
                                          uint32_t index, Int32OperandId indexId)
{
    if (!obj->is<TypedArrayObject>() && !IsPrimitiveArrayTypedObject(obj))
        return false;

    if (!cx_->runtime()->jitSupportsFloatingPoint && TypedThingRequiresFloatingPoint(obj))
        return false;

    // Ensure the index is in-bounds so the element type gets monitored.
    if (obj->is<TypedArrayObject>() && index >= obj->as<TypedArrayObject>().length())
        return false;

    // Don't attach typed object stubs if the underlying storage could be
    // detached, as the stub will always bail out.
    if (IsPrimitiveArrayTypedObject(obj) && cx_->compartment()->detachedTypedObjects)
        return false;

    TypedThingLayout layout = GetTypedThingLayout(obj->getClass());

    if (IsPrimitiveArrayTypedObject(obj)) {
        writer.guardNoDetachedTypedObjects();
        writer.guardGroupForLayout(objId, obj->group());
    } else {
        writer.guardShapeForClass(objId, obj->as<TypedArrayObject>().shape());
    }

    writer.loadTypedElementResult(objId, indexId, layout, TypedThingElementType(obj));

    // Reading from Uint32Array may produce an int32 now but a double value
    // later, so ensure we monitor the result.
    if (TypedThingElementType(obj) == Scalar::Type::Uint32)
        writer.typeMonitorResult();
    else
        writer.returnFromIC();

    trackAttached("TypedElement");
    return true;
}

bool
GetPropIRGenerator::tryAttachProxyElement(HandleObject obj, ObjOperandId objId)
{
    if (!obj->is<ProxyObject>())
        return false;

    // The proxy stubs don't currently support |super| access.
    if (isSuper())
        return false;

    writer.guardIsProxy(objId);

    // We are not guarding against DOM proxies here, because there is no other
    // specialized DOM IC we could attach.
    // We could call maybeEmitIdGuard here and then emit CallProxyGetResult,
    // but for GetElem we prefer to attach a stub that can handle any Value
    // so we don't attach a new stub for every id.
    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem);
    MOZ_ASSERT(!isSuper());
    writer.callProxyGetByValueResult(objId, getElemKeyValueId());
    writer.typeMonitorResult();

    trackAttached("ProxyElement");
    return true;
}

void
GetPropIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("base", val_);
        sp.valueProperty("property", idVal_);
    }
#endif
}

void
IRGenerator::emitIdGuard(ValOperandId valId, jsid id)
{
    if (JSID_IS_SYMBOL(id)) {
        SymbolOperandId symId = writer.guardIsSymbol(valId);
        writer.guardSpecificSymbol(symId, JSID_TO_SYMBOL(id));
    } else {
        MOZ_ASSERT(JSID_IS_ATOM(id));
        StringOperandId strId = writer.guardIsString(valId);
        writer.guardSpecificAtom(strId, JSID_TO_ATOM(id));
    }
}

void
GetPropIRGenerator::maybeEmitIdGuard(jsid id)
{
    if (cacheKind_ == CacheKind::GetProp || cacheKind_ == CacheKind::GetPropSuper) {
        // Constant PropertyName, no guards necessary.
        MOZ_ASSERT(&idVal_.toString()->asAtom() == JSID_TO_ATOM(id));
        return;
    }

    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem || cacheKind_ == CacheKind::GetElemSuper);
    emitIdGuard(getElemKeyValueId(), id);
}

void
SetPropIRGenerator::maybeEmitIdGuard(jsid id)
{
    if (cacheKind_ == CacheKind::SetProp) {
        // Constant PropertyName, no guards necessary.
        MOZ_ASSERT(&idVal_.toString()->asAtom() == JSID_TO_ATOM(id));
        return;
    }

    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    emitIdGuard(setElemKeyValueId(), id);
}

GetNameIRGenerator::GetNameIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                                       ICState::Mode mode, HandleObject env,
                                       HandlePropertyName name)
  : IRGenerator(cx, script, pc, CacheKind::GetName, mode),
    env_(env),
    name_(name)
{}

bool
GetNameIRGenerator::tryAttachStub()
{
    MOZ_ASSERT(cacheKind_ == CacheKind::GetName);

    AutoAssertNoPendingException aanpe(cx_);

    ObjOperandId envId(writer.setInputOperandId(0));
    RootedId id(cx_, NameToId(name_));

    if (tryAttachGlobalNameValue(envId, id))
        return true;
    if (tryAttachGlobalNameGetter(envId, id))
        return true;
    if (tryAttachEnvironmentName(envId, id))
        return true;

    trackAttached(IRGenerator::NotAttached);
    return false;
}

bool
CanAttachGlobalName(JSContext* cx, Handle<LexicalEnvironmentObject*> globalLexical, HandleId id,
                    MutableHandleNativeObject holder, MutableHandleShape shape)
{
    // The property must be found, and it must be found as a normal data property.
    RootedNativeObject current(cx, globalLexical);
    while (true) {
        shape.set(current->lookup(cx, id));
        if (shape)
            break;

        if (current == globalLexical) {
            current = &globalLexical->global();
        } else {
            // In the browser the global prototype chain should be immutable.
            if (!current->staticPrototypeIsImmutable())
                return false;

            JSObject* proto = current->staticPrototype();
            if (!proto || !proto->is<NativeObject>())
                return false;

            current = &proto->as<NativeObject>();
        }
    }

    holder.set(current);
    return true;
}

bool
GetNameIRGenerator::tryAttachGlobalNameValue(ObjOperandId objId, HandleId id)
{
    if (!IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope())
        return false;

    Handle<LexicalEnvironmentObject*> globalLexical = env_.as<LexicalEnvironmentObject>();
    MOZ_ASSERT(globalLexical->isGlobal());

    RootedNativeObject holder(cx_);
    RootedShape shape(cx_);
    if (!CanAttachGlobalName(cx_, globalLexical, id, &holder, &shape))
        return false;

    // The property must be found, and it must be found as a normal data property.
    if (!shape->isDataProperty())
        return false;

    // This might still be an uninitialized lexical.
    if (holder->getSlot(shape->slot()).isMagic())
        return false;

    // Instantiate this global property, for use during Ion compilation.
    if (IsIonEnabled(cx_))
        EnsureTrackPropertyTypes(cx_, holder, id);

    if (holder == globalLexical) {
        // There is no need to guard on the shape. Lexical bindings are
        // non-configurable, and this stub cannot be shared across globals.
        size_t dynamicSlotOffset = holder->dynamicSlotIndex(shape->slot()) * sizeof(Value);
        writer.loadDynamicSlotResult(objId, dynamicSlotOffset);
    } else {
        // Check the prototype chain from the global to the holder
        // prototype. Ignore the global lexical scope as it doesn't figure
        // into the prototype chain. We guard on the global lexical
        // scope's shape independently.
        if (!IsCacheableGetPropReadSlot(&globalLexical->global(), holder, PropertyResult(shape)))
            return false;

        // Shape guard for global lexical.
        writer.guardShape(objId, globalLexical->lastProperty());

        // Guard on the shape of the GlobalObject.
        ObjOperandId globalId = writer.loadEnclosingEnvironment(objId);
        writer.guardShape(globalId, globalLexical->global().lastProperty());

        ObjOperandId holderId = globalId;
        if (holder != &globalLexical->global()) {
            // Shape guard holder.
            holderId = writer.loadObject(holder);
            writer.guardShape(holderId, holder->lastProperty());
        }

        EmitLoadSlotResult(writer, holderId, holder, shape);
    }

    writer.typeMonitorResult();

    trackAttached("GlobalNameValue");
    return true;
}

bool
GetNameIRGenerator::tryAttachGlobalNameGetter(ObjOperandId objId, HandleId id)
{
    if (!IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope())
        return false;

    Handle<LexicalEnvironmentObject*> globalLexical = env_.as<LexicalEnvironmentObject>();
    MOZ_ASSERT(globalLexical->isGlobal());

    RootedNativeObject holder(cx_);
    RootedShape shape(cx_);
    if (!CanAttachGlobalName(cx_, globalLexical, id, &holder, &shape))
        return false;

    if (holder == globalLexical)
        return false;

    if (!IsCacheableGetPropCallNative(&globalLexical->global(), holder, shape))
        return false;

    if (IsIonEnabled(cx_))
        EnsureTrackPropertyTypes(cx_, holder, id);

    // Shape guard for global lexical.
    writer.guardShape(objId, globalLexical->lastProperty());

    // Guard on the shape of the GlobalObject.
    ObjOperandId globalId = writer.loadEnclosingEnvironment(objId);
    writer.guardShape(globalId, globalLexical->global().lastProperty());

    if (holder != &globalLexical->global()) {
        // Shape guard holder.
        ObjOperandId holderId = writer.loadObject(holder);
        writer.guardShape(holderId, holder->lastProperty());
    }

    EmitCallGetterResultNoGuards(writer, &globalLexical->global(), holder, shape, globalId);

    trackAttached("GlobalNameGetter");
    return true;
}

static bool
NeedEnvironmentShapeGuard(JSObject* envObj)
{
    if (!envObj->is<CallObject>())
        return true;

    // We can skip a guard on the call object if the script's bindings are
    // guaranteed to be immutable (and thus cannot introduce shadowing
    // variables). The function might have been relazified under rare
    // conditions. In that case, we pessimistically create the guard.
    CallObject* callObj = &envObj->as<CallObject>();
    JSFunction* fun = &callObj->callee();
    if (!fun->hasScript() || fun->nonLazyScript()->funHasExtensibleScope())
        return true;

    return false;
}

bool
GetNameIRGenerator::tryAttachEnvironmentName(ObjOperandId objId, HandleId id)
{
    if (IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope())
        return false;

    RootedObject env(cx_, env_);
    RootedShape shape(cx_);
    RootedNativeObject holder(cx_);

    while (env) {
        if (env->is<GlobalObject>()) {
            shape = env->as<GlobalObject>().lookup(cx_, id);
            if (shape)
                break;
            return false;
        }

        if (!env->is<EnvironmentObject>() || env->is<WithEnvironmentObject>())
            return false;

        MOZ_ASSERT(!env->hasUncacheableProto());

        // Check for an 'own' property on the env. There is no need to
        // check the prototype as non-with scopes do not inherit properties
        // from any prototype.
        shape = env->as<NativeObject>().lookup(cx_, id);
        if (shape)
            break;

        env = env->enclosingEnvironment();
    }

    holder = &env->as<NativeObject>();
    if (!IsCacheableGetPropReadSlot(holder, holder, PropertyResult(shape)))
        return false;
    if (holder->getSlot(shape->slot()).isMagic())
        return false;

    ObjOperandId lastObjId = objId;
    env = env_;
    while (env) {
        if (NeedEnvironmentShapeGuard(env))
            writer.guardShape(lastObjId, env->maybeShape());

        if (env == holder)
            break;

        lastObjId = writer.loadEnclosingEnvironment(lastObjId);
        env = env->enclosingEnvironment();
    }

    if (holder->isFixedSlot(shape->slot())) {
        writer.loadEnvironmentFixedSlotResult(lastObjId, NativeObject::getFixedSlotOffset(shape->slot()));
    } else {
        size_t dynamicSlotOffset = holder->dynamicSlotIndex(shape->slot()) * sizeof(Value);
        writer.loadEnvironmentDynamicSlotResult(lastObjId, dynamicSlotOffset);
    }
    writer.typeMonitorResult();

    trackAttached("EnvironmentName");
    return true;
}

void
GetNameIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("base", ObjectValue(*env_));
        sp.valueProperty("property", StringValue(name_));
    }
#endif
}

BindNameIRGenerator::BindNameIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                                         ICState::Mode mode, HandleObject env,
                                         HandlePropertyName name)
  : IRGenerator(cx, script, pc, CacheKind::BindName, mode),
    env_(env),
    name_(name)
{}

bool
BindNameIRGenerator::tryAttachStub()
{
    MOZ_ASSERT(cacheKind_ == CacheKind::BindName);

    AutoAssertNoPendingException aanpe(cx_);

    ObjOperandId envId(writer.setInputOperandId(0));
    RootedId id(cx_, NameToId(name_));

    if (tryAttachGlobalName(envId, id))
        return true;
    if (tryAttachEnvironmentName(envId, id))
        return true;

    trackAttached(IRGenerator::NotAttached);
    return false;
}

bool
BindNameIRGenerator::tryAttachGlobalName(ObjOperandId objId, HandleId id)
{
    if (!IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope())
        return false;

    Handle<LexicalEnvironmentObject*> globalLexical = env_.as<LexicalEnvironmentObject>();
    MOZ_ASSERT(globalLexical->isGlobal());

    JSObject* result = nullptr;
    if (Shape* shape = globalLexical->lookup(cx_, id)) {
        // If this is an uninitialized lexical or a const, we need to return a
        // RuntimeLexicalErrorObject.
        if (globalLexical->getSlot(shape->slot()).isMagic() || !shape->writable())
            return false;
        result = globalLexical;
    } else {
        result = &globalLexical->global();
    }

    if (result == globalLexical) {
        // Lexical bindings are non-configurable so we can just return the
        // global lexical.
        writer.loadObjectResult(objId);
    } else {
        // If the property exists on the global and is non-configurable, it cannot be
        // shadowed by the lexical scope so we can just return the global without a
        // shape guard.
        Shape* shape = result->as<GlobalObject>().lookup(cx_, id);
        if (!shape || shape->configurable())
            writer.guardShape(objId, globalLexical->lastProperty());
        ObjOperandId globalId = writer.loadEnclosingEnvironment(objId);
        writer.loadObjectResult(globalId);
    }
    writer.returnFromIC();

    trackAttached("GlobalName");
    return true;
}

bool
BindNameIRGenerator::tryAttachEnvironmentName(ObjOperandId objId, HandleId id)
{
    if (IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope())
        return false;

    RootedObject env(cx_, env_);
    RootedShape shape(cx_);
    while (true) {
        if (!env->is<GlobalObject>() && !env->is<EnvironmentObject>())
            return false;
        if (env->is<WithEnvironmentObject>())
            return false;

        MOZ_ASSERT(!env->hasUncacheableProto());

        // When we reach an unqualified variables object (like the global) we
        // have to stop looking and return that object.
        if (env->isUnqualifiedVarObj())
            break;

        // Check for an 'own' property on the env. There is no need to
        // check the prototype as non-with scopes do not inherit properties
        // from any prototype.
        shape = env->as<NativeObject>().lookup(cx_, id);
        if (shape)
            break;

        env = env->enclosingEnvironment();
    }

    // If this is an uninitialized lexical or a const, we need to return a
    // RuntimeLexicalErrorObject.
    RootedNativeObject holder(cx_, &env->as<NativeObject>());
    if (shape &&
        holder->is<EnvironmentObject>() &&
        (holder->getSlot(shape->slot()).isMagic() || !shape->writable()))
    {
        return false;
    }

    ObjOperandId lastObjId = objId;
    env = env_;
    while (env) {
        if (NeedEnvironmentShapeGuard(env) && !env->is<GlobalObject>())
            writer.guardShape(lastObjId, env->maybeShape());

        if (env == holder)
            break;

        lastObjId = writer.loadEnclosingEnvironment(lastObjId);
        env = env->enclosingEnvironment();
    }
    writer.loadObjectResult(lastObjId);
    writer.returnFromIC();

    trackAttached("EnvironmentName");
    return true;
}

void
BindNameIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("base", ObjectValue(*env_));
        sp.valueProperty("property", StringValue(name_));
    }
#endif
}

HasPropIRGenerator::HasPropIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                                       CacheKind cacheKind, ICState::Mode mode,
                                       HandleValue idVal, HandleValue val)
  : IRGenerator(cx, script, pc, cacheKind, mode),
    val_(val),
    idVal_(idVal)
{ }

bool
HasPropIRGenerator::tryAttachDense(HandleObject obj, ObjOperandId objId,
                                   uint32_t index, Int32OperandId indexId)
{
    if (!obj->isNative())
        return false;

    NativeObject* nobj = &obj->as<NativeObject>();
    if (!nobj->containsDenseElement(index))
        return false;

    // Guard shape to ensure object class is NativeObject.
    TestMatchingNativeReceiver(writer, nobj, objId);
    writer.loadDenseElementExistsResult(objId, indexId);
    writer.returnFromIC();

    trackAttached("DenseHasProp");
    return true;
}

bool
HasPropIRGenerator::tryAttachDenseHole(HandleObject obj, ObjOperandId objId,
                                       uint32_t index, Int32OperandId indexId)
{
    bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

    if (!obj->isNative())
        return false;

    NativeObject* nobj = &obj->as<NativeObject>();
    if (nobj->containsDenseElement(index))
        return false;
    if (!CanAttachDenseElementHole(nobj, hasOwn))
        return false;

    // Guard shape to ensure class is NativeObject and to prevent non-dense
    // elements being added. Also ensures prototype doesn't change if dynamic
    // checks aren't emitted.
    TestMatchingNativeReceiver(writer, nobj, objId);

    // Generate prototype guards if needed. This includes monitoring that
    // properties were not added in the chain.
    if (!hasOwn)
        GeneratePrototypeHoleGuards(writer, nobj, objId);

    writer.loadDenseElementHoleExistsResult(objId, indexId);
    writer.returnFromIC();

    trackAttached("DenseHasPropHole");
    return true;
}

bool
HasPropIRGenerator::tryAttachSparse(HandleObject obj, ObjOperandId objId,
                                    Int32OperandId indexId)
{
    bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

    if (!obj->isNative())
        return false;
    if (!obj->as<NativeObject>().isIndexed())
        return false;
    if (!CanAttachDenseElementHole(&obj->as<NativeObject>(), hasOwn,
        /* allowIndexedReceiver = */ true))
    {
        return false;
    }

    // Guard that this is a native object.
    writer.guardIsNativeObject(objId);

    // Generate prototype guards if needed. This includes monitoring that
    // properties were not added in the chain.
    if (!hasOwn) {
        // If GeneratePrototypeHoleGuards below won't add guards for prototype,
        // we should add our own since we aren't guarding shape.
        if (!obj->hasUncacheableProto())
            GuardGroupProto(writer, obj, objId);

        GeneratePrototypeHoleGuards(writer, obj, objId);
    }

    // Because of the prototype guard we know that the prototype chain
    // does not include any dense or sparse (i.e indexed) properties.
    writer.callObjectHasSparseElementResult(objId, indexId);
    writer.returnFromIC();

    trackAttached("Sparse");
    return true;
}


bool
HasPropIRGenerator::tryAttachNamedProp(HandleObject obj, ObjOperandId objId,
                                       HandleId key, ValOperandId keyId)
{
    bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

    JSObject* holder = nullptr;
    PropertyResult prop;

    if (hasOwn) {
        if (!LookupOwnPropertyPure(cx_, obj, key, &prop))
            return false;

        holder = obj;
    } else {
        if (!LookupPropertyPure(cx_, obj, key, &holder, &prop))
            return false;
    }
    if (!prop)
        return false;

    if (tryAttachMegamorphic(objId, keyId))
        return true;
    if (tryAttachNative(obj, objId, key, keyId, prop, holder))
        return true;
    if (tryAttachUnboxed(obj, objId, key, keyId))
        return true;
    if (tryAttachTypedObject(obj, objId, key, keyId))
        return true;
    if (tryAttachUnboxedExpando(obj, objId, key, keyId))
        return true;

    return false;
}

bool
HasPropIRGenerator::tryAttachMegamorphic(ObjOperandId objId, ValOperandId keyId)
{
    bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

    if (mode_ != ICState::Mode::Megamorphic)
        return false;

    writer.megamorphicHasPropResult(objId, keyId, hasOwn);
    writer.returnFromIC();
    trackAttached("MegamorphicHasProp");
    return true;
}

bool
HasPropIRGenerator::tryAttachNative(JSObject* obj, ObjOperandId objId, jsid key,
                                    ValOperandId keyId, PropertyResult prop, JSObject* holder)
{
    if (!prop.isNativeProperty())
        return false;

    if (!IsCacheableProtoChain(obj, holder))
        return false;

    Maybe<ObjOperandId> tempId;
    emitIdGuard(keyId, key);
    EmitReadSlotGuard(writer, obj, holder, objId, &tempId);
    writer.loadBooleanResult(true);
    writer.returnFromIC();

    trackAttached("NativeHasProp");
    return true;
}

bool
HasPropIRGenerator::tryAttachUnboxed(JSObject* obj, ObjOperandId objId,
                                     jsid key, ValOperandId keyId)
{
    if (!obj->is<UnboxedPlainObject>())
        return false;

    const UnboxedLayout::Property* prop = obj->as<UnboxedPlainObject>().layout().lookup(key);
    if (!prop)
        return false;

    emitIdGuard(keyId, key);
    writer.guardGroupForLayout(objId, obj->group());
    writer.loadBooleanResult(true);
    writer.returnFromIC();

    trackAttached("UnboxedHasProp");
    return true;
}

bool
HasPropIRGenerator::tryAttachUnboxedExpando(JSObject* obj, ObjOperandId objId,
                                            jsid key, ValOperandId keyId)
{
    if (!obj->is<UnboxedPlainObject>())
        return false;

    UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando();
    if (!expando)
        return false;

    Shape* shape = expando->lookup(cx_, key);
    if (!shape)
        return false;

    Maybe<ObjOperandId> tempId;
    emitIdGuard(keyId, key);
    EmitReadSlotGuard(writer, obj, obj, objId, &tempId);
    writer.loadBooleanResult(true);
    writer.returnFromIC();

    trackAttached("UnboxedExpandoHasProp");
    return true;
}

bool
HasPropIRGenerator::tryAttachTypedArray(HandleObject obj, ObjOperandId objId,
                                        Int32OperandId indexId)
{
    if (!obj->is<TypedArrayObject>() && !IsPrimitiveArrayTypedObject(obj))
        return false;

    TypedThingLayout layout = GetTypedThingLayout(obj->getClass());

    if (IsPrimitiveArrayTypedObject(obj))
        writer.guardGroupForLayout(objId, obj->group());
    else
        writer.guardShapeForClass(objId, obj->as<TypedArrayObject>().shape());

    writer.loadTypedElementExistsResult(objId, indexId, layout);

    writer.returnFromIC();

    trackAttached("TypedArrayObject");
    return true;
}

bool
HasPropIRGenerator::tryAttachTypedObject(JSObject* obj, ObjOperandId objId,
                                         jsid key, ValOperandId keyId)
{
    if (!obj->is<TypedObject>())
        return false;

    if (!obj->as<TypedObject>().typeDescr().hasProperty(cx_->names(), key))
        return false;

    emitIdGuard(keyId, key);
    writer.guardGroupForLayout(objId, obj->group());
    writer.loadBooleanResult(true);
    writer.returnFromIC();

    trackAttached("TypedObjectHasProp");
    return true;
}

bool
HasPropIRGenerator::tryAttachSlotDoesNotExist(JSObject* obj, ObjOperandId objId,
                                              jsid key, ValOperandId keyId)
{
    bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

    emitIdGuard(keyId, key);
    if (hasOwn) {
        Maybe<ObjOperandId> tempId;
        TestMatchingReceiver(writer, obj, objId, &tempId);
    } else {
        Maybe<ObjOperandId> tempId;
        EmitReadSlotGuard(writer, obj, nullptr, objId, &tempId);
    }
    writer.loadBooleanResult(false);
    writer.returnFromIC();

    trackAttached("DoesNotExist");
    return true;
}

bool
HasPropIRGenerator::tryAttachDoesNotExist(HandleObject obj, ObjOperandId objId,
                                          HandleId key, ValOperandId keyId)
{
    bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

    // Check that property doesn't exist on |obj| or it's prototype chain. These
    // checks allow Native/Unboxed/Typed objects with a NativeObject prototype
    // chain. They return false if unknown such as resolve hooks or proxies.
    if (hasOwn) {
        if (!CheckHasNoSuchOwnProperty(cx_, obj, key))
            return false;
    } else {
        if (!CheckHasNoSuchProperty(cx_, obj, key))
            return false;
    }

    if (tryAttachMegamorphic(objId, keyId))
        return true;
    if (tryAttachSlotDoesNotExist(obj, objId, key, keyId))
        return true;

    return false;
}

bool
HasPropIRGenerator::tryAttachProxyElement(HandleObject obj, ObjOperandId objId,
                                          ValOperandId keyId)
{
    bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

    if (!obj->is<ProxyObject>())
        return false;

    writer.guardIsProxy(objId);
    writer.callProxyHasPropResult(objId, keyId, hasOwn);
    writer.returnFromIC();

    trackAttached("ProxyHasProp");
    return true;
}

bool
HasPropIRGenerator::tryAttachStub()
{
    MOZ_ASSERT(cacheKind_ == CacheKind::In ||
               cacheKind_ == CacheKind::HasOwn);

    AutoAssertNoPendingException aanpe(cx_);

    // NOTE: Argument order is PROPERTY, OBJECT
    ValOperandId keyId(writer.setInputOperandId(0));
    ValOperandId valId(writer.setInputOperandId(1));

    if (!val_.isObject()) {
        trackAttached(IRGenerator::NotAttached);
        return false;
    }
    RootedObject obj(cx_, &val_.toObject());
    ObjOperandId objId = writer.guardIsObject(valId);

    // Optimize Proxies
    if (tryAttachProxyElement(obj, objId, keyId))
        return true;

    RootedId id(cx_);
    bool nameOrSymbol;
    if (!ValueToNameOrSymbolId(cx_, idVal_, &id, &nameOrSymbol)) {
        cx_->clearPendingException();
        return false;
    }

    if (nameOrSymbol) {
        if (tryAttachNamedProp(obj, objId, id, keyId))
            return true;
        if (tryAttachDoesNotExist(obj, objId, id, keyId))
            return true;

        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    uint32_t index;
    Int32OperandId indexId;
    if (maybeGuardInt32Index(idVal_, keyId, &index, &indexId)) {
        if (tryAttachDense(obj, objId, index, indexId))
            return true;
        if (tryAttachDenseHole(obj, objId, index, indexId))
            return true;
        if (tryAttachTypedArray(obj, objId, indexId))
            return true;
        if (tryAttachSparse(obj, objId, indexId))
            return true;

        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    trackAttached(IRGenerator::NotAttached);
    return false;
}

void
HasPropIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("base", val_);
        sp.valueProperty("property", idVal_);
    }
#endif
}

bool
IRGenerator::maybeGuardInt32Index(const Value& index, ValOperandId indexId,
                                  uint32_t* int32Index, Int32OperandId* int32IndexId)
{
    if (index.isNumber()) {
        int32_t indexSigned;
        if (index.isInt32()) {
            indexSigned = index.toInt32();
        } else {
            // We allow negative zero here.
            if (!mozilla::NumberEqualsInt32(index.toDouble(), &indexSigned))
                return false;
            if (!cx_->runtime()->jitSupportsFloatingPoint)
                return false;
        }

        if (indexSigned < 0)
            return false;

        *int32Index = uint32_t(indexSigned);
        *int32IndexId = writer.guardIsInt32Index(indexId);
        return true;
    }

    if (index.isString()) {
        int32_t indexSigned = GetIndexFromString(index.toString());
        if (indexSigned < 0)
            return false;

        StringOperandId strId = writer.guardIsString(indexId);
        *int32Index = uint32_t(indexSigned);
        *int32IndexId = writer.guardAndGetIndexFromString(strId);
        return true;
    }

    return false;
}

SetPropIRGenerator::SetPropIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                                       CacheKind cacheKind, ICState::Mode mode,
                                       bool* isTemporarilyUnoptimizable,
                                       HandleValue lhsVal, HandleValue idVal, HandleValue rhsVal,
                                       bool needsTypeBarrier, bool maybeHasExtraIndexedProps)
  : IRGenerator(cx, script, pc, cacheKind, mode),
    lhsVal_(lhsVal),
    idVal_(idVal),
    rhsVal_(rhsVal),
    isTemporarilyUnoptimizable_(isTemporarilyUnoptimizable),
    typeCheckInfo_(cx, needsTypeBarrier),
    preliminaryObjectAction_(PreliminaryObjectAction::None),
    attachedTypedArrayOOBStub_(false),
    maybeHasExtraIndexedProps_(maybeHasExtraIndexedProps)
{}

bool
SetPropIRGenerator::tryAttachStub()
{
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
        return false;
    }

    if (lhsVal_.isObject()) {
        RootedObject obj(cx_, &lhsVal_.toObject());

        ObjOperandId objId = writer.guardIsObject(objValId);
        if (IsPropertySetOp(JSOp(*pc_))) {
            if (tryAttachMegamorphicSetElement(obj, objId, rhsValId))
                return true;
        }
        if (nameOrSymbol) {
            if (tryAttachNativeSetSlot(obj, objId, id, rhsValId))
                return true;
            if (tryAttachUnboxedExpandoSetSlot(obj, objId, id, rhsValId))
                return true;
            if (tryAttachUnboxedProperty(obj, objId, id, rhsValId))
                return true;
            if (tryAttachTypedObjectProperty(obj, objId, id, rhsValId))
                return true;
            if (IsPropertySetOp(JSOp(*pc_))) {
                if (tryAttachSetArrayLength(obj, objId, id, rhsValId))
                    return true;
                if (tryAttachSetter(obj, objId, id, rhsValId))
                    return true;
                if (tryAttachWindowProxy(obj, objId, id, rhsValId))
                    return true;
                if (tryAttachProxy(obj, objId, id, rhsValId))
                    return true;
            }
            return false;
        }

        if (IsPropertySetOp(JSOp(*pc_))) {
            if (tryAttachProxyElement(obj, objId, rhsValId))
                return true;
        }

        uint32_t index;
        Int32OperandId indexId;
        if (maybeGuardInt32Index(idVal_, setElemKeyValueId(), &index, &indexId)) {
            if (tryAttachSetDenseElement(obj, objId, index, indexId, rhsValId))
                return true;
            if (tryAttachSetDenseElementHole(obj, objId, index, indexId, rhsValId))
                return true;
            if (tryAttachSetTypedElement(obj, objId, index, indexId, rhsValId))
                return true;
            return false;
        }
        return false;
    }
    return false;
}

static void
EmitStoreSlotAndReturn(CacheIRWriter& writer, ObjOperandId objId, NativeObject* nobj, Shape* shape,
                       ValOperandId rhsId)
{
    if (nobj->isFixedSlot(shape->slot())) {
        size_t offset = NativeObject::getFixedSlotOffset(shape->slot());
        writer.storeFixedSlot(objId, offset, rhsId);
    } else {
        size_t offset = nobj->dynamicSlotIndex(shape->slot()) * sizeof(Value);
        writer.storeDynamicSlot(objId, offset, rhsId);
    }
    writer.returnFromIC();
}

static Shape*
LookupShapeForSetSlot(JSOp op, NativeObject* obj, jsid id)
{
    Shape* shape = obj->lookupPure(id);
    if (!shape || !shape->isDataProperty() || !shape->writable())
        return nullptr;

    // If this is an op like JSOP_INITELEM / [[DefineOwnProperty]], the
    // property's attributes may have to be changed too, so make sure it's a
    // simple data property.
    if (IsPropertyInitOp(op) && (!shape->configurable() || !shape->enumerable()))
        return nullptr;

    return shape;
}

static bool
CanAttachNativeSetSlot(JSContext* cx, JSOp op, HandleObject obj, HandleId id,
                       bool* isTemporarilyUnoptimizable, MutableHandleShape propShape)
{
    if (!obj->isNative())
        return false;

    propShape.set(LookupShapeForSetSlot(op, &obj->as<NativeObject>(), id));
    if (!propShape)
        return false;

    ObjectGroup* group = JSObject::getGroup(cx, obj);
    if (!group) {
        cx->recoverFromOutOfMemory();
        return false;
    }

    // For some property writes, such as the initial overwrite of global
    // properties, TI will not mark the property as having been
    // overwritten. Don't attach a stub in this case, so that we don't
    // execute another write to the property without TI seeing that write.
    EnsureTrackPropertyTypes(cx, obj, id);
    if (!PropertyHasBeenMarkedNonConstant(obj, id)) {
        *isTemporarilyUnoptimizable = true;
        return false;
    }

    return true;
}

bool
SetPropIRGenerator::tryAttachNativeSetSlot(HandleObject obj, ObjOperandId objId, HandleId id,
                                           ValOperandId rhsId)
{
    RootedShape propShape(cx_);
    if (!CanAttachNativeSetSlot(cx_, JSOp(*pc_), obj, id, isTemporarilyUnoptimizable_, &propShape))
        return false;

    if (mode_ == ICState::Mode::Megamorphic && cacheKind_ == CacheKind::SetProp) {
        writer.megamorphicStoreSlot(objId, JSID_TO_ATOM(id)->asPropertyName(), rhsId,
                                    typeCheckInfo_.needsTypeBarrier());
        writer.returnFromIC();
        trackAttached("MegamorphicNativeSlot");
        return true;
    }

    maybeEmitIdGuard(id);

    // If we need a property type barrier (always in Baseline, sometimes in
    // Ion), guard on both the shape and the group. If Ion knows the property
    // types match, we don't need the group guard.
    NativeObject* nobj = &obj->as<NativeObject>();
    if (typeCheckInfo_.needsTypeBarrier())
        writer.guardGroupForTypeBarrier(objId, nobj->group());
    TestMatchingNativeReceiver(writer, nobj, objId);

    if (IsPreliminaryObject(obj))
        preliminaryObjectAction_ = PreliminaryObjectAction::NotePreliminary;
    else
        preliminaryObjectAction_ = PreliminaryObjectAction::Unlink;

    typeCheckInfo_.set(nobj->group(), id);
    EmitStoreSlotAndReturn(writer, objId, nobj, propShape, rhsId);

    trackAttached("NativeSlot");
    return true;
}

bool
SetPropIRGenerator::tryAttachUnboxedExpandoSetSlot(HandleObject obj, ObjOperandId objId,
                                                   HandleId id, ValOperandId rhsId)
{
    if (!obj->is<UnboxedPlainObject>())
        return false;

    UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando();
    if (!expando)
        return false;

    Shape* propShape = LookupShapeForSetSlot(JSOp(*pc_), expando, id);
    if (!propShape)
        return false;

    maybeEmitIdGuard(id);

    Maybe<ObjOperandId> expandoId;
    TestMatchingReceiver(writer, obj, objId, &expandoId);

    // Property types must be added to the unboxed object's group, not the
    // expando's group (it has unknown properties).
    typeCheckInfo_.set(obj->group(), id);
    EmitStoreSlotAndReturn(writer, expandoId.ref(), expando, propShape, rhsId);

    trackAttached("UnboxedExpando");
    return true;
}

static void
EmitGuardUnboxedPropertyType(CacheIRWriter& writer, JSValueType propType, ValOperandId valId)
{
    if (propType == JSVAL_TYPE_OBJECT) {
        // Unboxed objects store NullValue as nullptr object.
        writer.guardIsObjectOrNull(valId);
    } else {
        writer.guardType(valId, propType);
    }
}

bool
SetPropIRGenerator::tryAttachUnboxedProperty(HandleObject obj, ObjOperandId objId, HandleId id,
                                             ValOperandId rhsId)
{
    if (!obj->is<UnboxedPlainObject>())
        return false;

    if (!cx_->runtime()->jitSupportsFloatingPoint)
        return false;

    const UnboxedLayout::Property* property = obj->as<UnboxedPlainObject>().layout().lookup(id);
    if (!property)
        return false;

    maybeEmitIdGuard(id);
    writer.guardGroupForLayout(objId, obj->group());
    EmitGuardUnboxedPropertyType(writer, property->type, rhsId);
    writer.storeUnboxedProperty(objId, property->type,
                                UnboxedPlainObject::offsetOfData() + property->offset,
                                rhsId);
    writer.returnFromIC();

    typeCheckInfo_.set(obj->group(), id);
    preliminaryObjectAction_ = PreliminaryObjectAction::Unlink;

    trackAttached("Unboxed");
    return true;
}

bool
SetPropIRGenerator::tryAttachTypedObjectProperty(HandleObject obj, ObjOperandId objId, HandleId id,
                                                 ValOperandId rhsId)
{
    if (!obj->is<TypedObject>())
        return false;

    if (!cx_->runtime()->jitSupportsFloatingPoint || cx_->compartment()->detachedTypedObjects)
        return false;

    if (!obj->as<TypedObject>().typeDescr().is<StructTypeDescr>())
        return false;

    StructTypeDescr* structDescr = &obj->as<TypedObject>().typeDescr().as<StructTypeDescr>();
    size_t fieldIndex;
    if (!structDescr->fieldIndex(id, &fieldIndex))
        return false;

    TypeDescr* fieldDescr = &structDescr->fieldDescr(fieldIndex);
    if (!fieldDescr->is<SimpleTypeDescr>())
        return false;

    uint32_t fieldOffset = structDescr->fieldOffset(fieldIndex);
    TypedThingLayout layout = GetTypedThingLayout(obj->getClass());

    maybeEmitIdGuard(id);
    writer.guardNoDetachedTypedObjects();
    writer.guardGroupForLayout(objId, obj->group());

    typeCheckInfo_.set(obj->group(), id);

    // Scalar types can always be stored without a type update stub.
    if (fieldDescr->is<ScalarTypeDescr>()) {
        Scalar::Type type = fieldDescr->as<ScalarTypeDescr>().type();
        writer.storeTypedObjectScalarProperty(objId, fieldOffset, layout, type, rhsId);
        writer.returnFromIC();

        trackAttached("TypedObject");
        return true;
    }

    // For reference types, guard on the RHS type first, so that
    // StoreTypedObjectReferenceProperty is infallible.
    ReferenceTypeDescr::Type type = fieldDescr->as<ReferenceTypeDescr>().type();
    switch (type) {
      case ReferenceTypeDescr::TYPE_ANY:
        break;
      case ReferenceTypeDescr::TYPE_OBJECT:
        writer.guardIsObjectOrNull(rhsId);
        break;
      case ReferenceTypeDescr::TYPE_STRING:
        writer.guardType(rhsId, JSVAL_TYPE_STRING);
        break;
    }

    writer.storeTypedObjectReferenceProperty(objId, fieldOffset, layout, type, rhsId);
    writer.returnFromIC();

    trackAttached("TypedObject");
    return true;
}

void
SetPropIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("base", lhsVal_);
        sp.valueProperty("property", idVal_);
        sp.valueProperty("value", rhsVal_);
    }
#endif
}

static bool
IsCacheableSetPropCallNative(JSObject* obj, JSObject* holder, Shape* shape)
{
    if (!shape || !IsCacheableProtoChain(obj, holder))
        return false;

    if (!shape->hasSetterValue())
        return false;

    if (!shape->setterObject() || !shape->setterObject()->is<JSFunction>())
        return false;

    JSFunction& setter = shape->setterObject()->as<JSFunction>();
    if (!setter.isNativeWithCppEntry())
        return false;

    if (setter.isClassConstructor())
        return false;

    if (setter.hasJitInfo() && !setter.jitInfo()->needsOuterizedThisObject())
        return true;

    return !IsWindow(obj);
}

static bool
IsCacheableSetPropCallScripted(JSObject* obj, JSObject* holder, Shape* shape,
                               bool* isTemporarilyUnoptimizable = nullptr)
{
    if (!shape || !IsCacheableProtoChain(obj, holder))
        return false;

    if (IsWindow(obj))
        return false;

    if (!shape->hasSetterValue())
        return false;

    if (!shape->setterObject() || !shape->setterObject()->is<JSFunction>())
        return false;

    JSFunction& setter = shape->setterObject()->as<JSFunction>();
    if (setter.isNativeWithCppEntry())
        return false;

    // Natives with jit entry can use the scripted path.
    if (setter.isNativeWithJitEntry())
        return true;

    if (!setter.hasScript()) {
        if (isTemporarilyUnoptimizable)
            *isTemporarilyUnoptimizable = true;
        return false;
    }

    if (setter.isClassConstructor())
        return false;

    return true;
}

static bool
CanAttachSetter(JSContext* cx, jsbytecode* pc, HandleObject obj, HandleId id,
                MutableHandleObject holder, MutableHandleShape propShape,
                bool* isTemporarilyUnoptimizable)
{
    // Don't attach a setter stub for ops like JSOP_INITELEM.
    MOZ_ASSERT(IsPropertySetOp(JSOp(*pc)));

    PropertyResult prop;
    if (!LookupPropertyPure(cx, obj, id, holder.address(), &prop))
        return false;

    if (prop.isNonNativeProperty())
        return false;

    propShape.set(prop.maybeShape());
    if (!IsCacheableSetPropCallScripted(obj, holder, propShape, isTemporarilyUnoptimizable) &&
        !IsCacheableSetPropCallNative(obj, holder, propShape))
    {
        return false;
    }

    return true;
}

static void
EmitCallSetterNoGuards(CacheIRWriter& writer, JSObject* obj, JSObject* holder,
                       Shape* shape, ObjOperandId objId, ValOperandId rhsId)
{
    if (IsCacheableSetPropCallNative(obj, holder, shape)) {
        JSFunction* target = &shape->setterValue().toObject().as<JSFunction>();
        MOZ_ASSERT(target->isNativeWithCppEntry());
        writer.callNativeSetter(objId, target, rhsId);
        writer.returnFromIC();
        return;
    }

    MOZ_ASSERT(IsCacheableSetPropCallScripted(obj, holder, shape));

    JSFunction* target = &shape->setterValue().toObject().as<JSFunction>();
    MOZ_ASSERT(target->hasJitEntry());
    writer.callScriptedSetter(objId, target, rhsId);
    writer.returnFromIC();
}

bool
SetPropIRGenerator::tryAttachSetter(HandleObject obj, ObjOperandId objId, HandleId id,
                                    ValOperandId rhsId)
{
    RootedObject holder(cx_);
    RootedShape propShape(cx_);
    if (!CanAttachSetter(cx_, pc_, obj, id, &holder, &propShape, isTemporarilyUnoptimizable_))
        return false;

    maybeEmitIdGuard(id);

    // Use the megamorphic guard if we're in megamorphic mode, except if |obj|
    // is a Window as GuardHasGetterSetter doesn't support this yet (Window may
    // require outerizing).
    if (mode_ == ICState::Mode::Specialized || IsWindow(obj)) {
        Maybe<ObjOperandId> expandoId;
        TestMatchingReceiver(writer, obj, objId, &expandoId);

        if (obj != holder) {
            GeneratePrototypeGuards(writer, obj, holder, objId);

            // Guard on the holder's shape.
            ObjOperandId holderId = writer.loadObject(holder);
            TestMatchingHolder(writer, holder, holderId);
        }
    } else {
        writer.guardHasGetterSetter(objId, propShape);
    }

    EmitCallSetterNoGuards(writer, obj, holder, propShape, objId, rhsId);

    trackAttached("Setter");
    return true;
}

bool
SetPropIRGenerator::tryAttachSetArrayLength(HandleObject obj, ObjOperandId objId, HandleId id,
                                            ValOperandId rhsId)
{
    // Don't attach an array length stub for ops like JSOP_INITELEM.
    MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

    if (!obj->is<ArrayObject>() ||
        !JSID_IS_ATOM(id, cx_->names().length) ||
        !obj->as<ArrayObject>().lengthIsWritable())
    {
        return false;
    }

    maybeEmitIdGuard(id);
    writer.guardClass(objId, GuardClassKind::Array);
    writer.callSetArrayLength(objId, IsStrictSetPC(pc_), rhsId);
    writer.returnFromIC();

    trackAttached("SetArrayLength");
    return true;
}

bool
SetPropIRGenerator::tryAttachSetDenseElement(HandleObject obj, ObjOperandId objId, uint32_t index,
                                             Int32OperandId indexId, ValOperandId rhsId)
{
    if (!obj->isNative())
        return false;

    NativeObject* nobj = &obj->as<NativeObject>();
    if (!nobj->containsDenseElement(index) || nobj->getElementsHeader()->isFrozen())
        return false;

    if (typeCheckInfo_.needsTypeBarrier())
        writer.guardGroupForTypeBarrier(objId, nobj->group());
    TestMatchingNativeReceiver(writer, nobj, objId);

    writer.storeDenseElement(objId, indexId, rhsId);
    writer.returnFromIC();

    // Type inference uses JSID_VOID for the element types.
    typeCheckInfo_.set(nobj->group(), JSID_VOID);

    trackAttached("SetDenseElement");
    return true;
}

static bool
CanAttachAddElement(JSObject* obj, bool isInit)
{
    // Make sure the objects on the prototype don't have any indexed properties
    // or that such properties can't appear without a shape change.
    do {
        // The first two checks are also relevant to the receiver object.
        if (obj->isNative() && obj->as<NativeObject>().isIndexed())
            return false;

        const Class* clasp = obj->getClass();
        if (clasp != &ArrayObject::class_ &&
            (clasp->getAddProperty() ||
             clasp->getResolve() ||
             clasp->getOpsLookupProperty() ||
             clasp->getOpsSetProperty()))
        {
            return false;
        }

        // If we're initializing a property instead of setting one, the objects
        // on the prototype are not relevant.
        if (isInit)
            break;

        JSObject* proto = obj->staticPrototype();
        if (!proto)
            break;

        if (!proto->isNative())
            return false;

        if (proto->as<NativeObject>().denseElementsAreFrozen())
            return false;

        obj = proto;
    } while (true);

    return true;
}

bool
SetPropIRGenerator::tryAttachSetDenseElementHole(HandleObject obj, ObjOperandId objId,
                                                 uint32_t index, Int32OperandId indexId,
                                                 ValOperandId rhsId)
{
    if (!obj->isNative() || rhsVal_.isMagic(JS_ELEMENTS_HOLE))
        return false;

    JSOp op = JSOp(*pc_);
    MOZ_ASSERT(IsPropertySetOp(op) || IsPropertyInitOp(op));

    if (op == JSOP_INITHIDDENELEM)
        return false;

    NativeObject* nobj = &obj->as<NativeObject>();
    if (!nobj->nonProxyIsExtensible())
        return false;

    MOZ_ASSERT(!nobj->getElementsHeader()->isFrozen(),
               "Extensible objects should not have frozen elements");

    uint32_t initLength = nobj->getDenseInitializedLength();

    // Optimize if we're adding an element at initLength or writing to a hole.
    // Don't handle the adding case if the current accesss is in bounds, to
    // ensure we always call noteArrayWriteHole.
    bool isAdd = index == initLength;
    bool isHoleInBounds = index < initLength && !nobj->containsDenseElement(index);
    if (!isAdd && !isHoleInBounds)
        return false;

    // Can't add new elements to arrays with non-writable length.
    if (isAdd && nobj->is<ArrayObject>() && !nobj->as<ArrayObject>().lengthIsWritable())
        return false;

    // Typed arrays don't have dense elements.
    if (nobj->is<TypedArrayObject>())
        return false;

    // Check for other indexed properties or class hooks.
    if (!CanAttachAddElement(nobj, IsPropertyInitOp(op)))
        return false;

    if (typeCheckInfo_.needsTypeBarrier())
        writer.guardGroupForTypeBarrier(objId, nobj->group());
    TestMatchingNativeReceiver(writer, nobj, objId);

    // Also shape guard the proto chain, unless this is an INITELEM or we know
    // the proto chain has no indexed props.
    if (IsPropertySetOp(op) && maybeHasExtraIndexedProps_)
        ShapeGuardProtoChain(writer, obj, objId);

    writer.storeDenseElementHole(objId, indexId, rhsId, isAdd);
    writer.returnFromIC();

    // Type inference uses JSID_VOID for the element types.
    typeCheckInfo_.set(nobj->group(), JSID_VOID);

    trackAttached(isAdd ? "AddDenseElement" : "StoreDenseElementHole");
    return true;
}

bool
SetPropIRGenerator::tryAttachSetTypedElement(HandleObject obj, ObjOperandId objId,
                                             uint32_t index, Int32OperandId indexId,
                                             ValOperandId rhsId)
{
    if (!obj->is<TypedArrayObject>() && !IsPrimitiveArrayTypedObject(obj))
        return false;

    if (!rhsVal_.isNumber())
        return false;

    if (!cx_->runtime()->jitSupportsFloatingPoint && TypedThingRequiresFloatingPoint(obj))
        return false;

    bool handleOutOfBounds = false;
    if (obj->is<TypedArrayObject>()) {
        handleOutOfBounds = (index >= obj->as<TypedArrayObject>().length());
    } else {
        // Typed objects throw on out of bounds accesses. Don't attach
        // a stub in this case.
        if (index >= obj->as<TypedObject>().length())
            return false;

        // Don't attach stubs if the underlying storage for typed objects
        // in the compartment could be detached, as the stub will always
        // bail out.
        if (cx_->compartment()->detachedTypedObjects)
            return false;
    }

    Scalar::Type elementType = TypedThingElementType(obj);
    TypedThingLayout layout = GetTypedThingLayout(obj->getClass());

    if (IsPrimitiveArrayTypedObject(obj)) {
        writer.guardNoDetachedTypedObjects();
        writer.guardGroupForLayout(objId, obj->group());
    } else {
        writer.guardShapeForClass(objId, obj->as<TypedArrayObject>().shape());
    }

    writer.storeTypedElement(objId, indexId, rhsId, layout, elementType, handleOutOfBounds);
    writer.returnFromIC();

    if (handleOutOfBounds)
        attachedTypedArrayOOBStub_ = true;

    trackAttached(handleOutOfBounds ? "SetTypedElementOOB" : "SetTypedElement");
    return true;
}

bool
SetPropIRGenerator::tryAttachGenericProxy(HandleObject obj, ObjOperandId objId, HandleId id,
                                          ValOperandId rhsId, bool handleDOMProxies)
{
    MOZ_ASSERT(obj->is<ProxyObject>());

    writer.guardIsProxy(objId);

    if (!handleDOMProxies) {
        // Ensure that the incoming object is not a DOM proxy, so that we can
        // get to the specialized stubs. If handleDOMProxies is true, we were
        // unable to attach a specialized DOM stub, so we just handle all
        // proxies here.
        writer.guardNotDOMProxy(objId);
    }

    if (cacheKind_ == CacheKind::SetProp || mode_ == ICState::Mode::Specialized) {
        maybeEmitIdGuard(id);
        writer.callProxySet(objId, id, rhsId, IsStrictSetPC(pc_));
    } else {
        // Attach a stub that handles every id.
        MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
        MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);
        writer.callProxySetByValue(objId, setElemKeyValueId(), rhsId, IsStrictSetPC(pc_));
    }

    writer.returnFromIC();

    trackAttached("GenericProxy");
    return true;
}

bool
SetPropIRGenerator::tryAttachDOMProxyShadowed(HandleObject obj, ObjOperandId objId, HandleId id,
                                              ValOperandId rhsId)
{
    MOZ_ASSERT(IsCacheableDOMProxy(obj));

    maybeEmitIdGuard(id);
    TestMatchingProxyReceiver(writer, &obj->as<ProxyObject>(), objId);
    writer.callProxySet(objId, id, rhsId, IsStrictSetPC(pc_));
    writer.returnFromIC();

    trackAttached("DOMProxyShadowed");
    return true;
}

bool
SetPropIRGenerator::tryAttachDOMProxyUnshadowed(HandleObject obj, ObjOperandId objId, HandleId id,
                                                ValOperandId rhsId)
{
    MOZ_ASSERT(IsCacheableDOMProxy(obj));

    RootedObject proto(cx_, obj->staticPrototype());
    if (!proto)
        return false;

    RootedObject holder(cx_);
    RootedShape propShape(cx_);
    if (!CanAttachSetter(cx_, pc_, proto, id, &holder, &propShape, isTemporarilyUnoptimizable_))
        return false;

    maybeEmitIdGuard(id);

    // Guard that our expando object hasn't started shadowing this property.
    TestMatchingProxyReceiver(writer, &obj->as<ProxyObject>(), objId);
    CheckDOMProxyExpandoDoesNotShadow(writer, obj, id, objId);

    GeneratePrototypeGuards(writer, obj, holder, objId);

    // Guard on the holder of the property.
    ObjOperandId holderId = writer.loadObject(holder);
    TestMatchingHolder(writer, holder, holderId);

    // EmitCallSetterNoGuards expects |obj| to be the object the property is
    // on to do some checks. Since we actually looked at proto, and no extra
    // guards will be generated, we can just pass that instead.
    EmitCallSetterNoGuards(writer, proto, holder, propShape, objId, rhsId);

    trackAttached("DOMProxyUnshadowed");
    return true;
}

bool
SetPropIRGenerator::tryAttachDOMProxyExpando(HandleObject obj, ObjOperandId objId, HandleId id,
                                             ValOperandId rhsId)
{
    MOZ_ASSERT(IsCacheableDOMProxy(obj));

    RootedValue expandoVal(cx_, GetProxyPrivate(obj));
    RootedObject expandoObj(cx_);
    if (expandoVal.isObject()) {
        expandoObj = &expandoVal.toObject();
    } else {
        MOZ_ASSERT(!expandoVal.isUndefined(),
                   "How did a missing expando manage to shadow things?");
        auto expandoAndGeneration = static_cast<ExpandoAndGeneration*>(expandoVal.toPrivate());
        MOZ_ASSERT(expandoAndGeneration);
        expandoObj = &expandoAndGeneration->expando.toObject();
    }

    RootedShape propShape(cx_);
    if (CanAttachNativeSetSlot(cx_, JSOp(*pc_), expandoObj, id, isTemporarilyUnoptimizable_,
                               &propShape))
    {
        maybeEmitIdGuard(id);
        ObjOperandId expandoObjId =
            guardDOMProxyExpandoObjectAndShape(obj, objId, expandoVal, expandoObj);

        NativeObject* nativeExpandoObj = &expandoObj->as<NativeObject>();
        writer.guardGroupForTypeBarrier(expandoObjId, nativeExpandoObj->group());
        typeCheckInfo_.set(nativeExpandoObj->group(), id);

        EmitStoreSlotAndReturn(writer, expandoObjId, nativeExpandoObj, propShape, rhsId);
        trackAttached("DOMProxyExpandoSlot");
        return true;
    }

    RootedObject holder(cx_);
    if (CanAttachSetter(cx_, pc_, expandoObj, id, &holder, &propShape,
                        isTemporarilyUnoptimizable_))
    {
        // Note that we don't actually use the expandoObjId here after the
        // shape guard. The DOM proxy (objId) is passed to the setter as
        // |this|.
        maybeEmitIdGuard(id);
        guardDOMProxyExpandoObjectAndShape(obj, objId, expandoVal, expandoObj);

        MOZ_ASSERT(holder == expandoObj);
        EmitCallSetterNoGuards(writer, expandoObj, expandoObj, propShape, objId, rhsId);
        trackAttached("DOMProxyExpandoSetter");
        return true;
    }

    return false;
}

bool
SetPropIRGenerator::tryAttachProxy(HandleObject obj, ObjOperandId objId, HandleId id,
                                   ValOperandId rhsId)
{
    // Don't attach a proxy stub for ops like JSOP_INITELEM.
    MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

    ProxyStubType type = GetProxyStubType(cx_, obj, id);
    if (type == ProxyStubType::None)
        return false;

    if (mode_ == ICState::Mode::Megamorphic)
        return tryAttachGenericProxy(obj, objId, id, rhsId, /* handleDOMProxies = */ true);

    switch (type) {
      case ProxyStubType::None:
        break;
      case ProxyStubType::DOMExpando:
        if (tryAttachDOMProxyExpando(obj, objId, id, rhsId))
            return true;
        if (*isTemporarilyUnoptimizable_) {
            // Scripted setter without JIT code. Just wait.
            return false;
        }
        MOZ_FALLTHROUGH; // Fall through to the generic shadowed case.
      case ProxyStubType::DOMShadowed:
        return tryAttachDOMProxyShadowed(obj, objId, id, rhsId);
      case ProxyStubType::DOMUnshadowed:
        if (tryAttachDOMProxyUnshadowed(obj, objId, id, rhsId))
            return true;
        if (*isTemporarilyUnoptimizable_) {
            // Scripted setter without JIT code. Just wait.
            return false;
        }
        return tryAttachGenericProxy(obj, objId, id, rhsId, /* handleDOMProxies = */ true);
      case ProxyStubType::Generic:
        return tryAttachGenericProxy(obj, objId, id, rhsId, /* handleDOMProxies = */ false);
    }

    MOZ_CRASH("Unexpected ProxyStubType");
}

bool
SetPropIRGenerator::tryAttachProxyElement(HandleObject obj, ObjOperandId objId, ValOperandId rhsId)
{
    // Don't attach a proxy stub for ops like JSOP_INITELEM.
    MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

    if (!obj->is<ProxyObject>())
        return false;

    writer.guardIsProxy(objId);

    // Like GetPropIRGenerator::tryAttachProxyElement, don't check for DOM
    // proxies here as we don't have specialized DOM stubs for this.
    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    writer.callProxySetByValue(objId, setElemKeyValueId(), rhsId, IsStrictSetPC(pc_));
    writer.returnFromIC();

    trackAttached("ProxyElement");
    return true;
}

bool
SetPropIRGenerator::tryAttachMegamorphicSetElement(HandleObject obj, ObjOperandId objId,
                                                   ValOperandId rhsId)
{
    MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

    if (mode_ != ICState::Mode::Megamorphic || cacheKind_ != CacheKind::SetElem)
        return false;

    // The generic proxy stubs are faster.
    if (obj->is<ProxyObject>())
        return false;

    writer.megamorphicSetElement(objId, setElemKeyValueId(), rhsId, IsStrictSetPC(pc_));
    writer.returnFromIC();

    trackAttached("MegamorphicSetElement");
    return true;
}

bool
SetPropIRGenerator::tryAttachWindowProxy(HandleObject obj, ObjOperandId objId, HandleId id,
                                         ValOperandId rhsId)
{
    // Attach a stub when the receiver is a WindowProxy and we can do the set
    // on the Window (the global object).

    if (!IsWindowProxy(obj))
        return false;

    // If we're megamorphic prefer a generic proxy stub that handles a lot more
    // cases.
    if (mode_ == ICState::Mode::Megamorphic)
        return false;

    // This must be a WindowProxy for the current Window/global. Else it would
    // be a cross-compartment wrapper and IsWindowProxy returns false for
    // those.
    MOZ_ASSERT(obj->getClass() == cx_->runtime()->maybeWindowProxyClass());
    MOZ_ASSERT(ToWindowIfWindowProxy(obj) == cx_->global());

    // Now try to do the set on the Window (the current global).
    Handle<GlobalObject*> windowObj = cx_->global();

    RootedShape propShape(cx_);
    if (!CanAttachNativeSetSlot(cx_, JSOp(*pc_), windowObj, id, isTemporarilyUnoptimizable_,
                                &propShape))
    {
        return false;
    }

    maybeEmitIdGuard(id);

    writer.guardClass(objId, GuardClassKind::WindowProxy);
    ObjOperandId windowObjId = writer.loadObject(windowObj);

    writer.guardShape(windowObjId, windowObj->lastProperty());
    writer.guardGroupForTypeBarrier(windowObjId, windowObj->group());
    typeCheckInfo_.set(windowObj->group(), id);

    EmitStoreSlotAndReturn(writer, windowObjId, windowObj, propShape, rhsId);

    trackAttached("WindowProxySlot");
    return true;
}

bool
SetPropIRGenerator::tryAttachAddSlotStub(HandleObjectGroup oldGroup, HandleShape oldShape)
{
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
        return false;
    }

    if (!lhsVal_.isObject() || !nameOrSymbol)
        return false;

    RootedObject obj(cx_, &lhsVal_.toObject());

    PropertyResult prop;
    JSObject* holder;
    if (!LookupPropertyPure(cx_, obj, id, &holder, &prop))
        return false;
    if (obj != holder)
        return false;

    Shape* propShape = nullptr;
    NativeObject* holderOrExpando = nullptr;

    if (obj->isNative()) {
        propShape = prop.shape();
        holderOrExpando = &obj->as<NativeObject>();
    } else {
        if (!obj->is<UnboxedPlainObject>())
            return false;
        UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando();
        if (!expando)
            return false;
        propShape = expando->lookupPure(id);
        if (!propShape)
            return false;
        holderOrExpando = expando;
    }

    MOZ_ASSERT(propShape);

    // The property must be the last added property of the object.
    if (holderOrExpando->lastProperty() != propShape)
        return false;

    // Object must be extensible, oldShape must be immediate parent of
    // current shape.
    if (!obj->nonProxyIsExtensible() || propShape->previous() != oldShape)
        return false;

    // Basic shape checks.
    if (propShape->inDictionary() ||
        !propShape->isDataProperty() ||
        !propShape->writable())
    {
        return false;
    }

    // Watch out for resolve hooks.
    if (ClassMayResolveId(cx_->names(), obj->getClass(), id, obj)) {
        // The JSFunction resolve hook defines a (non-configurable and
        // non-enumerable) |prototype| property on certain functions. Scripts
        // often assign a custom |prototype| object and we want to optimize
        // this |prototype| set and eliminate the default object allocation.
        //
        // We check group->maybeInterpretedFunction() here and guard on the
        // group. The group is unique for a particular function so this ensures
        // we don't add the default prototype property to functions that don't
        // have it.
        if (!obj->is<JSFunction>() ||
            !JSID_IS_ATOM(id, cx_->names().prototype) ||
            !oldGroup->maybeInterpretedFunction() ||
            !obj->as<JSFunction>().needsPrototypeProperty())
        {
            return false;
        }
        MOZ_ASSERT(!propShape->configurable());
        MOZ_ASSERT(!propShape->enumerable());
    }

    // Also watch out for addProperty hooks. Ignore the Array addProperty hook,
    // because it doesn't do anything for non-index properties.
    DebugOnly<uint32_t> index;
    MOZ_ASSERT_IF(obj->is<ArrayObject>(), !IdIsIndex(id, &index));
    if (!obj->is<ArrayObject>() && obj->getClass()->getAddProperty())
        return false;

    // Walk up the object prototype chain and ensure that all prototypes are
    // native, and that all prototypes have no setter defined on the property.
    for (JSObject* proto = obj->staticPrototype(); proto; proto = proto->staticPrototype()) {
        if (!proto->isNative())
            return false;

        // If prototype defines this property in a non-plain way, don't optimize.
        Shape* protoShape = proto->as<NativeObject>().lookup(cx_, id);
        if (protoShape && !protoShape->hasDefaultSetter())
            return false;

        // Otherwise, if there's no such property, watch out for a resolve hook
        // that would need to be invoked and thus prevent inlining of property
        // addition. Allow the JSFunction resolve hook as it only defines plain
        // data properties and we don't need to invoke it for objects on the
        // proto chain.
        if (ClassMayResolveId(cx_->names(), proto->getClass(), id, proto) &&
            !proto->is<JSFunction>())
        {
            return false;
        }
    }

    ObjOperandId objId = writer.guardIsObject(objValId);
    maybeEmitIdGuard(id);

    // In addition to guarding for type barrier, we need this group guard (or
    // shape guard below) to ensure class is unchanged.
    MOZ_ASSERT(!oldGroup->hasUncacheableClass() || obj->is<ShapedObject>());
    writer.guardGroupForTypeBarrier(objId, oldGroup);

    // If we are adding a property to an object for which the new script
    // properties analysis hasn't been performed yet, make sure the stub fails
    // after we run the analysis as a group change may be required here. The
    // group change is not required for correctness but improves type
    // information elsewhere.
    if (oldGroup->newScript() && !oldGroup->newScript()->analyzed()) {
        writer.guardGroupHasUnanalyzedNewScript(oldGroup);
        MOZ_ASSERT(IsPreliminaryObject(obj));
        preliminaryObjectAction_ = PreliminaryObjectAction::NotePreliminary;
    } else {
        preliminaryObjectAction_ = PreliminaryObjectAction::Unlink;
    }

    // Shape guard the holder.
    ObjOperandId holderId = objId;
    if (!obj->isNative()) {
        MOZ_ASSERT(obj->as<UnboxedPlainObject>().maybeExpando());
        holderId = writer.guardAndLoadUnboxedExpando(objId);
    }
    writer.guardShape(holderId, oldShape);

    ShapeGuardProtoChain(writer, obj, objId);

    ObjectGroup* newGroup = obj->group();

    // Check if we have to change the object's group. If we're adding an
    // unboxed expando property, we pass the expando object to AddAndStore*Slot.
    // That's okay because we only have to do a group change if the object is a
    // PlainObject.
    bool changeGroup = oldGroup != newGroup;
    MOZ_ASSERT_IF(changeGroup, obj->is<PlainObject>());

    if (holderOrExpando->isFixedSlot(propShape->slot())) {
        size_t offset = NativeObject::getFixedSlotOffset(propShape->slot());
        writer.addAndStoreFixedSlot(holderId, offset, rhsValId, propShape,
                                    changeGroup, newGroup);
        trackAttached("AddSlot");
    } else {
        size_t offset = holderOrExpando->dynamicSlotIndex(propShape->slot()) * sizeof(Value);
        uint32_t numOldSlots = NativeObject::dynamicSlotsCount(oldShape);
        uint32_t numNewSlots = NativeObject::dynamicSlotsCount(propShape);
        if (numOldSlots == numNewSlots) {
            writer.addAndStoreDynamicSlot(holderId, offset, rhsValId, propShape,
                                          changeGroup, newGroup);
            trackAttached("AddSlot");
        } else {
            MOZ_ASSERT(numNewSlots > numOldSlots);
            writer.allocateAndStoreDynamicSlot(holderId, offset, rhsValId, propShape,
                                               changeGroup, newGroup, numNewSlots);
            trackAttached("AllocateSlot");
        }
    }
    writer.returnFromIC();

    typeCheckInfo_.set(oldGroup, id);
    return true;
}

InstanceOfIRGenerator::InstanceOfIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                                            ICState::Mode mode, HandleValue lhs, HandleObject rhs)
  : IRGenerator(cx, script, pc, CacheKind::InstanceOf, mode),
    lhsVal_(lhs),
    rhsObj_(rhs)
{ }

bool
InstanceOfIRGenerator::tryAttachStub()
{
    MOZ_ASSERT(cacheKind_ == CacheKind::InstanceOf);
    AutoAssertNoPendingException aanpe(cx_);

    // Ensure RHS is a function -- could be a Proxy, which the IC isn't prepared to handle.
    if (!rhsObj_->is<JSFunction>()) {
        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    HandleFunction fun = rhsObj_.as<JSFunction>();

    if (fun->isBoundFunction()) {
        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    // If the user has supplied their own @@hasInstance method we shouldn't
    // clobber it.
    if (!js::FunctionHasDefaultHasInstance(fun, cx_->wellKnownSymbols())) {
        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    // Refuse to optimize any function whose [[Prototype]] isn't
    // Function.prototype.
    if (!fun->hasStaticPrototype() || fun->hasUncacheableProto()) {
        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    Value funProto = cx_->global()->getPrototype(JSProto_Function);
    if (!funProto.isObject() || fun->staticPrototype() != &funProto.toObject()) {
        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    // Ensure that the function's prototype slot is the same.
    Shape* shape = fun->lookupPure(cx_->names().prototype);
    if (!shape || !shape->isDataProperty()) {
        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    uint32_t slot = shape->slot();

    MOZ_ASSERT(fun->numFixedSlots() == 0, "Stub code relies on this");
    if (!fun->getSlot(slot).isObject()) {
        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    JSObject* prototypeObject = &fun->getSlot(slot).toObject();

    // Abstract Objects
    ValOperandId lhs(writer.setInputOperandId(0));
    ValOperandId rhs(writer.setInputOperandId(1));

    ObjOperandId rhsId = writer.guardIsObject(rhs);
    writer.guardShape(rhsId, fun->lastProperty());

    // Load prototypeObject into the cache -- consumed twice in the IC
    ObjOperandId protoId = writer.loadObject(prototypeObject);
    // Ensure that rhs[slot] == prototypeObject.
    writer.guardFunctionPrototype(rhsId, slot, protoId);

    // Needn't guard LHS is object, because the actual stub can handle that
    // and correctly return false.
    writer.loadInstanceOfObjectResult(lhs, protoId, slot);
    writer.returnFromIC();
    trackAttached("InstanceOf");
    return true;
}

void
InstanceOfIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("lhs", lhsVal_);
        sp.valueProperty("rhs", ObjectValue(*rhsObj_));
    }
#endif
}

TypeOfIRGenerator::TypeOfIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                                     ICState::Mode mode, HandleValue value)
  : IRGenerator(cx, script, pc, CacheKind::TypeOf, mode),
    val_(value)
{ }

bool
TypeOfIRGenerator::tryAttachStub()
{
    MOZ_ASSERT(cacheKind_ == CacheKind::TypeOf);

    AutoAssertNoPendingException aanpe(cx_);

    ValOperandId valId(writer.setInputOperandId(0));

    if (tryAttachPrimitive(valId))
        return true;

    MOZ_ALWAYS_TRUE(tryAttachObject(valId));
    return true;
}

bool
TypeOfIRGenerator::tryAttachPrimitive(ValOperandId valId)
{
    if (!val_.isPrimitive())
        return false;

    if (val_.isNumber())
        writer.guardIsNumber(valId);
    else
        writer.guardType(valId,  val_.extractNonDoubleType());

    writer.loadStringResult(TypeName(js::TypeOfValue(val_), cx_->names()));
    writer.returnFromIC();

    return true;
}

bool
TypeOfIRGenerator::tryAttachObject(ValOperandId valId)
{
    if (!val_.isObject())
        return false;

    ObjOperandId objId = writer.guardIsObject(valId);
    writer.loadTypeOfObjectResult(objId);
    writer.returnFromIC();

    return true;
}

GetIteratorIRGenerator::GetIteratorIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                                               ICState::Mode mode, HandleValue value)
  : IRGenerator(cx, script, pc, CacheKind::GetIterator, mode),
    val_(value)
{ }

bool
GetIteratorIRGenerator::tryAttachStub()
{
    MOZ_ASSERT(cacheKind_ == CacheKind::GetIterator);

    AutoAssertNoPendingException aanpe(cx_);

    if (mode_ == ICState::Mode::Megamorphic)
        return false;

    ValOperandId valId(writer.setInputOperandId(0));
    if (!val_.isObject())
        return false;

    RootedObject obj(cx_, &val_.toObject());

    ObjOperandId objId = writer.guardIsObject(valId);
    if (tryAttachNativeIterator(objId, obj))
        return true;

    return false;
}

bool
GetIteratorIRGenerator::tryAttachNativeIterator(ObjOperandId objId, HandleObject obj)
{
    MOZ_ASSERT(JSOp(*pc_) == JSOP_ITER);

    PropertyIteratorObject* iterobj = LookupInIteratorCache(cx_, obj);
    if (!iterobj)
        return false;

    MOZ_ASSERT(obj->isNative() || obj->is<UnboxedPlainObject>());

    // Guard on the receiver's shape/group.
    Maybe<ObjOperandId> expandoId;
    TestMatchingReceiver(writer, obj, objId, &expandoId);

    // Ensure the receiver or its expando object has no dense elements.
    if (obj->isNative())
        writer.guardNoDenseElements(objId);
    else if (expandoId)
        writer.guardNoDenseElements(*expandoId);

    // Do the same for the objects on the proto chain.
    GeneratePrototypeHoleGuards(writer, obj, objId);

    ObjOperandId iterId =
        writer.guardAndGetIterator(objId, iterobj, &cx_->compartment()->enumerators);
    writer.loadObjectResult(iterId);
    writer.returnFromIC();

    return true;
}

CallIRGenerator::CallIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, JSOp op,
                                 ICState::Mode mode, uint32_t argc,
                                 HandleValue callee, HandleValue thisval, HandleValueArray args)
  : IRGenerator(cx, script, pc, CacheKind::Call, mode),
    op_(op),
    argc_(argc),
    callee_(callee),
    thisval_(thisval),
    args_(args),
    typeCheckInfo_(cx, /* needsTypeBarrier = */ true),
    cacheIRStubKind_(BaselineCacheIRStubKind::Regular)
{ }

bool
CallIRGenerator::tryAttachStringSplit()
{
    // Only optimize StringSplitString(str, str)
    if (argc_ != 2 || !args_[0].isString() || !args_[1].isString())
        return false;

    // Just for now: if they're both atoms, then do not optimize using
    // CacheIR and allow the legacy "ConstStringSplit" BaselineIC optimization
    // to proceed.
    if (args_[0].toString()->isAtom() && args_[1].toString()->isAtom())
        return false;

    // Get the object group to use for this location.
    RootedObjectGroup group(cx_, ObjectGroupCompartment::getStringSplitStringGroup(cx_));
    if (!group)
        return false;

    AutoAssertNoPendingException aanpe(cx_);
    Int32OperandId argcId(writer.setInputOperandId(0));

    // Ensure argc == 1.
    writer.guardSpecificInt32Immediate(argcId, 2);

    // 2 arguments.  Stack-layout here is (bottom to top):
    //
    //  3: Callee
    //  2: ThisValue
    //  1: Arg0
    //  0: Arg1 <-- Top of stack

    // Ensure callee is the |String_split| native function.
    ValOperandId calleeValId = writer.loadStackValue(3);
    ObjOperandId calleeObjId = writer.guardIsObject(calleeValId);
    writer.guardIsNativeFunction(calleeObjId, js::intrinsic_StringSplitString);

    // Ensure arg0 is a string.
    ValOperandId arg0ValId = writer.loadStackValue(1);
    StringOperandId arg0StrId = writer.guardIsString(arg0ValId);

    // Ensure arg1 is a string.
    ValOperandId arg1ValId = writer.loadStackValue(0);
    StringOperandId arg1StrId = writer.guardIsString(arg1ValId);

    // Call custom string splitter VM-function.
    writer.callStringSplitResult(arg0StrId, arg1StrId, group);
    writer.typeMonitorResult();

    cacheIRStubKind_ = BaselineCacheIRStubKind::Monitored;
    trackAttached("StringSplitString");

    TypeScript::Monitor(cx_, script_, pc_, TypeSet::ObjectType(group));

    return true;
}

bool
CallIRGenerator::tryAttachArrayPush()
{
    // Only optimize on obj.push(val);
    if (argc_ != 1 || !thisval_.isObject())
        return false;

    // Where |obj| is a native array.
    RootedObject thisobj(cx_, &thisval_.toObject());
    if (!thisobj->is<ArrayObject>())
        return false;

    RootedArrayObject thisarray(cx_, &thisobj->as<ArrayObject>());

    // And the object group for the array is not collecting preliminary objects.
    if (thisobj->group()->maybePreliminaryObjects())
        return false;

    // Check for other indexed properties or class hooks.
    if (!CanAttachAddElement(thisobj, /* isInit = */ false))
        return false;

    // Can't add new elements to arrays with non-writable length.
    if (!thisarray->lengthIsWritable())
        return false;

    // Check that array is extensible.
    if (!thisarray->nonProxyIsExtensible())
        return false;

    MOZ_ASSERT(!thisarray->getElementsHeader()->isFrozen(),
               "Extensible arrays should not have frozen elements");
    MOZ_ASSERT(thisarray->lengthIsWritable());

    // After this point, we can generate code fine.

    // Generate code.
    AutoAssertNoPendingException aanpe(cx_);
    Int32OperandId argcId(writer.setInputOperandId(0));

    // Ensure argc == 1.
    writer.guardSpecificInt32Immediate(argcId, 1);

    // 1 argument only.  Stack-layout here is (bottom to top):
    //
    //  2: Callee
    //  1: ThisValue
    //  0: Arg0 <-- Top of stack.

    // Guard callee is the |js::array_push| native function.
    ValOperandId calleeValId = writer.loadStackValue(2);
    ObjOperandId calleeObjId = writer.guardIsObject(calleeValId);
    writer.guardIsNativeFunction(calleeObjId, js::array_push);

    // Guard this is an array object.
    ValOperandId thisValId = writer.loadStackValue(1);
    ObjOperandId thisObjId = writer.guardIsObject(thisValId);

    // This is a soft assert, documenting the fact that we pass 'true'
    // for needsTypeBarrier when constructing typeCheckInfo_ for CallIRGenerator.
    // Can be removed safely if the assumption becomes false.
    MOZ_ASSERT(typeCheckInfo_.needsTypeBarrier());

    // Guard that the group and shape matches.
    if (typeCheckInfo_.needsTypeBarrier())
        writer.guardGroupForTypeBarrier(thisObjId, thisobj->group());
    TestMatchingNativeReceiver(writer, thisarray, thisObjId);

    // Guard proto chain shapes.
    ShapeGuardProtoChain(writer, thisobj, thisObjId);

    // arr.push(x) is equivalent to arr[arr.length] = x for regular arrays.
    ValOperandId argId = writer.loadStackValue(0);
    writer.arrayPush(thisObjId, argId);

    writer.returnFromIC();

    // Set the type-check info, and the stub kind to Updated
    typeCheckInfo_.set(thisobj->group(), JSID_VOID);

    cacheIRStubKind_ = BaselineCacheIRStubKind::Updated;

    trackAttached("ArrayPush");
    return true;
}

bool
CallIRGenerator::tryAttachArrayJoin()
{
    // Only handle argc <= 1.
    if (argc_ > 1)
        return false;

    // Only optimize on obj.join(...);
    if (!thisval_.isObject())
        return false;

    // Where |obj| is a native array.
    RootedObject thisobj(cx_, &thisval_.toObject());
    if (!thisobj->is<ArrayObject>())
        return false;

    RootedArrayObject thisarray(cx_, &thisobj->as<ArrayObject>());

    // And the array is of length 0 or 1.
    if (thisarray->length() > 1)
        return false;

    // And the array is packed.
    if (thisarray->getDenseInitializedLength() != thisarray->length())
        return false;

    // We don't need to worry about indexed properties because we can perform
    // hole check manually.

    // Generate code.
    AutoAssertNoPendingException aanpe(cx_);
    Int32OperandId argcId(writer.setInputOperandId(0));

    // if 0 arguments:
    //  1: Callee
    //  0: ThisValue <-- Top of stack.
    //
    // if 1 argument:
    //  2: Callee
    //  1: ThisValue
    //  0: Arg0 [optional] <-- Top of stack.

    // Guard callee is the |js::array_join| native function.
    uint32_t calleeIndex = (argc_ == 0) ? 1 : 2;
    ValOperandId calleeValId = writer.loadStackValue(calleeIndex);
    ObjOperandId calleeObjId = writer.guardIsObject(calleeValId);
    writer.guardIsNativeFunction(calleeObjId, js::array_join);

    if (argc_ == 1) {
        // If argcount is 1, guard that the argument is a string.
        ValOperandId argValId = writer.loadStackValue(0);
        writer.guardIsString(argValId);
    }

    // Guard this is an array object.
    uint32_t thisIndex = (argc_ == 0) ? 0 : 1;
    ValOperandId thisValId = writer.loadStackValue(thisIndex);
    ObjOperandId thisObjId = writer.guardIsObject(thisValId);
    writer.guardClass(thisObjId, GuardClassKind::Array);

    // Do the join.
    writer.arrayJoinResult(thisObjId);

    writer.returnFromIC();

    // The result of this stub does not need to be monitored because it will
    // always return a string.  We will add String to the stack typeset when
    // attaching this stub.

    // Set the stub kind to Regular
    cacheIRStubKind_ = BaselineCacheIRStubKind::Regular;

    trackAttached("ArrayJoin");
    return true;
}

bool
CallIRGenerator::tryAttachStub()
{
    // Only optimize on JSOP_CALL or JSOP_CALL_IGNORES_RV.  No fancy business for now.
    if ((op_ != JSOP_CALL) && (op_ != JSOP_CALL_IGNORES_RV))
        return false;

    // Only optimize when the mode is Specialized.
    if (mode_ != ICState::Mode::Specialized)
        return false;

    // Ensure callee is a function.
    if (!callee_.isObject() || !callee_.toObject().is<JSFunction>())
        return false;

    RootedFunction calleeFunc(cx_, &callee_.toObject().as<JSFunction>());

    // Check for native-function optimizations.
    if (calleeFunc->isNative()) {
        if (calleeFunc->native() == js::intrinsic_StringSplitString) {
            if (tryAttachStringSplit())
                return true;
        }

        if (calleeFunc->native() == js::array_push) {
            if (tryAttachArrayPush())
                return true;
        }

        if (calleeFunc->native() == js::array_join) {
            if (tryAttachArrayJoin())
                return true;
        }
    }

    return false;
}

void
CallIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("callee", callee_);
        sp.valueProperty("thisval", thisval_);
        sp.valueProperty("argc", Int32Value(argc_));
    }
#endif
}

// Class which holds a shape pointer for use when caches might reference data in other zones.
static const Class shapeContainerClass = {
    "ShapeContainer",
    JSCLASS_HAS_RESERVED_SLOTS(1)
};

static const size_t SHAPE_CONTAINER_SLOT = 0;

JSObject*
jit::NewWrapperWithObjectShape(JSContext* cx, HandleNativeObject obj)
{
    MOZ_ASSERT(cx->compartment() != obj->compartment());

    RootedObject wrapper(cx);
    {
        AutoCompartment ac(cx, obj);
        wrapper = NewObjectWithClassProto(cx, &shapeContainerClass, nullptr);
        if (!obj)
            return nullptr;
        wrapper->as<NativeObject>().setSlot(SHAPE_CONTAINER_SLOT, PrivateGCThingValue(obj->lastProperty()));
    }
    if (!JS_WrapObject(cx, &wrapper))
        return nullptr;
    MOZ_ASSERT(IsWrapper(wrapper));
    return wrapper;
}

void
jit::LoadShapeWrapperContents(MacroAssembler& masm, Register obj, Register dst, Label* failure)
{
    masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), dst);
    Address privateAddr(dst, detail::ProxyReservedSlots::offsetOfPrivateSlot());
    masm.branchTestObject(Assembler::NotEqual, privateAddr, failure);
    masm.unboxObject(privateAddr, dst);
    masm.unboxNonDouble(Address(dst, NativeObject::getFixedSlotOffset(SHAPE_CONTAINER_SLOT)), dst,
                        JSVAL_TYPE_PRIVATE_GCTHING);
}

CompareIRGenerator::CompareIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                                       ICState::Mode mode, JSOp op,
                                       HandleValue lhsVal, HandleValue rhsVal)
  : IRGenerator(cx, script, pc, CacheKind::Compare, mode),
    op_(op), lhsVal_(lhsVal), rhsVal_(rhsVal)
{ }

bool
CompareIRGenerator::tryAttachString(ValOperandId lhsId, ValOperandId rhsId)
{
    MOZ_ASSERT(IsEqualityOp(op_));

    if (!lhsVal_.isString() || !rhsVal_.isString())
        return false;

    StringOperandId lhsStrId = writer.guardIsString(lhsId);
    StringOperandId rhsStrId = writer.guardIsString(rhsId);
    writer.compareStringResult(op_, lhsStrId, rhsStrId);
    writer.returnFromIC();

    trackAttached("String");
    return true;
}

bool
CompareIRGenerator::tryAttachObject(ValOperandId lhsId, ValOperandId rhsId)
{
    MOZ_ASSERT(IsEqualityOp(op_));

    if (!lhsVal_.isObject() || !rhsVal_.isObject())
        return false;

    ObjOperandId lhsObjId = writer.guardIsObject(lhsId);
    ObjOperandId rhsObjId = writer.guardIsObject(rhsId);
    writer.compareObjectResult(op_, lhsObjId, rhsObjId);
    writer.returnFromIC();

    trackAttached("Object");
    return true;
}

bool
CompareIRGenerator::tryAttachSymbol(ValOperandId lhsId, ValOperandId rhsId)
{
    MOZ_ASSERT(IsEqualityOp(op_));

    if (!lhsVal_.isSymbol() || !rhsVal_.isSymbol())
        return false;

    SymbolOperandId lhsSymId = writer.guardIsSymbol(lhsId);
    SymbolOperandId rhsSymId = writer.guardIsSymbol(rhsId);
    writer.compareSymbolResult(op_, lhsSymId, rhsSymId);
    writer.returnFromIC();

    trackAttached("Symbol");
    return true;
}

bool
CompareIRGenerator::tryAttachStrictDifferentTypes(ValOperandId lhsId, ValOperandId rhsId)
{
    MOZ_ASSERT(IsEqualityOp(op_));

    if (op_ != JSOP_STRICTEQ && op_ != JSOP_STRICTNE)
        return false;

    // Probably can't hit some of these.
    if (SameType(lhsVal_, rhsVal_) || (lhsVal_.isNumber() && rhsVal_.isNumber()))
        return false;

    // Compare tags
    ValueTagOperandId lhsTypeId = writer.loadValueTag(lhsId);
    ValueTagOperandId rhsTypeId = writer.loadValueTag(rhsId);
    writer.guardTagNotEqual(lhsTypeId, rhsTypeId);

    // Now that we've passed the guard, we know differing types, so return the bool result.
    writer.loadBooleanResult(op_ == JSOP_STRICTNE ? true : false);
    writer.returnFromIC();

    trackAttached("StrictDifferentTypes");
    return true;
}

bool
CompareIRGenerator::tryAttachStub()
{
    MOZ_ASSERT(cacheKind_ == CacheKind::Compare);
    MOZ_ASSERT(IsEqualityOp(op_) ||
               op_ == JSOP_LE || op_ == JSOP_LT ||
               op_ == JSOP_GE || op_ == JSOP_GT);

    AutoAssertNoPendingException aanpe(cx_);

    ValOperandId lhsId(writer.setInputOperandId(0));
    ValOperandId rhsId(writer.setInputOperandId(1));

    if (IsEqualityOp(op_)) {
        if (tryAttachString(lhsId, rhsId))
            return true;
        if (tryAttachObject(lhsId, rhsId))
            return true;
        if (tryAttachSymbol(lhsId, rhsId))
            return true;
        if (tryAttachStrictDifferentTypes(lhsId, rhsId))
            return true;

        trackAttached(IRGenerator::NotAttached);
        return false;
    }

    trackAttached(IRGenerator::NotAttached);
    return false;
}

void
CompareIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("lhs", lhsVal_);
        sp.valueProperty("rhs", rhsVal_);
    }
#endif
}

ToBoolIRGenerator::ToBoolIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, ICState::Mode mode,
                                     HandleValue val)
  : IRGenerator(cx, script, pc, CacheKind::ToBool, mode),
    val_(val)
{}

void
ToBoolIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("val", val_);
    }
#endif
}

bool
ToBoolIRGenerator::tryAttachStub()
{
    AutoAssertNoPendingException aanpe(cx_);

    if (tryAttachInt32())
        return true;
    if (tryAttachDouble())
        return true;
    if (tryAttachString())
        return true;
    if (tryAttachNullOrUndefined())
        return true;
    if (tryAttachObject())
        return true;
    if (tryAttachSymbol())
        return true;

    trackAttached(IRGenerator::NotAttached);
    return false;
}

bool
ToBoolIRGenerator::tryAttachInt32()
{
    if (!val_.isInt32())
        return false;

    ValOperandId valId(writer.setInputOperandId(0));
    writer.guardType(valId, JSVAL_TYPE_INT32);
    writer.loadInt32TruthyResult(valId);
    writer.returnFromIC();
    trackAttached("ToBoolInt32");
    return true;
}

bool
ToBoolIRGenerator::tryAttachDouble()
{
    if (!val_.isDouble() || !cx_->runtime()->jitSupportsFloatingPoint)
        return false;

    ValOperandId valId(writer.setInputOperandId(0));
    writer.guardType(valId, JSVAL_TYPE_DOUBLE);
    writer.loadDoubleTruthyResult(valId);
    writer.returnFromIC();
    trackAttached("ToBoolDouble");
    return true;
}

bool
ToBoolIRGenerator::tryAttachSymbol()
{
    if (!val_.isSymbol())
        return false;

    ValOperandId valId(writer.setInputOperandId(0));
    writer.guardType(valId, JSVAL_TYPE_SYMBOL);
    writer.loadBooleanResult(true);
    writer.returnFromIC();
    trackAttached("ToBoolSymbol");
    return true;
}

bool
ToBoolIRGenerator::tryAttachString()
{
    if (!val_.isString())
        return false;

    ValOperandId valId(writer.setInputOperandId(0));
    StringOperandId strId = writer.guardIsString(valId);
    writer.loadStringTruthyResult(strId);
    writer.returnFromIC();
    trackAttached("ToBoolString");
    return true;
}

bool
ToBoolIRGenerator::tryAttachNullOrUndefined()
{
    if (!val_.isNullOrUndefined())
        return false;

    ValOperandId valId(writer.setInputOperandId(0));
    writer.guardIsNullOrUndefined(valId);
    writer.loadBooleanResult(false);
    writer.returnFromIC();
    trackAttached("ToBoolNullOrUndefined");
    return true;
}

bool
ToBoolIRGenerator::tryAttachObject()
{
    if (!val_.isObject())
        return false;

    ValOperandId valId(writer.setInputOperandId(0));
    ObjOperandId objId = writer.guardIsObject(valId);
    writer.loadObjectTruthyResult(objId);
    writer.returnFromIC();
    trackAttached("ToBoolObject");
    return true;
}

GetIntrinsicIRGenerator::GetIntrinsicIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, ICState::Mode mode,
                                                 HandleValue val)
  : IRGenerator(cx, script, pc, CacheKind::GetIntrinsic, mode)
  , val_(val)
{}

void
GetIntrinsicIRGenerator::trackAttached(const char* name)
{
#ifdef JS_CACHEIR_SPEW
    if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
        sp.valueProperty("val", val_);
    }
#endif
}

bool
GetIntrinsicIRGenerator::tryAttachStub()
{
    writer.loadValueResult(val_);
    writer.returnFromIC();
    trackAttached("GetIntrinsic");
    return true;
}
