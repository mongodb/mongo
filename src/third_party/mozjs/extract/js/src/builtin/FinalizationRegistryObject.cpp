/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Implementation of JS FinalizationRegistry objects.

#include "builtin/FinalizationRegistryObject.h"

#include "mozilla/ScopeExit.h"

#include "jsapi.h"

#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"

#include "gc/GCContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

///////////////////////////////////////////////////////////////////////////
// FinalizationRecordObject

const JSClass FinalizationRecordObject::class_ = {
    "FinalizationRecord", JSCLASS_HAS_RESERVED_SLOTS(SlotCount)};

/* static */
FinalizationRecordObject* FinalizationRecordObject::create(
    JSContext* cx, HandleFinalizationQueueObject queue, HandleValue heldValue) {
  MOZ_ASSERT(queue);

  auto record = NewObjectWithGivenProto<FinalizationRecordObject>(cx, nullptr);
  if (!record) {
    return nullptr;
  }

  MOZ_ASSERT(queue->compartment() == record->compartment());

  record->initReservedSlot(QueueSlot, ObjectValue(*queue));
  record->initReservedSlot(HeldValueSlot, heldValue);
  record->initReservedSlot(InMapSlot, BooleanValue(false));

  return record;
}

FinalizationQueueObject* FinalizationRecordObject::queue() const {
  Value value = getReservedSlot(QueueSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  return &value.toObject().as<FinalizationQueueObject>();
}

Value FinalizationRecordObject::heldValue() const {
  return getReservedSlot(HeldValueSlot);
}

bool FinalizationRecordObject::isRegistered() const {
  MOZ_ASSERT_IF(!queue(), heldValue().isUndefined());
  return queue();
}

bool FinalizationRecordObject::isInRecordMap() const {
  return getReservedSlot(InMapSlot).toBoolean();
}

void FinalizationRecordObject::setInRecordMap(bool newValue) {
  MOZ_ASSERT(newValue != isInRecordMap());
  setReservedSlot(InMapSlot, BooleanValue(newValue));
}

void FinalizationRecordObject::clear() {
  MOZ_ASSERT(queue());
  setReservedSlot(QueueSlot, UndefinedValue());
  setReservedSlot(HeldValueSlot, UndefinedValue());
  MOZ_ASSERT(!isRegistered());
}

///////////////////////////////////////////////////////////////////////////
// FinalizationRegistrationsObject

const JSClass FinalizationRegistrationsObject::class_ = {
    "FinalizationRegistrations",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) | JSCLASS_BACKGROUND_FINALIZE,
    &classOps_, JS_NULL_CLASS_SPEC};

const JSClassOps FinalizationRegistrationsObject::classOps_ = {
    nullptr,                                    // addProperty
    nullptr,                                    // delProperty
    nullptr,                                    // enumerate
    nullptr,                                    // newEnumerate
    nullptr,                                    // resolve
    nullptr,                                    // mayResolve
    FinalizationRegistrationsObject::finalize,  // finalize
    nullptr,                                    // call
    nullptr,                                    // construct
    FinalizationRegistrationsObject::trace,     // trace
};

/* static */
FinalizationRegistrationsObject* FinalizationRegistrationsObject::create(
    JSContext* cx) {
  auto records = cx->make_unique<WeakFinalizationRecordVector>(cx->zone());
  if (!records) {
    return nullptr;
  }

  auto object =
      NewObjectWithGivenProto<FinalizationRegistrationsObject>(cx, nullptr);
  if (!object) {
    return nullptr;
  }

  InitReservedSlot(object, RecordsSlot, records.release(),
                   MemoryUse::FinalizationRecordVector);

  return object;
}

/* static */
void FinalizationRegistrationsObject::trace(JSTracer* trc, JSObject* obj) {
  if (!trc->traceWeakEdges()) {
    return;
  }

  auto* self = &obj->as<FinalizationRegistrationsObject>();
  if (WeakFinalizationRecordVector* records = self->records()) {
    TraceRange(trc, records->length(), records->begin(),
               "FinalizationRegistrationsObject records");
  }
}

/* static */
void FinalizationRegistrationsObject::finalize(JS::GCContext* gcx,
                                               JSObject* obj) {
  auto* self = &obj->as<FinalizationRegistrationsObject>();
  gcx->delete_(obj, self->records(), MemoryUse::FinalizationRecordVector);
}

inline WeakFinalizationRecordVector*
FinalizationRegistrationsObject::records() {
  return static_cast<WeakFinalizationRecordVector*>(privatePtr());
}

inline const WeakFinalizationRecordVector*
FinalizationRegistrationsObject::records() const {
  return static_cast<const WeakFinalizationRecordVector*>(privatePtr());
}

inline void* FinalizationRegistrationsObject::privatePtr() const {
  Value value = getReservedSlot(RecordsSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  void* ptr = value.toPrivate();
  MOZ_ASSERT(ptr);
  return ptr;
}

inline bool FinalizationRegistrationsObject::isEmpty() const {
  MOZ_ASSERT(records());
  return records()->empty();
}

inline bool FinalizationRegistrationsObject::append(
    HandleFinalizationRecordObject record) {
  MOZ_ASSERT(records());
  return records()->append(record);
}

inline void FinalizationRegistrationsObject::remove(
    HandleFinalizationRecordObject record) {
  MOZ_ASSERT(records());
  records()->eraseIfEqual(record);
}

inline bool FinalizationRegistrationsObject::traceWeak(JSTracer* trc) {
  MOZ_ASSERT(records());
  return records()->traceWeak(trc);
}

///////////////////////////////////////////////////////////////////////////
// FinalizationRegistryObject

// Bug 1600300: FinalizationRegistryObject is foreground finalized so that
// HeapPtr destructors never see referents with released arenas. When this is
// fixed we may be able to make this background finalized again.
const JSClass FinalizationRegistryObject::class_ = {
    "FinalizationRegistry",
    JSCLASS_HAS_CACHED_PROTO(JSProto_FinalizationRegistry) |
        JSCLASS_HAS_RESERVED_SLOTS(SlotCount) | JSCLASS_FOREGROUND_FINALIZE,
    &classOps_, &classSpec_};

const JSClass FinalizationRegistryObject::protoClass_ = {
    "FinalizationRegistry.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_FinalizationRegistry), JS_NULL_CLASS_OPS,
    &classSpec_};

const JSClassOps FinalizationRegistryObject::classOps_ = {
    nullptr,                               // addProperty
    nullptr,                               // delProperty
    nullptr,                               // enumerate
    nullptr,                               // newEnumerate
    nullptr,                               // resolve
    nullptr,                               // mayResolve
    FinalizationRegistryObject::finalize,  // finalize
    nullptr,                               // call
    nullptr,                               // construct
    FinalizationRegistryObject::trace,     // trace
};

const ClassSpec FinalizationRegistryObject::classSpec_ = {
    GenericCreateConstructor<construct, 1, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<FinalizationRegistryObject>,
    nullptr,
    nullptr,
    methods_,
    properties_};

const JSFunctionSpec FinalizationRegistryObject::methods_[] = {
    JS_FN("register", register_, 2, 0), JS_FN("unregister", unregister, 1, 0),
    JS_FN("cleanupSome", cleanupSome, 0, 0), JS_FS_END};

const JSPropertySpec FinalizationRegistryObject::properties_[] = {
    JS_STRING_SYM_PS(toStringTag, "FinalizationRegistry", JSPROP_READONLY),
    JS_PS_END};

/* static */
bool FinalizationRegistryObject::construct(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "FinalizationRegistry")) {
    return false;
  }

  RootedObject cleanupCallback(
      cx, ValueToCallable(cx, args.get(0), 1, NO_CONSTRUCT));
  if (!cleanupCallback) {
    return false;
  }

  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(
          cx, args, JSProto_FinalizationRegistry, &proto)) {
    return false;
  }

  Rooted<UniquePtr<ObjectWeakMap>> registrations(
      cx, cx->make_unique<ObjectWeakMap>(cx));
  if (!registrations) {
    return false;
  }

  RootedFinalizationQueueObject queue(
      cx, FinalizationQueueObject::create(cx, cleanupCallback));
  if (!queue) {
    return false;
  }

  RootedFinalizationRegistryObject registry(
      cx, NewObjectWithClassProto<FinalizationRegistryObject>(cx, proto));
  if (!registry) {
    return false;
  }

  registry->initReservedSlot(QueueSlot, ObjectValue(*queue));
  InitReservedSlot(registry, RegistrationsSlot, registrations.release(),
                   MemoryUse::FinalizationRegistryRegistrations);

  if (!cx->runtime()->gc.addFinalizationRegistry(cx, registry)) {
    return false;
  }

  queue->setHasRegistry(true);

  args.rval().setObject(*registry);
  return true;
}

/* static */
void FinalizationRegistryObject::trace(JSTracer* trc, JSObject* obj) {
  // Trace the registrations weak map. At most this traces the
  // FinalizationRegistrationsObject values of the map; the contents of those
  // objects are weakly held and are not traced by this method.

  auto* registry = &obj->as<FinalizationRegistryObject>();
  if (ObjectWeakMap* registrations = registry->registrations()) {
    registrations->trace(trc);
  }
}

void FinalizationRegistryObject::traceWeak(JSTracer* trc) {
  // Trace and update the contents of the registrations weak map's values, which
  // are weakly held.

  MOZ_ASSERT(registrations());
  for (ObjectValueWeakMap::Enum e(registrations()->valueMap()); !e.empty();
       e.popFront()) {
    auto* registrations =
        &e.front().value().toObject().as<FinalizationRegistrationsObject>();
    if (!registrations->traceWeak(trc)) {
      e.removeFront();
    }
  }
}

/* static */
void FinalizationRegistryObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  auto registry = &obj->as<FinalizationRegistryObject>();

  // The queue's flag should have been updated by
  // GCRuntime::sweepFinalizationRegistries.
  MOZ_ASSERT_IF(registry->queue(), !registry->queue()->hasRegistry());

  gcx->delete_(obj, registry->registrations(),
               MemoryUse::FinalizationRegistryRegistrations);
}

FinalizationQueueObject* FinalizationRegistryObject::queue() const {
  Value value = getReservedSlot(QueueSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  return &value.toObject().as<FinalizationQueueObject>();
}

ObjectWeakMap* FinalizationRegistryObject::registrations() const {
  Value value = getReservedSlot(RegistrationsSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  return static_cast<ObjectWeakMap*>(value.toPrivate());
}

// FinalizationRegistry.prototype.register(target, heldValue [, unregisterToken
// ])
// https://tc39.es/proposal-weakrefs/#sec-finalization-registry.prototype.register
/* static */
bool FinalizationRegistryObject::register_(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // 1. Let finalizationRegistry be the this value.
  // 2. If Type(finalizationRegistry) is not Object, throw a TypeError
  // exception.
  // 3. If finalizationRegistry does not have a [[Cells]] internal slot, throw a
  // TypeError exception.
  if (!args.thisv().isObject() ||
      !args.thisv().toObject().is<FinalizationRegistryObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_A_FINALIZATION_REGISTRY,
                              "Receiver of FinalizationRegistry.register call");
    return false;
  }

  RootedFinalizationRegistryObject registry(
      cx, &args.thisv().toObject().as<FinalizationRegistryObject>());

  // 4. If Type(target) is not Object, throw a TypeError exception.
  if (!args.get(0).isObject()) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_OBJECT_REQUIRED,
        "target argument to FinalizationRegistry.register");
    return false;
  }

  RootedObject target(cx, &args[0].toObject());

  // 5. If SameValue(target, heldValue), throw a TypeError exception.
  if (args.get(1).isObject() && &args.get(1).toObject() == target) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_HELD_VALUE);
    return false;
  }

  HandleValue heldValue = args.get(1);

  // 6. If Type(unregisterToken) is not Object,
  //    a. If unregisterToken is not undefined, throw a TypeError exception.
  if (!args.get(2).isUndefined() && !args.get(2).isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_UNREGISTER_TOKEN,
                              "FinalizationRegistry.register");
    return false;
  }

  RootedObject unregisterToken(cx);
  if (!args.get(2).isUndefined()) {
    unregisterToken = &args[2].toObject();
  }

  // Create the finalization record representing this target and heldValue.
  Rooted<FinalizationQueueObject*> queue(cx, registry->queue());
  Rooted<FinalizationRecordObject*> record(
      cx, FinalizationRecordObject::create(cx, queue, heldValue));
  if (!record) {
    return false;
  }

  // Add the record to the registrations if an unregister token was supplied.
  if (unregisterToken &&
      !addRegistration(cx, registry, unregisterToken, record)) {
    return false;
  }

  auto registrationsGuard = mozilla::MakeScopeExit([&] {
    if (unregisterToken) {
      removeRegistrationOnError(registry, unregisterToken, record);
    }
  });

  // Fully unwrap the target to pass it to the GC.
  RootedObject unwrappedTarget(cx);
  unwrappedTarget = CheckedUnwrapDynamic(target, cx);
  if (!unwrappedTarget) {
    ReportAccessDenied(cx);
    return false;
  }

  // If the target is a DOM wrapper, preserve it.
  if (!preserveDOMWrapper(cx, target)) {
    return false;
  }

  // Wrap the record into the compartment of the target.
  RootedObject wrappedRecord(cx, record);
  AutoRealm ar(cx, unwrappedTarget);
  if (!JS_WrapObject(cx, &wrappedRecord)) {
    return false;
  }

  if (JS_IsDeadWrapper(wrappedRecord)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
  }

  // Register the record with the target.
  gc::GCRuntime* gc = &cx->runtime()->gc;
  if (!gc->registerWithFinalizationRegistry(cx, unwrappedTarget,
                                            wrappedRecord)) {
    return false;
  }

  registrationsGuard.release();
  args.rval().setUndefined();
  return true;
}

/* static */
bool FinalizationRegistryObject::preserveDOMWrapper(JSContext* cx,
                                                    HandleObject obj) {
  if (!MaybePreserveDOMWrapper(cx, obj)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_FINALIZATION_REGISTRY_OBJECT);
    return false;
  }

  return true;
}

/* static */
bool FinalizationRegistryObject::addRegistration(
    JSContext* cx, HandleFinalizationRegistryObject registry,
    HandleObject unregisterToken, HandleFinalizationRecordObject record) {
  // Add the record to the list of records associated with this unregister
  // token.

  MOZ_ASSERT(unregisterToken);
  MOZ_ASSERT(registry->registrations());

  auto& map = *registry->registrations();
  Rooted<FinalizationRegistrationsObject*> recordsObject(cx);
  JSObject* obj = map.lookup(unregisterToken);
  if (obj) {
    recordsObject = &obj->as<FinalizationRegistrationsObject>();
  } else {
    recordsObject = FinalizationRegistrationsObject::create(cx);
    if (!recordsObject || !map.add(cx, unregisterToken, recordsObject)) {
      return false;
    }
  }

  if (!recordsObject->append(record)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

/* static */ void FinalizationRegistryObject::removeRegistrationOnError(
    HandleFinalizationRegistryObject registry, HandleObject unregisterToken,
    HandleFinalizationRecordObject record) {
  // Remove a registration if something went wrong before we added it to the
  // target zone's map. Note that this can't remove a registration after that
  // point.

  MOZ_ASSERT(unregisterToken);
  MOZ_ASSERT(registry->registrations());
  JS::AutoAssertNoGC nogc;

  auto& map = *registry->registrations();
  JSObject* obj = map.lookup(unregisterToken);
  MOZ_ASSERT(obj);
  auto records = &obj->as<FinalizationRegistrationsObject>();
  records->remove(record);

  if (records->empty()) {
    map.remove(unregisterToken);
  }
}

// FinalizationRegistry.prototype.unregister ( unregisterToken )
// https://tc39.es/proposal-weakrefs/#sec-finalization-registry.prototype.unregister
/* static */
bool FinalizationRegistryObject::unregister(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // 1. Let finalizationRegistry be the this value.
  // 2. If Type(finalizationRegistry) is not Object, throw a TypeError
  //    exception.
  // 3. If finalizationRegistry does not have a [[Cells]] internal slot, throw a
  //    TypeError exception.
  if (!args.thisv().isObject() ||
      !args.thisv().toObject().is<FinalizationRegistryObject>()) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NOT_A_FINALIZATION_REGISTRY,
        "Receiver of FinalizationRegistry.unregister call");
    return false;
  }

  RootedFinalizationRegistryObject registry(
      cx, &args.thisv().toObject().as<FinalizationRegistryObject>());

  // 4. If Type(unregisterToken) is not Object, throw a TypeError exception.
  if (!args.get(0).isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_UNREGISTER_TOKEN,
                              "FinalizationRegistry.unregister");
    return false;
  }

  RootedObject unregisterToken(cx, &args[0].toObject());

  // 5. Let removed be false.
  bool removed = false;

  // 6. For each Record { [[Target]], [[HeldValue]], [[UnregisterToken]] } cell
  //    that is an element of finalizationRegistry.[[Cells]], do
  //    a. If SameValue(cell.[[UnregisterToken]], unregisterToken) is true, then
  //       i. Remove cell from finalizationRegistry.[[Cells]].
  //       ii. Set removed to true.

  RootedObject obj(cx, registry->registrations()->lookup(unregisterToken));
  if (obj) {
    auto* records = obj->as<FinalizationRegistrationsObject>().records();
    MOZ_ASSERT(records);
    MOZ_ASSERT(!records->empty());
    for (FinalizationRecordObject* record : *records) {
      if (unregisterRecord(record)) {
        removed = true;
      }
    }
    registry->registrations()->remove(unregisterToken);
  }

  // 7. Return removed.
  args.rval().setBoolean(removed);
  return true;
}

/* static */
bool FinalizationRegistryObject::unregisterRecord(
    FinalizationRecordObject* record) {
  if (!record->isRegistered()) {
    return false;
  }

  // Clear the fields of this record; it will be removed from the target's
  // list when it is next swept.
  record->clear();
  return true;
}

// FinalizationRegistry.prototype.cleanupSome ( [ callback ] )
// https://tc39.es/proposal-weakrefs/#sec-finalization-registry.prototype.cleanupSome
bool FinalizationRegistryObject::cleanupSome(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // 1. Let finalizationRegistry be the this value.
  // 2. Perform ? RequireInternalSlot(finalizationRegistry, [[Cells]]).
  if (!args.thisv().isObject() ||
      !args.thisv().toObject().is<FinalizationRegistryObject>()) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NOT_A_FINALIZATION_REGISTRY,
        "Receiver of FinalizationRegistry.cleanupSome call");
    return false;
  }

  RootedFinalizationRegistryObject registry(
      cx, &args.thisv().toObject().as<FinalizationRegistryObject>());

  // 3. If callback is not undefined and IsCallable(callback) is false, throw a
  //    TypeError exception.
  RootedObject cleanupCallback(cx);
  if (!args.get(0).isUndefined()) {
    cleanupCallback = ValueToCallable(cx, args.get(0), -1, NO_CONSTRUCT);
    if (!cleanupCallback) {
      return false;
    }
  }

  RootedFinalizationQueueObject queue(cx, registry->queue());
  if (!FinalizationQueueObject::cleanupQueuedRecords(cx, queue,
                                                     cleanupCallback)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

///////////////////////////////////////////////////////////////////////////
// FinalizationQueueObject

// Bug 1600300: FinalizationQueueObject is foreground finalized so that
// HeapPtr destructors never see referents with released arenas. When this is
// fixed we may be able to make this background finalized again.
const JSClass FinalizationQueueObject::class_ = {
    "FinalizationQueue",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) | JSCLASS_FOREGROUND_FINALIZE,
    &classOps_};

const JSClassOps FinalizationQueueObject::classOps_ = {
    nullptr,                            // addProperty
    nullptr,                            // delProperty
    nullptr,                            // enumerate
    nullptr,                            // newEnumerate
    nullptr,                            // resolve
    nullptr,                            // mayResolve
    FinalizationQueueObject::finalize,  // finalize
    nullptr,                            // call
    nullptr,                            // construct
    FinalizationQueueObject::trace,     // trace
};

/* static */
FinalizationQueueObject* FinalizationQueueObject::create(
    JSContext* cx, HandleObject cleanupCallback) {
  MOZ_ASSERT(cleanupCallback);

  Rooted<UniquePtr<FinalizationRecordVector>> recordsToBeCleanedUp(
      cx, cx->make_unique<FinalizationRecordVector>(cx->zone()));
  if (!recordsToBeCleanedUp) {
    return nullptr;
  }

  Handle<PropertyName*> funName = cx->names().empty_;
  RootedFunction doCleanupFunction(
      cx, NewNativeFunction(cx, doCleanup, 0, funName,
                            gc::AllocKind::FUNCTION_EXTENDED));
  if (!doCleanupFunction) {
    return nullptr;
  }

  // It's problematic storing a CCW to a global in another compartment because
  // you don't know how far to unwrap it to get the original object
  // back. Instead store a CCW to a plain object in the same compartment as the
  // global (this uses Object.prototype).
  RootedObject incumbentObject(cx);
  if (!GetObjectFromIncumbentGlobal(cx, &incumbentObject) || !incumbentObject) {
    return nullptr;
  }

  FinalizationQueueObject* queue =
      NewObjectWithGivenProto<FinalizationQueueObject>(cx, nullptr);
  if (!queue) {
    return nullptr;
  }

  queue->initReservedSlot(CleanupCallbackSlot, ObjectValue(*cleanupCallback));
  queue->initReservedSlot(IncumbentObjectSlot, ObjectValue(*incumbentObject));
  InitReservedSlot(queue, RecordsToBeCleanedUpSlot,
                   recordsToBeCleanedUp.release(),
                   MemoryUse::FinalizationRegistryRecordVector);
  queue->initReservedSlot(IsQueuedForCleanupSlot, BooleanValue(false));
  queue->initReservedSlot(DoCleanupFunctionSlot,
                          ObjectValue(*doCleanupFunction));
  queue->initReservedSlot(HasRegistrySlot, BooleanValue(false));

  doCleanupFunction->setExtendedSlot(DoCleanupFunction_QueueSlot,
                                     ObjectValue(*queue));

  return queue;
}

/* static */
void FinalizationQueueObject::trace(JSTracer* trc, JSObject* obj) {
  auto queue = &obj->as<FinalizationQueueObject>();

  if (FinalizationRecordVector* records = queue->recordsToBeCleanedUp()) {
    records->trace(trc);
  }
}

/* static */
void FinalizationQueueObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  auto queue = &obj->as<FinalizationQueueObject>();

  gcx->delete_(obj, queue->recordsToBeCleanedUp(),
               MemoryUse::FinalizationRegistryRecordVector);
}

void FinalizationQueueObject::setHasRegistry(bool newValue) {
  MOZ_ASSERT(hasRegistry() != newValue);

  // Suppress our assertions about touching grey things. It's OK for us to set a
  // boolean slot even if this object is gray.
  AutoTouchingGrayThings atgt;

  setReservedSlot(HasRegistrySlot, BooleanValue(newValue));
}

bool FinalizationQueueObject::hasRegistry() const {
  return getReservedSlot(HasRegistrySlot).toBoolean();
}

inline JSObject* FinalizationQueueObject::cleanupCallback() const {
  Value value = getReservedSlot(CleanupCallbackSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  return &value.toObject();
}

JSObject* FinalizationQueueObject::incumbentObject() const {
  Value value = getReservedSlot(IncumbentObjectSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  return &value.toObject();
}

FinalizationRecordVector* FinalizationQueueObject::recordsToBeCleanedUp()
    const {
  Value value = getReservedSlot(RecordsToBeCleanedUpSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  return static_cast<FinalizationRecordVector*>(value.toPrivate());
}

bool FinalizationQueueObject::isQueuedForCleanup() const {
  return getReservedSlot(IsQueuedForCleanupSlot).toBoolean();
}

JSFunction* FinalizationQueueObject::doCleanupFunction() const {
  Value value = getReservedSlot(DoCleanupFunctionSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  return &value.toObject().as<JSFunction>();
}

void FinalizationQueueObject::queueRecordToBeCleanedUp(
    FinalizationRecordObject* record) {
  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!recordsToBeCleanedUp()->append(record)) {
    oomUnsafe.crash("FinalizationQueueObject::queueRecordsToBeCleanedUp");
  }
}

void FinalizationQueueObject::setQueuedForCleanup(bool value) {
  MOZ_ASSERT(value != isQueuedForCleanup());
  setReservedSlot(IsQueuedForCleanupSlot, BooleanValue(value));
}

/* static */
bool FinalizationQueueObject::doCleanup(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedFunction callee(cx, &args.callee().as<JSFunction>());

  Value value = callee->getExtendedSlot(DoCleanupFunction_QueueSlot);
  RootedFinalizationQueueObject queue(
      cx, &value.toObject().as<FinalizationQueueObject>());

  queue->setQueuedForCleanup(false);
  return cleanupQueuedRecords(cx, queue);
}

// CleanupFinalizationRegistry ( finalizationRegistry [ , callback ] )
// https://tc39.es/proposal-weakrefs/#sec-cleanup-finalization-registry
/* static */
bool FinalizationQueueObject::cleanupQueuedRecords(
    JSContext* cx, HandleFinalizationQueueObject queue,
    HandleObject callbackArg) {
  MOZ_ASSERT(cx->compartment() == queue->compartment());

  // 2. If callback is undefined, set callback to
  //    finalizationRegistry.[[CleanupCallback]].
  RootedValue callback(cx);
  if (callbackArg) {
    callback.setObject(*callbackArg);
  } else {
    JSObject* cleanupCallback = queue->cleanupCallback();
    MOZ_ASSERT(cleanupCallback);
    callback.setObject(*cleanupCallback);
  }

  // 3. While finalizationRegistry.[[Cells]] contains a Record cell such that
  //    cell.[[WeakRefTarget]] is empty, then an implementation may perform the
  //    following steps,
  //    a. Choose any such cell.
  //    b. Remove cell from finalizationRegistry.[[Cells]].
  //    c. Perform ? Call(callback, undefined, « cell.[[HeldValue]] »).

  RootedValue heldValue(cx);
  RootedValue rval(cx);
  FinalizationRecordVector* records = queue->recordsToBeCleanedUp();
  while (!records->empty()) {
    FinalizationRecordObject* record = records->popCopy();

    // Skip over records that have been unregistered.
    if (!record->isRegistered()) {
      continue;
    }

    heldValue.set(record->heldValue());

    record->clear();

    if (!Call(cx, callback, UndefinedHandleValue, heldValue, &rval)) {
      return false;
    }
  }

  return true;
}
