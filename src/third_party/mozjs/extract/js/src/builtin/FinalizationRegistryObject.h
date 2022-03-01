/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * FinalizationRegistry objects allow a program to register to receive a
 * callback after a 'target' object dies. The callback is passed a 'held value'
 * (that hopefully doesn't entrain the target). An 'unregister token' is an
 * object which can be used to remove multiple previous registrations in one go.
 *
 * To arrange this, the following data structures are used:
 *
 *   +---------------------------------------+-------------------------------+
 *   |   FinalizationRegistry compartment    |   Target zone / compartment   |
 *   |                                       |                               |
 *   |        +----------------------+       |     +------------------+      |
 *   |  +-----+ FinalizationRegistry |       |     |       Zone       |      |
 *   |  |     +----------+-----------+       |     +---------+--------+      |
 *   |  |                |                   |               |               |
 *   |  |                v                   |               v               |
 *   |  |  +-------------+-------------+     |  +------------+------------+  |
 *   |  |  |       Registrations       |     |  |  FinalizationRecordMap  |  |
 *   |  |  |         weak map          |     |  |           map           |  |
 *   |  |  +---------------------------+     |  +-------------------------+  |
 *   |  |  | Unregister  :   Records   |     |  |  Target  : Finalization |  |
 *   |  |  |   token     :   object    |     |  |  object  : RecordVector |  |
 *   |  |  +--------------------+------+     |  +----+-------------+------+  |
 *   |  |                       |            |       |             |         |
 *   |  |                       v            |       v             |         |
 *   |  |  +--------------------+------+     |  +----+-----+       |         |
 *   |  |  |       Finalization        |     |  |  Target  |       |         |
 *   |  |  |    RegistrationsObject    |     |  | JSObject |       |         |
 *   |  |  +---------------------------+     |  +----------+       |         |
 *   |  |  |       RecordVector        |     |                     |         |
 *   |  |  +-------------+-------------+     |                     |         |
 *   |  |                |                   |                     |         |
 *   |  |              * v                   |                     |         |
 *   |  |  +-------------+-------------+ *   |                     |         |
 *   |  |  | FinalizationRecordObject  +<--------------------------+         |
 *   |  |  +---------------------------+     |                               |
 *   |  |  | Queue                     +--+  |                               |
 *   |  |  +---------------------------+  |  |                               |
 *   |  |  | Held value                |  |  |                               |
 *   |  |  +---------------------------+  |  |                               |
 *   |  |                                 |  |                               |
 *   |  +--------------+   +--------------+  |                               |
 *   |                 |   |                 |                               |
 *   |                 v   v                 |                               |
 *   |      +----------+---+----------+      |                               |
 *   |      | FinalizationQueueObject |      |                               |
 *   |      +-------------------------+      |                               |
 *   |                                       |                               |
 *   +---------------------------------------+-------------------------------+
 *
 * A FinalizationRegistry consists of two parts: the FinalizationRegistry that
 * consumers see and a FinalizationQueue used internally to queue and call the
 * cleanup callbacks.
 *
 * Registering a target with a FinalizationRegistry creates a FinalizationRecord
 * containing a pointer to the queue and the heldValue. This is added to a
 * vector of records associated with the target, implemented as a map on the
 * target's Zone. All finalization records are treated as GC roots.
 *
 * When a target is registered an unregister token may be supplied. If so, this
 * is also recorded by the registry and is stored in a weak map of
 * registrations. The values of this map are FinalizationRegistrationsObject
 * objects. It's necessary to have another JSObject here because our weak map
 * implementation only supports JS types as values.
 *
 * When targets are unregistered, the registration is looked up in the weakmap
 * and the corresponding records are cleared.

 * The finalization record maps are swept during GC to check for records that
 * have been cleared by unregistration, for FinalizationRecords that are dead
 * and for nuked CCWs. In all cases the record is removed and the cleanup
 * callback is not run.
 *
 * Following this the targets are checked to see if they are dying. For such
 * targets the associated record list is processed and for each record the
 * heldValue is queued on the FinalizationQueue. At a later time this causes the
 * client's cleanup callback to be run.
 */

#ifndef builtin_FinalizationRegistryObject_h
#define builtin_FinalizationRegistryObject_h

#include "gc/Barrier.h"
#include "js/GCVector.h"
#include "vm/NativeObject.h"

namespace js {

class FinalizationRegistryObject;
class FinalizationRecordObject;
class FinalizationQueueObject;
class ObjectWeakMap;

using HandleFinalizationRegistryObject = Handle<FinalizationRegistryObject*>;
using HandleFinalizationRecordObject = Handle<FinalizationRecordObject*>;
using HandleFinalizationQueueObject = Handle<FinalizationQueueObject*>;
using RootedFinalizationRegistryObject = Rooted<FinalizationRegistryObject*>;
using RootedFinalizationRecordObject = Rooted<FinalizationRecordObject*>;
using RootedFinalizationQueueObject = Rooted<FinalizationQueueObject*>;

// A finalization record: a pair of finalization registry and held value.
//
// A finalization record represents the registered interest of a finalization
// registry in a target's finalization.
//
// Finalization records are initially 'active' but may be cleared and become
// inactive. This happens when:
//  - the heldValue is passed to the registry's cleanup callback
//  - the registry's unregister method removes the registration
class FinalizationRecordObject : public NativeObject {
  enum { QueueSlot = 0, HeldValueSlot, SlotCount };

 public:
  static const JSClass class_;

  static FinalizationRecordObject* create(JSContext* cx,
                                          HandleFinalizationQueueObject queue,
                                          HandleValue heldValue);

  FinalizationQueueObject* queue() const;
  Value heldValue() const;
  bool isActive() const;

  void clear();
};

// A vector of weakly-held FinalizationRecordObjects.
using WeakFinalizationRecordVector =
    GCVector<WeakHeapPtr<FinalizationRecordObject*>, 1, js::ZoneAllocPolicy>;

// A JS object containing a vector of weakly-held FinalizationRecordObjects,
// which holds the records corresponding to the registrations for a particular
// registration token. These are used as the values in the registration
// weakmap. Since the contents of the vector are weak references they are not
// traced.
class FinalizationRegistrationsObject : public NativeObject {
  enum { RecordsSlot = 0, SlotCount };

 public:
  static const JSClass class_;

  static FinalizationRegistrationsObject* create(JSContext* cx);

  WeakFinalizationRecordVector* records();
  const WeakFinalizationRecordVector* records() const;

  bool isEmpty() const;

  bool append(HandleFinalizationRecordObject record);
  void remove(HandleFinalizationRecordObject record);

  void sweep();

 private:
  static const JSClassOps classOps_;

  void* privatePtr() const;

  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JSFreeOp* fop, JSObject* obj);
};

using FinalizationRecordVector =
    GCVector<HeapPtr<FinalizationRecordObject*>, 1, js::ZoneAllocPolicy>;

// The JS FinalizationRegistry object itself.
class FinalizationRegistryObject : public NativeObject {
  enum { QueueSlot = 0, RegistrationsSlot, SlotCount };

 public:
  static const JSClass class_;
  static const JSClass protoClass_;

  FinalizationQueueObject* queue() const;
  ObjectWeakMap* registrations() const;

  void sweep();

  static bool unregisterRecord(FinalizationRecordObject* record);

  static bool cleanupQueuedRecords(JSContext* cx,
                                   HandleFinalizationRegistryObject registry,
                                   HandleObject callback = nullptr);

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
  static const JSFunctionSpec methods_[];
  static const JSPropertySpec properties_[];

  static bool construct(JSContext* cx, unsigned argc, Value* vp);
  static bool register_(JSContext* cx, unsigned argc, Value* vp);
  static bool unregister(JSContext* cx, unsigned argc, Value* vp);
  static bool cleanupSome(JSContext* cx, unsigned argc, Value* vp);

  static bool addRegistration(JSContext* cx,
                              HandleFinalizationRegistryObject registry,
                              HandleObject unregisterToken,
                              HandleFinalizationRecordObject record);
  static void removeRegistrationOnError(
      HandleFinalizationRegistryObject registry, HandleObject unregisterToken,
      HandleFinalizationRecordObject record);

  static bool preserveDOMWrapper(JSContext* cx, HandleObject obj);

  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JSFreeOp* fop, JSObject* obj);
};

// Contains information about the cleanup callback and the records queued to
// be cleaned up. This is not exposed to content JS.
class FinalizationQueueObject : public NativeObject {
  enum {
    CleanupCallbackSlot = 0,
    IncumbentObjectSlot,
    RecordsToBeCleanedUpSlot,
    IsQueuedForCleanupSlot,
    DoCleanupFunctionSlot,
    HasRegistrySlot,
    SlotCount
  };

  enum DoCleanupFunctionSlots {
    DoCleanupFunction_QueueSlot = 0,
  };

 public:
  static const JSClass class_;

  JSObject* cleanupCallback() const;
  JSObject* incumbentObject() const;
  FinalizationRecordVector* recordsToBeCleanedUp() const;
  bool isQueuedForCleanup() const;
  JSFunction* doCleanupFunction() const;
  bool hasRegistry() const;

  void queueRecordToBeCleanedUp(FinalizationRecordObject* record);
  void setQueuedForCleanup(bool value);

  void setHasRegistry(bool newValue);

  static FinalizationQueueObject* create(JSContext* cx,
                                         HandleObject cleanupCallback);

  static bool cleanupQueuedRecords(JSContext* cx,
                                   HandleFinalizationQueueObject registry,
                                   HandleObject callback = nullptr);

 private:
  static const JSClassOps classOps_;

  static bool doCleanup(JSContext* cx, unsigned argc, Value* vp);

  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JSFreeOp* fop, JSObject* obj);
};

}  // namespace js

#endif /* builtin_FinalizationRegistryObject_h */
