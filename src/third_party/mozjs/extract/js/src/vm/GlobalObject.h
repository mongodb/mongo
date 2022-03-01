/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GlobalObject_h
#define vm_GlobalObject_h

#include "mozilla/Assertions.h"

#include <stdint.h>
#include <type_traits>

#include "jsapi.h"
#include "jsexn.h"
#include "jsfriendapi.h"
#include "jspubtd.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "gc/AllocKind.h"
#include "gc/Rooting.h"
#include "js/CallArgs.h"
#include "js/Class.h"
#include "js/ErrorReport.h"
#include "js/PropertyDescriptor.h"
#include "js/RootingAPI.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/Realm.h"
#include "vm/Runtime.h"
#include "vm/Shape.h"
#include "vm/StringType.h"

struct JSFunctionSpec;
class JSJitInfo;
struct JSPrincipals;
struct JSPropertySpec;

namespace JS {
class JS_PUBLIC_API RealmOptions;
};

namespace js {

class GlobalScope;
class GlobalLexicalEnvironmentObject;
class PlainObject;
class RegExpStatics;

/*
 * Global object slots are reserved as follows:
 *
 * [0, APPLICATION_SLOTS)
 *   Pre-reserved slots in all global objects set aside for the embedding's
 *   use. As with all reserved slots these start out as UndefinedValue() and
 *   are traced for GC purposes. Apart from that the engine never touches
 *   these slots, so the embedding can do whatever it wants with them.
 * [APPLICATION_SLOTS, APPLICATION_SLOTS + JSProto_LIMIT)
 *   Stores the original value of the constructor for the corresponding
 *   JSProtoKey.
 * [APPLICATION_SLOTS + JSProto_LIMIT, APPLICATION_SLOTS + 2 * JSProto_LIMIT)
 *   Stores the prototype, if any, for the constructor for the corresponding
 *   JSProtoKey offset from JSProto_LIMIT.
 * [APPLICATION_SLOTS + 2 * JSProto_LIMIT, RESERVED_SLOTS)
 *   Various one-off values: ES5 13.2.3's [[ThrowTypeError]], RegExp statics,
 *   the original eval for this global object (implementing |var eval =
 *   otherWindow.eval; eval(...)| as an indirect eval), a bit indicating
 *   whether this object has been cleared (see JS_ClearScope), and a cache for
 *   whether eval is allowed (per the global's Content Security Policy).
 *
 * The two JSProto_LIMIT-sized ranges are necessary to implement
 * js::FindClassObject, and spec language speaking in terms of "the original
 * Array prototype object", or "as if by the expression new Array()" referring
 * to the original Array constructor. The actual (writable and even deletable)
 * Object, Array, &c. properties are not stored in reserved slots.
 */
class GlobalObject : public NativeObject {
  /* Count of slots set aside for application use. */
  static const unsigned APPLICATION_SLOTS = JSCLASS_GLOBAL_APPLICATION_SLOTS;

  /*
   * Count of slots to store built-in prototypes and initial visible
   * properties for the constructors.
   */
  static const unsigned STANDARD_CLASS_SLOTS = JSProto_LIMIT * 2;

  enum : unsigned {
    /* Various function values needed by the engine. */
    EVAL = APPLICATION_SLOTS + STANDARD_CLASS_SLOTS,
    THROWTYPEERROR,

    /* One-off properties stored after slots for built-ins. */
    EMPTY_GLOBAL_SCOPE,
    ITERATOR_PROTO,
    ARRAY_ITERATOR_PROTO,
    STRING_ITERATOR_PROTO,
    REGEXP_STRING_ITERATOR_PROTO,
    GENERATOR_OBJECT_PROTO,
    ASYNC_ITERATOR_PROTO,
    ASYNC_FROM_SYNC_ITERATOR_PROTO,
    ASYNC_GENERATOR_PROTO,
    MAP_ITERATOR_PROTO,
    SET_ITERATOR_PROTO,
    WRAP_FOR_VALID_ITERATOR_PROTO,
    ITERATOR_HELPER_PROTO,
    ASYNC_ITERATOR_HELPER_PROTO,
    MODULE_PROTO,
    IMPORT_ENTRY_PROTO,
    EXPORT_ENTRY_PROTO,
    REQUESTED_MODULE_PROTO,
    MODULE_REQUEST_PROTO,
    REGEXP_STATICS,
    RUNTIME_CODEGEN_ENABLED,
    INTRINSICS,
    FOR_OF_PIC_CHAIN,
    WINDOW_PROXY,
    GLOBAL_THIS_RESOLVED,
    SOURCE_URLS,
    REALM_KEY_OBJECT,
    ARRAY_SHAPE,

    /* Total reserved-slot count for global objects. */
    RESERVED_SLOTS
  };

  /*
   * The slot count must be in the public API for JSCLASS_GLOBAL_FLAGS, and
   * we won't expose GlobalObject, so just assert that the two values are
   * synchronized.
   */
  static_assert(JSCLASS_GLOBAL_SLOT_COUNT == RESERVED_SLOTS,
                "global object slot counts are inconsistent");

  static unsigned constructorSlot(JSProtoKey key) {
    MOZ_ASSERT(key < JSProto_LIMIT);
    return APPLICATION_SLOTS + key;
  }

  static unsigned prototypeSlot(JSProtoKey key) {
    MOZ_ASSERT(key < JSProto_LIMIT);
    return APPLICATION_SLOTS + JSProto_LIMIT + key;
  }

 public:
  GlobalLexicalEnvironmentObject& lexicalEnvironment() const;
  GlobalScope& emptyGlobalScope() const;

  void setOriginalEval(JSObject* evalobj) {
    MOZ_ASSERT(getSlotRef(EVAL).isUndefined());
    setSlot(EVAL, ObjectValue(*evalobj));
  }

  Value getConstructor(JSProtoKey key) const {
    return getSlot(constructorSlot(key));
  }
  static bool skipDeselectedConstructor(JSContext* cx, JSProtoKey key);
  static bool initBuiltinConstructor(JSContext* cx,
                                     Handle<GlobalObject*> global,
                                     JSProtoKey key, HandleObject ctor,
                                     HandleObject proto);

 private:
  enum class IfClassIsDisabled { DoNothing, Throw };

  static bool resolveConstructor(JSContext* cx, Handle<GlobalObject*> global,
                                 JSProtoKey key, IfClassIsDisabled mode);

 public:
  static bool ensureConstructor(JSContext* cx, Handle<GlobalObject*> global,
                                JSProtoKey key) {
    if (global->isStandardClassResolved(key)) {
      return true;
    }
    return resolveConstructor(cx, global, key, IfClassIsDisabled::Throw);
  }

  static JSObject* getOrCreateConstructor(JSContext* cx, JSProtoKey key) {
    MOZ_ASSERT(key != JSProto_Null);
    Handle<GlobalObject*> global = cx->global();
    if (!GlobalObject::ensureConstructor(cx, global, key)) {
      return nullptr;
    }
    return &global->getConstructor(key).toObject();
  }

  static JSObject* getOrCreatePrototype(JSContext* cx, JSProtoKey key) {
    MOZ_ASSERT(key != JSProto_Null);
    Handle<GlobalObject*> global = cx->global();
    if (!GlobalObject::ensureConstructor(cx, global, key)) {
      return nullptr;
    }
    return &global->getPrototype(key).toObject();
  }

  JSObject* maybeGetConstructor(JSProtoKey protoKey) const {
    MOZ_ASSERT(JSProto_Null < protoKey);
    MOZ_ASSERT(protoKey < JSProto_LIMIT);
    const Value& v = getConstructor(protoKey);
    return v.isObject() ? &v.toObject() : nullptr;
  }

  JSObject* maybeGetPrototype(JSProtoKey protoKey) const {
    MOZ_ASSERT(JSProto_Null < protoKey);
    MOZ_ASSERT(protoKey < JSProto_LIMIT);
    const Value& v = getPrototype(protoKey);
    return v.isObject() ? &v.toObject() : nullptr;
  }

  static bool maybeResolveGlobalThis(JSContext* cx,
                                     Handle<GlobalObject*> global,
                                     bool* resolved);

  void setConstructor(JSProtoKey key, const Value& v) {
    setSlot(constructorSlot(key), v);
  }

  Value getPrototype(JSProtoKey key) const {
    return getSlot(prototypeSlot(key));
  }

  void setPrototype(JSProtoKey key, const Value& value) {
    setSlot(prototypeSlot(key), value);
  }

  /*
   * Lazy standard classes need a way to indicate they have been initialized.
   * Otherwise, when we delete them, we might accidentally recreate them via
   * a lazy initialization. We use the presence of an object in the
   * getConstructor(key) reserved slot to indicate that they've been
   * initialized.
   *
   * Note: A few builtin objects, like JSON and Math, are not constructors,
   * so getConstructor is a bit of a misnomer.
   */
  bool isStandardClassResolved(JSProtoKey key) const {
    // If the constructor is undefined, then it hasn't been initialized.
    Value value = getConstructor(key);
    MOZ_ASSERT(value.isUndefined() || value.isObject() ||
               value.isMagic(JS_OFF_THREAD_CONSTRUCTOR));
    return !value.isUndefined();
  }

 private:
  bool classIsInitialized(JSProtoKey key) const {
    bool inited = !getConstructor(key).isUndefined();
    MOZ_ASSERT(inited == !getPrototype(key).isUndefined());
    return inited;
  }

  bool functionObjectClassesInitialized() const {
    bool inited = classIsInitialized(JSProto_Function);
    MOZ_ASSERT(inited == classIsInitialized(JSProto_Object));
    return inited;
  }

  // Disallow use of unqualified JSObject::create in GlobalObject.
  static GlobalObject* create(...) = delete;

  friend struct ::JSRuntime;
  static GlobalObject* createInternal(JSContext* cx, const JSClass* clasp);

 public:
  static GlobalObject* new_(JSContext* cx, const JSClass* clasp,
                            JSPrincipals* principals,
                            JS::OnNewGlobalHookOption hookOption,
                            const JS::RealmOptions& options);

  /*
   * Create a constructor function with the specified name and length using
   * ctor, a method which creates objects with the given class.
   */
  static JSFunction* createConstructor(
      JSContext* cx, JSNative ctor, JSAtom* name, unsigned length,
      gc::AllocKind kind = gc::AllocKind::FUNCTION,
      const JSJitInfo* jitInfo = nullptr);

  /*
   * Create an object to serve as [[Prototype]] for instances of the given
   * class, using |Object.prototype| as its [[Prototype]].  Users creating
   * prototype objects with particular internal structure (e.g. reserved
   * slots guaranteed to contain values of particular types) must immediately
   * complete the minimal initialization to make the returned object safe to
   * touch.
   */
  static NativeObject* createBlankPrototype(JSContext* cx,
                                            Handle<GlobalObject*> global,
                                            const JSClass* clasp);

  /*
   * Identical to createBlankPrototype, but uses proto as the [[Prototype]]
   * of the returned blank prototype.
   */
  static NativeObject* createBlankPrototypeInheriting(JSContext* cx,
                                                      const JSClass* clasp,
                                                      HandleObject proto);

  template <typename T>
  static T* createBlankPrototypeInheriting(JSContext* cx, HandleObject proto) {
    NativeObject* res = createBlankPrototypeInheriting(cx, &T::class_, proto);
    return res ? &res->template as<T>() : nullptr;
  }

  template <typename T>
  static T* createBlankPrototype(JSContext* cx, Handle<GlobalObject*> global) {
    NativeObject* res = createBlankPrototype(cx, global, &T::class_);
    return res ? &res->template as<T>() : nullptr;
  }

  static JSObject* getOrCreateObjectPrototype(JSContext* cx,
                                              Handle<GlobalObject*> global) {
    if (!global->functionObjectClassesInitialized()) {
      if (!ensureConstructor(cx, global, JSProto_Object)) {
        return nullptr;
      }
    }
    return &global->getPrototype(JSProto_Object).toObject();
  }

  static JSObject* getOrCreateFunctionConstructor(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!global->functionObjectClassesInitialized()) {
      if (!ensureConstructor(cx, global, JSProto_Object)) {
        return nullptr;
      }
    }
    return &global->getConstructor(JSProto_Function).toObject();
  }

  static JSObject* getOrCreateFunctionPrototype(JSContext* cx,
                                                Handle<GlobalObject*> global) {
    if (!global->functionObjectClassesInitialized()) {
      if (!ensureConstructor(cx, global, JSProto_Object)) {
        return nullptr;
      }
    }
    return &global->getPrototype(JSProto_Function).toObject();
  }

  static NativeObject* getOrCreateArrayPrototype(JSContext* cx,
                                                 Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_Array)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_Array).toObject().as<NativeObject>();
  }

  NativeObject* maybeGetArrayPrototype() {
    if (classIsInitialized(JSProto_Array)) {
      return &getPrototype(JSProto_Array).toObject().as<NativeObject>();
    }
    return nullptr;
  }

  static JSObject* getOrCreateBooleanPrototype(JSContext* cx,
                                               Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_Boolean)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_Boolean).toObject();
  }

  static JSObject* getOrCreateNumberPrototype(JSContext* cx,
                                              Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_Number)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_Number).toObject();
  }

  static JSObject* getOrCreateStringPrototype(JSContext* cx,
                                              Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_String)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_String).toObject();
  }

  static JSObject* getOrCreateSymbolPrototype(JSContext* cx,
                                              Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_Symbol)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_Symbol).toObject();
  }

  static JSObject* getOrCreateBigIntPrototype(JSContext* cx,
                                              Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_BigInt)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_BigInt).toObject();
  }

  static JSObject* getOrCreatePromisePrototype(JSContext* cx,
                                               Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_Promise)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_Promise).toObject();
  }

  static JSObject* getOrCreateRegExpPrototype(JSContext* cx,
                                              Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_RegExp)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_RegExp).toObject();
  }

  JSObject* maybeGetRegExpPrototype() {
    if (classIsInitialized(JSProto_RegExp)) {
      return &getPrototype(JSProto_RegExp).toObject();
    }
    return nullptr;
  }

  static JSObject* getOrCreateSavedFramePrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_SavedFrame)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_SavedFrame).toObject();
  }

  static JSObject* getOrCreateArrayBufferConstructor(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_ArrayBuffer)) {
      return nullptr;
    }
    return &global->getConstructor(JSProto_ArrayBuffer).toObject();
  }

  static JSObject* getOrCreateArrayBufferPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_ArrayBuffer)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_ArrayBuffer).toObject();
  }

  static JSObject* getOrCreateSharedArrayBufferPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_SharedArrayBuffer)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_SharedArrayBuffer).toObject();
  }

  static JSObject* getOrCreateCustomErrorPrototype(JSContext* cx,
                                                   Handle<GlobalObject*> global,
                                                   JSExnType exnType) {
    JSProtoKey key = GetExceptionProtoKey(exnType);
    if (!ensureConstructor(cx, global, key)) {
      return nullptr;
    }
    return &global->getPrototype(key).toObject();
  }

  static JSFunction* getOrCreateErrorConstructor(JSContext* cx,
                                                 Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_Error)) {
      return nullptr;
    }
    return &global->getConstructor(JSProto_Error).toObject().as<JSFunction>();
  }

  static JSObject* getOrCreateErrorPrototype(JSContext* cx,
                                             Handle<GlobalObject*> global) {
    return getOrCreateCustomErrorPrototype(cx, global, JSEXN_ERR);
  }

  static NativeObject* getOrCreateSetPrototype(JSContext* cx,
                                               Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_Set)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_Set).toObject().as<NativeObject>();
  }

  static NativeObject* getOrCreateWeakSetPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_WeakSet)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_WeakSet).toObject().as<NativeObject>();
  }

  static bool ensureModulePrototypesCreated(JSContext* cx,
                                            Handle<GlobalObject*> global,
                                            bool setUsedAsPrototype = false);

  static JSObject* getOrCreateModulePrototype(JSContext* cx,
                                              Handle<GlobalObject*> global) {
    return getOrCreateObject(cx, global, MODULE_PROTO, initModuleProto);
  }

  static JSObject* getOrCreateImportEntryPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    return getOrCreateObject(cx, global, IMPORT_ENTRY_PROTO,
                             initImportEntryProto);
  }

  static JSObject* getOrCreateExportEntryPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    return getOrCreateObject(cx, global, EXPORT_ENTRY_PROTO,
                             initExportEntryProto);
  }

  static JSObject* getOrCreateRequestedModulePrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    return getOrCreateObject(cx, global, REQUESTED_MODULE_PROTO,
                             initRequestedModuleProto);
  }

  static JSObject* getOrCreateModuleRequestPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    return getOrCreateObject(cx, global, MODULE_REQUEST_PROTO,
                             initModuleRequestProto);
  }

  static JSFunction* getOrCreateTypedArrayConstructor(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_TypedArray)) {
      return nullptr;
    }
    return &global->getConstructor(JSProto_TypedArray)
                .toObject()
                .as<JSFunction>();
  }

  static JSObject* getOrCreateTypedArrayPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_TypedArray)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_TypedArray).toObject();
  }

 private:
  using ObjectInitOp = bool (*)(JSContext*, Handle<GlobalObject*>);
  using ObjectInitWithTagOp = bool (*)(JSContext*, Handle<GlobalObject*>,
                                       HandleAtom);

  static JSObject* getOrCreateObject(JSContext* cx,
                                     Handle<GlobalObject*> global,
                                     unsigned slot, ObjectInitOp init) {
    Value v = global->getSlotRef(slot);
    if (v.isObject()) {
      return &v.toObject();
    }

    return createObject(cx, global, slot, init);
  }

  static JSObject* getOrCreateObject(JSContext* cx,
                                     Handle<GlobalObject*> global,
                                     unsigned slot, HandleAtom tag,
                                     ObjectInitWithTagOp init) {
    Value v = global->getSlotRef(slot);
    if (v.isObject()) {
      return &v.toObject();
    }

    return createObject(cx, global, slot, tag, init);
  }

  static JSObject* createObject(JSContext* cx, Handle<GlobalObject*> global,
                                unsigned slot, ObjectInitOp init);
  static JSObject* createObject(JSContext* cx, Handle<GlobalObject*> global,
                                unsigned slot, HandleAtom tag,
                                ObjectInitWithTagOp init);

  static JSObject* createIteratorPrototype(JSContext* cx,
                                           Handle<GlobalObject*> global);

 public:
  static JSObject* getOrCreateIteratorPrototype(JSContext* cx,
                                                Handle<GlobalObject*> global) {
    if (global->getReservedSlot(ITERATOR_PROTO).isObject()) {
      return &global->getReservedSlot(ITERATOR_PROTO).toObject();
    }
    return createIteratorPrototype(cx, global);
  }

  static NativeObject* getOrCreateArrayIteratorPrototype(
      JSContext* cx, Handle<GlobalObject*> global);

  NativeObject* maybeGetArrayIteratorPrototype() {
    Value v = getSlotRef(ARRAY_ITERATOR_PROTO);
    if (v.isObject()) {
      return &v.toObject().as<NativeObject>();
    }
    return nullptr;
  }

  static JSObject* getOrCreateStringIteratorPrototype(
      JSContext* cx, Handle<GlobalObject*> global);

  static JSObject* getOrCreateRegExpStringIteratorPrototype(
      JSContext* cx, Handle<GlobalObject*> global);

  void setGeneratorObjectPrototype(JSObject* obj) {
    setSlot(GENERATOR_OBJECT_PROTO, ObjectValue(*obj));
  }

  static JSObject* getOrCreateGeneratorObjectPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_GeneratorFunction)) {
      return nullptr;
    }
    return &global->getSlot(GENERATOR_OBJECT_PROTO).toObject();
  }

  static JSObject* getOrCreateGeneratorFunctionPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_GeneratorFunction)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_GeneratorFunction).toObject();
  }

  static JSObject* getOrCreateGeneratorFunction(JSContext* cx,
                                                Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_GeneratorFunction)) {
      return nullptr;
    }
    return &global->getConstructor(JSProto_GeneratorFunction).toObject();
  }

  static JSObject* getOrCreateAsyncFunctionPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_AsyncFunction)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_AsyncFunction).toObject();
  }

  static JSObject* getOrCreateAsyncFunction(JSContext* cx,
                                            Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_AsyncFunction)) {
      return nullptr;
    }
    return &global->getConstructor(JSProto_AsyncFunction).toObject();
  }

  static JSObject* createAsyncIteratorPrototype(JSContext* cx,
                                                Handle<GlobalObject*> global);

  static JSObject* getOrCreateAsyncIteratorPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (global->getReservedSlot(ASYNC_ITERATOR_PROTO).isObject()) {
      return &global->getReservedSlot(ASYNC_ITERATOR_PROTO).toObject();
    }
    return createAsyncIteratorPrototype(cx, global);
    // return getOrCreateObject(cx, global, ASYNC_ITERATOR_PROTO,
    //                         initAsyncIteratorProto);
  }

  static JSObject* getOrCreateAsyncFromSyncIteratorPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    return getOrCreateObject(cx, global, ASYNC_FROM_SYNC_ITERATOR_PROTO,
                             initAsyncFromSyncIteratorProto);
  }

  static JSObject* getOrCreateAsyncGenerator(JSContext* cx,
                                             Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_AsyncGeneratorFunction)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_AsyncGeneratorFunction).toObject();
  }

  static JSObject* getOrCreateAsyncGeneratorFunction(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_AsyncGeneratorFunction)) {
      return nullptr;
    }
    return &global->getConstructor(JSProto_AsyncGeneratorFunction).toObject();
  }

  void setAsyncGeneratorPrototype(JSObject* obj) {
    setSlot(ASYNC_GENERATOR_PROTO, ObjectValue(*obj));
  }

  static JSObject* getOrCreateAsyncGeneratorPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_AsyncGeneratorFunction)) {
      return nullptr;
    }
    return &global->getSlot(ASYNC_GENERATOR_PROTO).toObject();
  }

  static JSObject* getOrCreateMapIteratorPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    return getOrCreateObject(cx, global, MAP_ITERATOR_PROTO,
                             initMapIteratorProto);
  }

  static JSObject* getOrCreateSetIteratorPrototype(
      JSContext* cx, Handle<GlobalObject*> global) {
    return getOrCreateObject(cx, global, SET_ITERATOR_PROTO,
                             initSetIteratorProto);
  }

  static JSObject* getOrCreateDataViewPrototype(JSContext* cx,
                                                Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_DataView)) {
      return nullptr;
    }
    return &global->getPrototype(JSProto_DataView).toObject();
  }

  static JSObject* getOrCreatePromiseConstructor(JSContext* cx,
                                                 Handle<GlobalObject*> global) {
    if (!ensureConstructor(cx, global, JSProto_Promise)) {
      return nullptr;
    }
    return &global->getConstructor(JSProto_Promise).toObject();
  }

  static NativeObject* getOrCreateWrapForValidIteratorPrototype(
      JSContext* cx, Handle<GlobalObject*> global);

  static NativeObject* getOrCreateIteratorHelperPrototype(
      JSContext* cx, Handle<GlobalObject*> global);

  static NativeObject* getOrCreateAsyncIteratorHelperPrototype(
      JSContext* cx, Handle<GlobalObject*> global);
  static bool initAsyncIteratorHelperProto(JSContext* cx,
                                           Handle<GlobalObject*> global);

  static NativeObject* getIntrinsicsHolder(JSContext* cx,
                                           Handle<GlobalObject*> global);

  bool maybeExistingIntrinsicValue(PropertyName* name, Value* vp) {
    Value slot = getReservedSlot(INTRINSICS);
    // If we're in the self-hosting compartment itself, the
    // intrinsics-holder isn't initialized at this point.
    if (slot.isUndefined()) {
      *vp = UndefinedValue();
      return false;
    }

    NativeObject* holder = &slot.toObject().as<NativeObject>();
    mozilla::Maybe<PropertyInfo> prop = holder->lookupPure(name);
    if (prop.isNothing()) {
      *vp = UndefinedValue();
      return false;
    }

    *vp = holder->getSlot(prop->slot());
    return true;
  }

  static bool maybeGetIntrinsicValue(JSContext* cx,
                                     Handle<GlobalObject*> global,
                                     Handle<PropertyName*> name,
                                     MutableHandleValue vp, bool* exists) {
    NativeObject* holder = getIntrinsicsHolder(cx, global);
    if (!holder) {
      return false;
    }

    if (mozilla::Maybe<PropertyInfo> prop = holder->lookup(cx, name)) {
      vp.set(holder->getSlot(prop->slot()));
      *exists = true;
    } else {
      *exists = false;
    }

    return true;
  }

  static bool getIntrinsicValue(JSContext* cx, Handle<GlobalObject*> global,
                                HandlePropertyName name,
                                MutableHandleValue value) {
    bool exists = false;
    if (!GlobalObject::maybeGetIntrinsicValue(cx, global, name, value,
                                              &exists)) {
      return false;
    }
    if (exists) {
      return true;
    }
    return getIntrinsicValueSlow(cx, global, name, value);
  }

  static bool getIntrinsicValueSlow(JSContext* cx, Handle<GlobalObject*> global,
                                    HandlePropertyName name,
                                    MutableHandleValue value);

  static bool addIntrinsicValue(JSContext* cx, Handle<GlobalObject*> global,
                                HandlePropertyName name, HandleValue value);

  static inline bool setIntrinsicValue(JSContext* cx,
                                       Handle<GlobalObject*> global,
                                       HandlePropertyName name,
                                       HandleValue value);

  static bool getSelfHostedFunction(JSContext* cx, Handle<GlobalObject*> global,
                                    HandlePropertyName selfHostedName,
                                    HandleAtom name, unsigned nargs,
                                    MutableHandleValue funVal);

  static RegExpStatics* getRegExpStatics(JSContext* cx,
                                         Handle<GlobalObject*> global);

  static JSObject* getOrCreateThrowTypeError(JSContext* cx,
                                             Handle<GlobalObject*> global);

  static bool isRuntimeCodeGenEnabled(JSContext* cx, HandleString code,
                                      Handle<GlobalObject*> global);

  static bool getOrCreateEval(JSContext* cx, Handle<GlobalObject*> global,
                              MutableHandleObject eval);

  // Infallibly test whether the given value is the eval function for this
  // global.
  bool valueIsEval(const Value& val);

  // Implemented in vm/Iteration.cpp.
  static bool initIteratorProto(JSContext* cx, Handle<GlobalObject*> global);
  template <unsigned Slot, const JSClass* ProtoClass,
            const JSFunctionSpec* Methods>
  static bool initObjectIteratorProto(JSContext* cx,
                                      Handle<GlobalObject*> global,
                                      HandleAtom tag);

  // Implemented in vm/AsyncIteration.cpp.
  static bool initAsyncIteratorProto(JSContext* cx,
                                     Handle<GlobalObject*> global);
  static bool initAsyncFromSyncIteratorProto(JSContext* cx,
                                             Handle<GlobalObject*> global);

  // Implemented in builtin/MapObject.cpp.
  static bool initMapIteratorProto(JSContext* cx, Handle<GlobalObject*> global);
  static bool initSetIteratorProto(JSContext* cx, Handle<GlobalObject*> global);

  // Implemented in builtin/ModuleObject.cpp
  static bool initModuleProto(JSContext* cx, Handle<GlobalObject*> global);
  static bool initImportEntryProto(JSContext* cx, Handle<GlobalObject*> global);
  static bool initExportEntryProto(JSContext* cx, Handle<GlobalObject*> global);
  static bool initRequestedModuleProto(JSContext* cx,
                                       Handle<GlobalObject*> global);
  static bool initModuleRequestProto(JSContext* cx,
                                     Handle<GlobalObject*> global);

  static bool initStandardClasses(JSContext* cx, Handle<GlobalObject*> global);
  static bool initSelfHostingBuiltins(JSContext* cx,
                                      Handle<GlobalObject*> global,
                                      const JSFunctionSpec* builtins);

  Realm::DebuggerVector& getDebuggers() const {
    return realm()->getDebuggers();
  }

  inline NativeObject* getForOfPICObject() {
    Value forOfPIC = getReservedSlot(FOR_OF_PIC_CHAIN);
    if (forOfPIC.isUndefined()) {
      return nullptr;
    }
    return &forOfPIC.toObject().as<NativeObject>();
  }
  static NativeObject* getOrCreateForOfPICObject(JSContext* cx,
                                                 Handle<GlobalObject*> global);

  JSObject* windowProxy() const {
    return &getReservedSlot(WINDOW_PROXY).toObject();
  }
  JSObject* maybeWindowProxy() const {
    Value v = getReservedSlot(WINDOW_PROXY);
    MOZ_ASSERT(v.isObject() || v.isUndefined());
    return v.isObject() ? &v.toObject() : nullptr;
  }
  void setWindowProxy(JSObject* windowProxy) {
    setReservedSlot(WINDOW_PROXY, ObjectValue(*windowProxy));
  }

  JSObject* getSourceURLsHolder() const {
    Value v = getReservedSlot(SOURCE_URLS);
    MOZ_ASSERT(v.isObject() || v.isUndefined());
    return v.isObject() ? &v.toObject() : nullptr;
  }
  void setSourceURLsHolder(JSObject* holder) {
    setReservedSlot(SOURCE_URLS, ObjectValue(*holder));
  }
  void clearSourceURLSHolder() {
    // This is called at the start of shrinking GCs, so avoids barriers.
    getSlotRef(SOURCE_URLS).unbarrieredSet(UndefinedValue());
  }

  void setArrayShape(Shape* shape) {
    MOZ_ASSERT(getSlot(ARRAY_SHAPE).isUndefined());
    initSlot(ARRAY_SHAPE, PrivateGCThingValue(shape));
  }
  Shape* maybeArrayShape() const {
    Value v = getSlot(ARRAY_SHAPE);
    MOZ_ASSERT(v.isUndefined() || v.isPrivateGCThing());
    return v.isPrivateGCThing() ? v.toGCThing()->as<Shape>() : nullptr;
  }

  // Returns an object that represents the realm, used by embedder.
  static JSObject* getOrCreateRealmKeyObject(JSContext* cx,
                                             Handle<GlobalObject*> global);

  // A class used in place of a prototype during off-thread parsing.
  struct OffThreadPlaceholderObject : public NativeObject {
    static const int32_t SlotIndexSlot = 0;
    static const JSClass class_;
    static OffThreadPlaceholderObject* New(JSContext* cx, unsigned slot);
    inline int32_t getSlotIndex() const;
  };

  static bool isOffThreadPrototypePlaceholder(JSObject* obj) {
    return obj->is<OffThreadPlaceholderObject>();
  }

  JSObject* getPrototypeForOffThreadPlaceholder(JSObject* placeholder);

 private:
  static bool resolveOffThreadConstructor(JSContext* cx,
                                          Handle<GlobalObject*> global,
                                          JSProtoKey key);
  static JSObject* createOffThreadObject(JSContext* cx,
                                         Handle<GlobalObject*> global,
                                         unsigned slot);
};

/*
 * Unless otherwise specified, define ctor.prototype = proto as non-enumerable,
 * non-configurable, and non-writable; and define proto.constructor = ctor as
 * non-enumerable but configurable and writable.
 */
extern bool LinkConstructorAndPrototype(
    JSContext* cx, JSObject* ctor, JSObject* proto,
    unsigned prototypeAttrs = JSPROP_PERMANENT | JSPROP_READONLY,
    unsigned constructorAttrs = 0);

/*
 * Define properties and/or functions on any object. Either ps or fs, or both,
 * may be null.
 */
extern bool DefinePropertiesAndFunctions(JSContext* cx, HandleObject obj,
                                         const JSPropertySpec* ps,
                                         const JSFunctionSpec* fs);

extern bool DefineToStringTag(JSContext* cx, HandleObject obj, JSAtom* tag);

/*
 * Convenience templates to generic constructor and prototype creation functions
 * for ClassSpecs.
 */

template <JSNative ctor, unsigned length, gc::AllocKind kind,
          const JSJitInfo* jitInfo = nullptr>
JSObject* GenericCreateConstructor(JSContext* cx, JSProtoKey key) {
  // Note - We duplicate the trick from ClassName() so that we don't need to
  // include vm/JSAtom-inl.h here.
  PropertyName* name = (&cx->names().Null)[key];
  return GlobalObject::createConstructor(cx, ctor, name, length, kind, jitInfo);
}

template <typename T>
JSObject* GenericCreatePrototype(JSContext* cx, JSProtoKey key) {
  static_assert(
      !std::is_same_v<T, PlainObject>,
      "creating Object.prototype is very special and isn't handled here");
  MOZ_ASSERT(&T::class_ == ProtoKeyToClass(key),
             "type mismatch--probably too much copy/paste in your ClassSpec");
  MOZ_ASSERT(
      InheritanceProtoKeyForStandardClass(key) == JSProto_Object,
      "subclasses (of anything but Object) can't use GenericCreatePrototype");
  return GlobalObject::createBlankPrototype(cx, cx->global(), &T::protoClass_);
}

inline JSProtoKey StandardProtoKeyOrNull(const JSObject* obj) {
  return JSCLASS_CACHED_PROTO_KEY(obj->getClass());
}

JSObject* NewTenuredObjectWithFunctionPrototype(JSContext* cx,
                                                Handle<GlobalObject*> global);

}  // namespace js

template <>
inline bool JSObject::is<js::GlobalObject>() const {
  return !!(getClass()->flags & JSCLASS_IS_GLOBAL);
}

#endif /* vm_GlobalObject_h */
