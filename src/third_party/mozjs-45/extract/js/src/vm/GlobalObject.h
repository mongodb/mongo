/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GlobalObject_h
#define vm_GlobalObject_h

#include "jsarray.h"
#include "jsbool.h"
#include "jsexn.h"
#include "jsfun.h"
#include "jsnum.h"

#include "builtin/RegExp.h"
#include "js/Vector.h"
#include "vm/ArrayBufferObject.h"
#include "vm/ErrorObject.h"
#include "vm/RegExpStatics.h"
#include "vm/Runtime.h"

namespace js {

extern JSObject*
InitSharedArrayBufferClass(JSContext* cx, HandleObject obj);

class Debugger;
class TypedObjectModuleObject;
class StaticBlockObject;
class ClonedBlockObject;

class SimdTypeDescr;

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
 * [APPLICATION_SLOTS + 2 * JSProto_LIMIT, APPLICATION_SLOTS + 3 * JSProto_LIMIT)
 *   Stores the current value of the global property named for the JSProtoKey
 *   for the corresponding JSProtoKey offset from 2 * JSProto_LIMIT.
 * [APPLICATION_SLOTS + 3 * JSProto_LIMIT, RESERVED_SLOTS)
 *   Various one-off values: ES5 13.2.3's [[ThrowTypeError]], RegExp statics,
 *   the original eval for this global object (implementing |var eval =
 *   otherWindow.eval; eval(...)| as an indirect eval), a bit indicating
 *   whether this object has been cleared (see JS_ClearScope), and a cache for
 *   whether eval is allowed (per the global's Content Security Policy).
 *
 * The first two JSProto_LIMIT-sized ranges are necessary to implement
 * js::FindClassObject, and spec language speaking in terms of "the original
 * Array prototype object", or "as if by the expression new Array()" referring
 * to the original Array constructor. The third range stores the (writable and
 * even deletable) Object, Array, &c. properties (although a slot won't be used
 * again if its property is deleted and readded).
 */
class GlobalObject : public NativeObject
{
    /* Count of slots set aside for application use. */
    static const unsigned APPLICATION_SLOTS = JSCLASS_GLOBAL_APPLICATION_SLOTS;

    /*
     * Count of slots to store built-in constructors, prototypes, and initial
     * visible properties for the constructors.
     */
    static const unsigned STANDARD_CLASS_SLOTS  = JSProto_LIMIT * 3;

    enum : unsigned {
        /* Various function values needed by the engine. */
        EVAL = APPLICATION_SLOTS + STANDARD_CLASS_SLOTS,
        CREATE_DATAVIEW_FOR_THIS,
        THROWTYPEERROR,

        /*
         * Instances of the internal createArrayFromBuffer function used by the
         * typed array code, one per typed array element type.
         */
        FROM_BUFFER_UINT8,
        FROM_BUFFER_INT8,
        FROM_BUFFER_UINT16,
        FROM_BUFFER_INT16,
        FROM_BUFFER_UINT32,
        FROM_BUFFER_INT32,
        FROM_BUFFER_FLOAT32,
        FROM_BUFFER_FLOAT64,
        FROM_BUFFER_UINT8CLAMPED,

        /* One-off properties stored after slots for built-ins. */
        LEXICAL_SCOPE,
        ITERATOR_PROTO,
        ARRAY_ITERATOR_PROTO,
        STRING_ITERATOR_PROTO,
        LEGACY_GENERATOR_OBJECT_PROTO,
        STAR_GENERATOR_OBJECT_PROTO,
        STAR_GENERATOR_FUNCTION_PROTO,
        STAR_GENERATOR_FUNCTION,
        MAP_ITERATOR_PROTO,
        SET_ITERATOR_PROTO,
        COLLATOR_PROTO,
        NUMBER_FORMAT_PROTO,
        DATE_TIME_FORMAT_PROTO,
        MODULE_PROTO,
        IMPORT_ENTRY_PROTO,
        EXPORT_ENTRY_PROTO,
        REGEXP_STATICS,
        WARNED_ONCE_FLAGS,
        RUNTIME_CODEGEN_ENABLED,
        DEBUGGERS,
        INTRINSICS,
        FOR_OF_PIC_CHAIN,
        MODULE_RESOLVE_HOOK,
        WINDOW_PROXY,

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

    enum WarnOnceFlag : int32_t {
        WARN_WATCH_DEPRECATED                   = 0x00000001,
        WARN_PROTO_SETTING_SLOW                 = 0x00000002,
        WARN_STRING_CONTAINS_DEPRECATED         = 0x00000004
    };

    // Emit the specified warning if the given slot in |obj|'s global isn't
    // true, then set the slot to true.  Thus calling this method warns once
    // for each global object it's called on, and every other call does
    // nothing.
    static bool
    warnOnceAbout(JSContext* cx, HandleObject obj, WarnOnceFlag flag, unsigned errorNumber);


  public:
    ClonedBlockObject& lexicalScope() const;

    void setThrowTypeError(JSFunction* fun) {
        MOZ_ASSERT(getSlotRef(THROWTYPEERROR).isUndefined());
        setSlot(THROWTYPEERROR, ObjectValue(*fun));
    }

    void setOriginalEval(JSObject* evalobj) {
        MOZ_ASSERT(getSlotRef(EVAL).isUndefined());
        setSlot(EVAL, ObjectValue(*evalobj));
    }

    Value getConstructor(JSProtoKey key) const {
        MOZ_ASSERT(key <= JSProto_LIMIT);
        return getSlot(APPLICATION_SLOTS + key);
    }
    static bool ensureConstructor(JSContext* cx, Handle<GlobalObject*> global, JSProtoKey key);
    static bool resolveConstructor(JSContext* cx, Handle<GlobalObject*> global, JSProtoKey key);
    static bool initBuiltinConstructor(JSContext* cx, Handle<GlobalObject*> global,
                                       JSProtoKey key, HandleObject ctor, HandleObject proto);

    void setConstructor(JSProtoKey key, const Value& v) {
        MOZ_ASSERT(key <= JSProto_LIMIT);
        setSlot(APPLICATION_SLOTS + key, v);
    }

    Value getPrototype(JSProtoKey key) const {
        MOZ_ASSERT(key <= JSProto_LIMIT);
        return getSlot(APPLICATION_SLOTS + JSProto_LIMIT + key);
    }

    void setPrototype(JSProtoKey key, const Value& value) {
        MOZ_ASSERT(key <= JSProto_LIMIT);
        setSlot(APPLICATION_SLOTS + JSProto_LIMIT + key, value);
    }

    static uint32_t constructorPropertySlot(JSProtoKey key) {
        MOZ_ASSERT(key <= JSProto_LIMIT);
        return APPLICATION_SLOTS + JSProto_LIMIT * 2 + key;
    }

    Value getConstructorPropertySlot(JSProtoKey key) {
        return getSlot(constructorPropertySlot(key));
    }

    void setConstructorPropertySlot(JSProtoKey key, const Value& ctor) {
        setSlot(constructorPropertySlot(key), ctor);
    }

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
        MOZ_ASSERT(getConstructor(key).isUndefined() ||
                   getConstructor(key).isObject());
        return !getConstructor(key).isUndefined();
    }

    /*
     * Using a Handle<GlobalObject*> as a Handle<Object*> is always safe as
     * GlobalObject derives JSObject. However, with C++'s semantics, Handle<T>
     * is not related to Handle<S>, independent of how S and T are related.
     * Further, Handle stores an indirect pointer and, again because of C++'s
     * semantics, T** is not related to S**, independent of how S and T are
     * related. Since we know that this specific case is safe, we provide a
     * manual upcast operation here to do the reinterpret_cast in a known-safe
     * manner.
     */
    static HandleObject upcast(Handle<GlobalObject*> global) {
        return HandleObject::fromMarkedLocation(
                reinterpret_cast<JSObject * const*>(global.address()));
    }

  private:
    bool arrayClassInitialized() const {
        return classIsInitialized(JSProto_Array);
    }

    bool booleanClassInitialized() const {
        return classIsInitialized(JSProto_Boolean);
    }
    bool numberClassInitialized() const {
        return classIsInitialized(JSProto_Number);
    }
    bool stringClassInitialized() const {
        return classIsInitialized(JSProto_String);
    }
    bool regexpClassInitialized() const {
        return classIsInitialized(JSProto_RegExp);
    }
    bool arrayBufferClassInitialized() const {
        return classIsInitialized(JSProto_ArrayBuffer);
    }
    bool sharedArrayBufferClassInitialized() const {
        return classIsInitialized(JSProto_SharedArrayBuffer);
    }
    bool errorClassesInitialized() const {
        return classIsInitialized(JSProto_Error);
    }
    bool dataViewClassInitialized() const {
        return classIsInitialized(JSProto_DataView);
    }

    Value createArrayFromBufferHelper(uint32_t slot) const {
        MOZ_ASSERT(FROM_BUFFER_UINT8 <= slot && slot <= FROM_BUFFER_UINT8CLAMPED);
        MOZ_ASSERT(!getSlot(slot).isUndefined());
        return getSlot(slot);
    }

    void setCreateArrayFromBufferHelper(uint32_t slot, Handle<JSFunction*> fun) {
        MOZ_ASSERT(getSlotRef(slot).isUndefined());
        setSlot(slot, ObjectValue(*fun));
    }

  public:
    /* XXX Privatize me! */
    void setCreateDataViewForThis(Handle<JSFunction*> fun) {
        MOZ_ASSERT(getSlotRef(CREATE_DATAVIEW_FOR_THIS).isUndefined());
        setSlot(CREATE_DATAVIEW_FOR_THIS, ObjectValue(*fun));
    }

    template<typename T>
    inline void setCreateArrayFromBuffer(Handle<JSFunction*> fun);

  private:
    // Disallow use of unqualified JSObject::create in GlobalObject.
    static GlobalObject* create(...) = delete;

    friend struct ::JSRuntime;
    static GlobalObject* createInternal(JSContext* cx, const Class* clasp);

  public:
    static GlobalObject*
    new_(JSContext* cx, const Class* clasp, JSPrincipals* principals,
         JS::OnNewGlobalHookOption hookOption, const JS::CompartmentOptions& options);

    /*
     * Create a constructor function with the specified name and length using
     * ctor, a method which creates objects with the given class.
     */
    JSFunction*
    createConstructor(JSContext* cx, JSNative ctor, JSAtom* name, unsigned length,
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
    NativeObject* createBlankPrototype(JSContext* cx, const js::Class* clasp);

    /*
     * Identical to createBlankPrototype, but uses proto as the [[Prototype]]
     * of the returned blank prototype.
     */
    NativeObject* createBlankPrototypeInheriting(JSContext* cx, const js::Class* clasp,
                                                 HandleObject proto);

    template <typename T>
    T* createBlankPrototype(JSContext* cx) {
        NativeObject* res = createBlankPrototype(cx, &T::class_);
        return res ? &res->template as<T>() : nullptr;
    }

    NativeObject* getOrCreateObjectPrototype(JSContext* cx) {
        if (functionObjectClassesInitialized())
            return &getPrototype(JSProto_Object).toObject().as<NativeObject>();
        RootedGlobalObject self(cx, this);
        if (!ensureConstructor(cx, self, JSProto_Object))
            return nullptr;
        return &self->getPrototype(JSProto_Object).toObject().as<NativeObject>();
    }

    NativeObject* getOrCreateFunctionPrototype(JSContext* cx) {
        if (functionObjectClassesInitialized())
            return &getPrototype(JSProto_Function).toObject().as<NativeObject>();
        RootedGlobalObject self(cx, this);
        if (!ensureConstructor(cx, self, JSProto_Object))
            return nullptr;
        return &self->getPrototype(JSProto_Function).toObject().as<NativeObject>();
    }

    static NativeObject* getOrCreateArrayPrototype(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_Array))
            return nullptr;
        return &global->getPrototype(JSProto_Array).toObject().as<NativeObject>();
    }

    NativeObject* maybeGetArrayPrototype() {
        if (arrayClassInitialized())
            return &getPrototype(JSProto_Array).toObject().as<NativeObject>();
        return nullptr;
    }

    static NativeObject* getOrCreateBooleanPrototype(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_Boolean))
            return nullptr;
        return &global->getPrototype(JSProto_Boolean).toObject().as<NativeObject>();
    }

    static NativeObject* getOrCreateNumberPrototype(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_Number))
            return nullptr;
        return &global->getPrototype(JSProto_Number).toObject().as<NativeObject>();
    }

    static NativeObject* getOrCreateStringPrototype(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_String))
            return nullptr;
        return &global->getPrototype(JSProto_String).toObject().as<NativeObject>();
    }

    static NativeObject* getOrCreateSymbolPrototype(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_Symbol))
            return nullptr;
        return &global->getPrototype(JSProto_Symbol).toObject().as<NativeObject>();
    }

    static NativeObject* getOrCreateRegExpPrototype(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_RegExp))
            return nullptr;
        return &global->getPrototype(JSProto_RegExp).toObject().as<NativeObject>();
    }

    JSObject* maybeGetRegExpPrototype() {
        if (regexpClassInitialized())
            return &getPrototype(JSProto_RegExp).toObject();
        return nullptr;
    }

    static NativeObject* getOrCreateSavedFramePrototype(JSContext* cx,
                                                        Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_SavedFrame))
            return nullptr;
        return &global->getPrototype(JSProto_SavedFrame).toObject().as<NativeObject>();
    }

    static JSObject* getOrCreateArrayBufferPrototype(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_ArrayBuffer))
            return nullptr;
        return &global->getPrototype(JSProto_ArrayBuffer).toObject();
    }

    JSObject* getOrCreateSharedArrayBufferPrototype(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_SharedArrayBuffer))
            return nullptr;
        return &global->getPrototype(JSProto_SharedArrayBuffer).toObject();
    }

    static JSObject* getOrCreateCustomErrorPrototype(JSContext* cx,
                                                     Handle<GlobalObject*> global,
                                                     JSExnType exnType)
    {
        JSProtoKey key = GetExceptionProtoKey(exnType);
        if (!ensureConstructor(cx, global, key))
            return nullptr;
        return &global->getPrototype(key).toObject();
    }

    JSObject* getOrCreateIntlObject(JSContext* cx) {
        return getOrCreateObject(cx, APPLICATION_SLOTS + JSProto_Intl, initIntlObject);
    }

    JSObject* getOrCreateTypedObjectModule(JSContext* cx) {
        return getOrCreateObject(cx, APPLICATION_SLOTS + JSProto_TypedObject, initTypedObjectModule);
    }

    JSObject* getOrCreateSimdGlobalObject(JSContext* cx) {
        return getOrCreateObject(cx, APPLICATION_SLOTS + JSProto_SIMD, initSimdObject);
    }

    template<class /* SimdTypeDescriptor (cf SIMD.h) */ T>
    SimdTypeDescr* getOrCreateSimdTypeDescr(JSContext* cx) {
        RootedObject globalSimdObject(cx, cx->global()->getOrCreateSimdGlobalObject(cx));
        if (!globalSimdObject)
            return nullptr;
        const Value& slot = globalSimdObject->as<NativeObject>().getReservedSlot(uint32_t(T::type));
        MOZ_ASSERT(slot.isObject());
        return &slot.toObject().as<SimdTypeDescr>();
    }

    TypedObjectModuleObject& getTypedObjectModule() const;

    JSObject* getLegacyIteratorPrototype() {
        return &getPrototype(JSProto_Iterator).toObject();
    }

    JSObject* getOrCreateCollatorPrototype(JSContext* cx) {
        return getOrCreateObject(cx, COLLATOR_PROTO, initCollatorProto);
    }

    JSObject* getOrCreateNumberFormatPrototype(JSContext* cx) {
        return getOrCreateObject(cx, NUMBER_FORMAT_PROTO, initNumberFormatProto);
    }

    JSObject* getOrCreateDateTimeFormatPrototype(JSContext* cx) {
        return getOrCreateObject(cx, DATE_TIME_FORMAT_PROTO, initDateTimeFormatProto);
    }

    JSObject* getModulePrototype() {
        return &getSlot(MODULE_PROTO).toObject();
    }

    JSObject* getImportEntryPrototype() {
        return &getSlot(IMPORT_ENTRY_PROTO).toObject();
    }

    JSObject* getExportEntryPrototype() {
        return &getSlot(EXPORT_ENTRY_PROTO).toObject();
    }

    static JSFunction*
    getOrCreateTypedArrayConstructor(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_TypedArray))
            return nullptr;
        return &global->getConstructor(JSProto_TypedArray).toObject().as<JSFunction>();
    }

    static JSObject*
    getOrCreateTypedArrayPrototype(JSContext* cx, Handle<GlobalObject*> global) {
        if (!ensureConstructor(cx, global, JSProto_TypedArray))
            return nullptr;
        return &global->getPrototype(JSProto_TypedArray).toObject();
    }

  private:
    typedef bool (*ObjectInitOp)(JSContext* cx, Handle<GlobalObject*> global);

    JSObject* getOrCreateObject(JSContext* cx, unsigned slot, ObjectInitOp init) {
        Value v = getSlotRef(slot);
        if (v.isObject())
            return &v.toObject();
        RootedGlobalObject self(cx, this);
        if (!init(cx, self))
            return nullptr;
        return &self->getSlot(slot).toObject();
    }

  public:
    static NativeObject* getOrCreateIteratorPrototype(JSContext* cx, Handle<GlobalObject*> global)
    {
        return MaybeNativeObject(global->getOrCreateObject(cx, ITERATOR_PROTO, initIteratorProto));
    }

    static NativeObject* getOrCreateArrayIteratorPrototype(JSContext* cx, Handle<GlobalObject*> global)
    {
        return MaybeNativeObject(global->getOrCreateObject(cx, ARRAY_ITERATOR_PROTO, initArrayIteratorProto));
    }

    static NativeObject* getOrCreateStringIteratorPrototype(JSContext* cx,
                                                            Handle<GlobalObject*> global)
    {
        return MaybeNativeObject(global->getOrCreateObject(cx, STRING_ITERATOR_PROTO, initStringIteratorProto));
    }

    static NativeObject* getOrCreateLegacyGeneratorObjectPrototype(JSContext* cx,
                                                                   Handle<GlobalObject*> global)
    {
        return MaybeNativeObject(global->getOrCreateObject(cx, LEGACY_GENERATOR_OBJECT_PROTO,
                                                           initLegacyGeneratorProto));
    }

    static NativeObject* getOrCreateStarGeneratorObjectPrototype(JSContext* cx,
                                                                 Handle<GlobalObject*> global)
    {
        return MaybeNativeObject(global->getOrCreateObject(cx, STAR_GENERATOR_OBJECT_PROTO, initStarGenerators));
    }

    static NativeObject* getOrCreateStarGeneratorFunctionPrototype(JSContext* cx,
                                                                   Handle<GlobalObject*> global)
    {
        return MaybeNativeObject(global->getOrCreateObject(cx, STAR_GENERATOR_FUNCTION_PROTO, initStarGenerators));
    }

    static JSObject* getOrCreateStarGeneratorFunction(JSContext* cx,
                                                      Handle<GlobalObject*> global)
    {
        return global->getOrCreateObject(cx, STAR_GENERATOR_FUNCTION, initStarGenerators);
    }

    static JSObject* getOrCreateMapIteratorPrototype(JSContext* cx,
                                                     Handle<GlobalObject*> global)
    {
        return global->getOrCreateObject(cx, MAP_ITERATOR_PROTO, initMapIteratorProto);
    }

    static JSObject* getOrCreateSetIteratorPrototype(JSContext* cx,
                                                     Handle<GlobalObject*> global)
    {
        return global->getOrCreateObject(cx, SET_ITERATOR_PROTO, initSetIteratorProto);
    }

    JSObject* getOrCreateDataViewPrototype(JSContext* cx) {
        RootedGlobalObject self(cx, this);
        if (!ensureConstructor(cx, self, JSProto_DataView))
            return nullptr;
        return &self->getPrototype(JSProto_DataView).toObject();
    }

    static NativeObject* getIntrinsicsHolder(JSContext* cx, Handle<GlobalObject*> global);

    Value existingIntrinsicValue(PropertyName* name) {
        Value slot = getReservedSlot(INTRINSICS);
        MOZ_ASSERT(slot.isObject(), "intrinsics holder must already exist");

        NativeObject* holder = &slot.toObject().as<NativeObject>();

        Shape* shape = holder->lookupPure(name);
        MOZ_ASSERT(shape, "intrinsic must already have been added to holder");

        return holder->getSlot(shape->slot());
    }

    static bool
    maybeGetIntrinsicValue(JSContext* cx, Handle<GlobalObject*> global, Handle<PropertyName*> name,
                           MutableHandleValue vp)
    {
        NativeObject* holder = getIntrinsicsHolder(cx, global);
        if (!holder)
            return false;

        if (Shape* shape = holder->lookupPure(name)) {
            vp.set(holder->getSlot(shape->slot()));
            return true;
        }
        return false;
    }

    static bool getIntrinsicValue(JSContext* cx, Handle<GlobalObject*> global,
                                  HandlePropertyName name, MutableHandleValue value)
    {
        if (GlobalObject::maybeGetIntrinsicValue(cx, global, name, value))
            return true;
        if (!cx->runtime()->cloneSelfHostedValue(cx, name, value))
            return false;
        return GlobalObject::addIntrinsicValue(cx, global, name, value);
    }

    static bool addIntrinsicValue(JSContext* cx, Handle<GlobalObject*> global,
                                  HandlePropertyName name, HandleValue value);

    static bool setIntrinsicValue(JSContext* cx, Handle<GlobalObject*> global,
                                  HandlePropertyName name, HandleValue value)
    {
        MOZ_ASSERT(cx->runtime()->isSelfHostingGlobal(global));
        RootedObject holder(cx, GlobalObject::getIntrinsicsHolder(cx, global));
        if (!holder)
            return false;
        return SetProperty(cx, holder, name, value);
    }

    static bool getSelfHostedFunction(JSContext* cx, Handle<GlobalObject*> global,
                                      HandlePropertyName selfHostedName, HandleAtom name,
                                      unsigned nargs, MutableHandleValue funVal);

    bool hasRegExpStatics() const;
    RegExpStatics* getRegExpStatics(ExclusiveContext* cx) const;
    RegExpStatics* getAlreadyCreatedRegExpStatics() const;

    JSObject* getThrowTypeError() const {
        const Value v = getReservedSlot(THROWTYPEERROR);
        MOZ_ASSERT(v.isObject(),
                   "attempting to access [[ThrowTypeError]] too early");
        return &v.toObject();
    }

    Value createDataViewForThis() const {
        MOZ_ASSERT(dataViewClassInitialized());
        return getSlot(CREATE_DATAVIEW_FOR_THIS);
    }

    template<typename T>
    inline Value createArrayFromBuffer() const;

    static bool isRuntimeCodeGenEnabled(JSContext* cx, Handle<GlobalObject*> global);

    // Warn about use of the deprecated watch/unwatch functions in the global
    // in which |obj| was created, if no prior warning was given.
    static bool warnOnceAboutWatch(JSContext* cx, HandleObject obj) {
        // Temporarily disabled until we've provided a watch/unwatch workaround for
        // debuggers like Firebug (bug 934669).
        //return warnOnceAbout(cx, obj, WARN_WATCH_DEPRECATED, JSMSG_OBJECT_WATCH_DEPRECATED);
        return true;
    }

    // Warn about use of the given __proto__ setter to attempt to mutate an
    // object's [[Prototype]], if no prior warning was given.
    static bool warnOnceAboutPrototypeMutation(JSContext* cx, HandleObject protoSetter) {
        return warnOnceAbout(cx, protoSetter, WARN_PROTO_SETTING_SLOW, JSMSG_PROTO_SETTING_SLOW);
    }

    // Warn about use of the deprecated String.prototype.contains method
    static bool warnOnceAboutStringContains(JSContext *cx, HandleObject strContains) {
        return warnOnceAbout(cx, strContains, WARN_STRING_CONTAINS_DEPRECATED,
                             JSMSG_DEPRECATED_STRING_CONTAINS);
    }

    static bool getOrCreateEval(JSContext* cx, Handle<GlobalObject*> global,
                                MutableHandleObject eval);

    // Infallibly test whether the given value is the eval function for this global.
    bool valueIsEval(Value val);

    // Implemented in jsiter.cpp.
    static bool initIteratorProto(JSContext* cx, Handle<GlobalObject*> global);
    static bool initArrayIteratorProto(JSContext* cx, Handle<GlobalObject*> global);
    static bool initStringIteratorProto(JSContext* cx, Handle<GlobalObject*> global);

    // Implemented in vm/GeneratorObject.cpp.
    static bool initLegacyGeneratorProto(JSContext* cx, Handle<GlobalObject*> global);
    static bool initStarGenerators(JSContext* cx, Handle<GlobalObject*> global);

    // Implemented in builtin/MapObject.cpp.
    static bool initMapIteratorProto(JSContext* cx, Handle<GlobalObject*> global);
    static bool initSetIteratorProto(JSContext* cx, Handle<GlobalObject*> global);

    // Implemented in Intl.cpp.
    static bool initIntlObject(JSContext* cx, Handle<GlobalObject*> global);
    static bool initCollatorProto(JSContext* cx, Handle<GlobalObject*> global);
    static bool initNumberFormatProto(JSContext* cx, Handle<GlobalObject*> global);
    static bool initDateTimeFormatProto(JSContext* cx, Handle<GlobalObject*> global);

    // Implemented in builtin/ModuleObject.cpp
    static bool initModuleProto(JSContext* cx, Handle<GlobalObject*> global);
    static bool initImportEntryProto(JSContext* cx, Handle<GlobalObject*> global);
    static bool initExportEntryProto(JSContext* cx, Handle<GlobalObject*> global);

    // Implemented in builtin/TypedObject.cpp
    static bool initTypedObjectModule(JSContext* cx, Handle<GlobalObject*> global);

    // Implemented in builtim/SIMD.cpp
    static bool initSimdObject(JSContext* cx, Handle<GlobalObject*> global);

    static bool initStandardClasses(JSContext* cx, Handle<GlobalObject*> global);
    static bool initSelfHostingBuiltins(JSContext* cx, Handle<GlobalObject*> global,
                                        const JSFunctionSpec* builtins);

    typedef js::Vector<js::Debugger*, 0, js::SystemAllocPolicy> DebuggerVector;

    /*
     * The collection of Debugger objects debugging this global. If this global
     * is not a debuggee, this returns either nullptr or an empty vector.
     */
    DebuggerVector* getDebuggers() const;

    /*
     * The same, but create the empty vector if one does not already
     * exist. Returns nullptr only on OOM.
     */
    static DebuggerVector* getOrCreateDebuggers(JSContext* cx, Handle<GlobalObject*> global);

    inline NativeObject* getForOfPICObject() {
        Value forOfPIC = getReservedSlot(FOR_OF_PIC_CHAIN);
        if (forOfPIC.isUndefined())
            return nullptr;
        return &forOfPIC.toObject().as<NativeObject>();
    }
    static NativeObject* getOrCreateForOfPICObject(JSContext* cx, Handle<GlobalObject*> global);

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

    void setModuleResolveHook(HandleFunction hook) {
        MOZ_ASSERT(hook);
        setSlot(MODULE_RESOLVE_HOOK, ObjectValue(*hook));
    }

    JSFunction* moduleResolveHook() {
        Value value = getSlotRef(MODULE_RESOLVE_HOOK);
        if (value.isUndefined())
            return nullptr;

        return &value.toObject().as<JSFunction>();
    }

    // Returns either this global's star-generator function prototype, or null
    // if that object was never created.  Dodgy; for use only in also-dodgy
    // GlobalHelperThreadState::mergeParseTaskCompartment().
    JSObject* getStarGeneratorFunctionPrototype();
};

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<uint8_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_UINT8, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<int8_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_INT8, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<uint16_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_UINT16, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<int16_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_INT16, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<uint32_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_UINT32, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<int32_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_INT32, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<float>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_FLOAT32, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<double>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_FLOAT64, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<uint8_clamped>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_UINT8CLAMPED, fun);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<uint8_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_UINT8);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<int8_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_INT8);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<uint16_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_UINT16);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<int16_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_INT16);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<uint32_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_UINT32);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<int32_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_INT32);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<float>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_FLOAT32);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<double>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_FLOAT64);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<uint8_clamped>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_UINT8CLAMPED);
}

/*
 * Define ctor.prototype = proto as non-enumerable, non-configurable, and
 * non-writable; define proto.constructor = ctor as non-enumerable but
 * configurable and writable.
 */
extern bool
LinkConstructorAndPrototype(JSContext* cx, JSObject* ctor, JSObject* proto);

/*
 * Define properties and/or functions on any object. Either ps or fs, or both,
 * may be null.
 */
extern bool
DefinePropertiesAndFunctions(JSContext* cx, HandleObject obj,
                             const JSPropertySpec* ps, const JSFunctionSpec* fs);

typedef HashSet<GlobalObject*, DefaultHasher<GlobalObject*>, SystemAllocPolicy> GlobalObjectSet;

/*
 * Convenience templates to generic constructor and prototype creation functions
 * for ClassSpecs.
 */

template<JSNative ctor, unsigned length, gc::AllocKind kind, const JSJitInfo* jitInfo = nullptr>
JSObject*
GenericCreateConstructor(JSContext* cx, JSProtoKey key)
{
    // Note - We duplicate the trick from ClassName() so that we don't need to
    // include jsatominlines.h here.
    PropertyName* name = (&cx->names().Null)[key];
    return cx->global()->createConstructor(cx, ctor, name, length, kind, jitInfo);
}

inline JSObject*
GenericCreatePrototype(JSContext* cx, JSProtoKey key)
{
    MOZ_ASSERT(key != JSProto_Object);
    const Class* clasp = ProtoKeyToClass(key);
    MOZ_ASSERT(clasp);
    JSProtoKey parentKey = ParentKeyForStandardClass(key);
    if (!GlobalObject::ensureConstructor(cx, cx->global(), parentKey))
        return nullptr;
    RootedObject parentProto(cx, &cx->global()->getPrototype(parentKey).toObject());
    return cx->global()->createBlankPrototypeInheriting(cx, clasp, parentProto);
}

inline JSProtoKey
StandardProtoKeyOrNull(const JSObject* obj)
{
    JSProtoKey key = JSCLASS_CACHED_PROTO_KEY(obj->getClass());
    if (key == JSProto_Error)
        return GetExceptionProtoKey(obj->as<ErrorObject>().type());
    return key;
}

} // namespace js

template<>
inline bool
JSObject::is<js::GlobalObject>() const
{
    return !!(getClass()->flags & JSCLASS_IS_GLOBAL);
}

#endif /* vm_GlobalObject_h */
