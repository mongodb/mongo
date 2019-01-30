/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript API. */

#ifndef jsapi_h
#define jsapi_h

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Range.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Variant.h"

#include <iterator>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "jspubtd.h"

#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/CharacterEncoding.h"
#include "js/Class.h"
#include "js/GCVector.h"
#include "js/HashTable.h"
#include "js/Id.h"
#include "js/Principals.h"
#include "js/Realm.h"
#include "js/RefCounted.h"
#include "js/RootingAPI.h"
#include "js/Stream.h"
#include "js/TracingAPI.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "js/Vector.h"

/************************************************************************/

namespace JS {

class TwoByteChars;

#ifdef JS_DEBUG

class JS_PUBLIC_API(AutoCheckRequestDepth)
{
    JSContext* cx;
  public:
    explicit AutoCheckRequestDepth(JSContext* cx);
    ~AutoCheckRequestDepth();
};

# define CHECK_REQUEST(cx) \
    JS::AutoCheckRequestDepth _autoCheckRequestDepth(cx)

#else

# define CHECK_REQUEST(cx) \
    ((void) 0)

#endif /* JS_DEBUG */

/** AutoValueArray roots an internal fixed-size array of Values. */
template <size_t N>
class MOZ_RAII AutoValueArray : public AutoGCRooter
{
    const size_t length_;
    Value elements_[N];

  public:
    explicit AutoValueArray(JSContext* cx
                            MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, VALARRAY), length_(N)
    {
        /* Always initialize in case we GC before assignment. */
        mozilla::PodArrayZero(elements_);
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    unsigned length() const { return length_; }
    const Value* begin() const { return elements_; }
    Value* begin() { return elements_; }

    HandleValue operator[](unsigned i) const {
        MOZ_ASSERT(i < N);
        return HandleValue::fromMarkedLocation(&elements_[i]);
    }
    MutableHandleValue operator[](unsigned i) {
        MOZ_ASSERT(i < N);
        return MutableHandleValue::fromMarkedLocation(&elements_[i]);
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

using ValueVector = JS::GCVector<JS::Value>;
using IdVector = JS::GCVector<jsid>;
using ScriptVector = JS::GCVector<JSScript*>;
using StringVector = JS::GCVector<JSString*>;

template<class Key, class Value>
class MOZ_RAII AutoHashMapRooter : protected AutoGCRooter
{
  private:
    typedef js::HashMap<Key, Value> HashMapImpl;

  public:
    explicit AutoHashMapRooter(JSContext* cx, ptrdiff_t tag
                               MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, tag), map(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    typedef Key KeyType;
    typedef Value ValueType;
    typedef typename HashMapImpl::Entry Entry;
    typedef typename HashMapImpl::Lookup Lookup;
    typedef typename HashMapImpl::Ptr Ptr;
    typedef typename HashMapImpl::AddPtr AddPtr;

    bool init(uint32_t len = 16) {
        return map.init(len);
    }
    bool initialized() const {
        return map.initialized();
    }
    Ptr lookup(const Lookup& l) const {
        return map.lookup(l);
    }
    void remove(Ptr p) {
        map.remove(p);
    }
    AddPtr lookupForAdd(const Lookup& l) const {
        return map.lookupForAdd(l);
    }

    template<typename KeyInput, typename ValueInput>
    bool add(AddPtr& p, const KeyInput& k, const ValueInput& v) {
        return map.add(p, k, v);
    }

    bool add(AddPtr& p, const Key& k) {
        return map.add(p, k);
    }

    template<typename KeyInput, typename ValueInput>
    bool relookupOrAdd(AddPtr& p, const KeyInput& k, const ValueInput& v) {
        return map.relookupOrAdd(p, k, v);
    }

    typedef typename HashMapImpl::Range Range;
    Range all() const {
        return map.all();
    }

    typedef typename HashMapImpl::Enum Enum;

    void clear() {
        map.clear();
    }

    void finish() {
        map.finish();
    }

    bool empty() const {
        return map.empty();
    }

    uint32_t count() const {
        return map.count();
    }

    size_t capacity() const {
        return map.capacity();
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return map.sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return map.sizeOfIncludingThis(mallocSizeOf);
    }

    /************************************************** Shorthand operations */

    bool has(const Lookup& l) const {
        return map.has(l);
    }

    template<typename KeyInput, typename ValueInput>
    bool put(const KeyInput& k, const ValueInput& v) {
        return map.put(k, v);
    }

    template<typename KeyInput, typename ValueInput>
    bool putNew(const KeyInput& k, const ValueInput& v) {
        return map.putNew(k, v);
    }

    Ptr lookupWithDefault(const Key& k, const Value& defaultValue) {
        return map.lookupWithDefault(k, defaultValue);
    }

    void remove(const Lookup& l) {
        map.remove(l);
    }

    friend void AutoGCRooter::trace(JSTracer* trc);

  private:
    AutoHashMapRooter(const AutoHashMapRooter& hmr) = delete;
    AutoHashMapRooter& operator=(const AutoHashMapRooter& hmr) = delete;

    HashMapImpl map;

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

template<class T>
class MOZ_RAII AutoHashSetRooter : protected AutoGCRooter
{
  private:
    typedef js::HashSet<T> HashSetImpl;

  public:
    explicit AutoHashSetRooter(JSContext* cx, ptrdiff_t tag
                               MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, tag), set(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    typedef typename HashSetImpl::Lookup Lookup;
    typedef typename HashSetImpl::Ptr Ptr;
    typedef typename HashSetImpl::AddPtr AddPtr;

    bool init(uint32_t len = 16) {
        return set.init(len);
    }
    bool initialized() const {
        return set.initialized();
    }
    Ptr lookup(const Lookup& l) const {
        return set.lookup(l);
    }
    void remove(Ptr p) {
        set.remove(p);
    }
    AddPtr lookupForAdd(const Lookup& l) const {
        return set.lookupForAdd(l);
    }

    bool add(AddPtr& p, const T& t) {
        return set.add(p, t);
    }

    bool relookupOrAdd(AddPtr& p, const Lookup& l, const T& t) {
        return set.relookupOrAdd(p, l, t);
    }

    typedef typename HashSetImpl::Range Range;
    Range all() const {
        return set.all();
    }

    typedef typename HashSetImpl::Enum Enum;

    void clear() {
        set.clear();
    }

    void finish() {
        set.finish();
    }

    bool empty() const {
        return set.empty();
    }

    uint32_t count() const {
        return set.count();
    }

    size_t capacity() const {
        return set.capacity();
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return set.sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return set.sizeOfIncludingThis(mallocSizeOf);
    }

    /************************************************** Shorthand operations */

    bool has(const Lookup& l) const {
        return set.has(l);
    }

    bool put(const T& t) {
        return set.put(t);
    }

    bool putNew(const T& t) {
        return set.putNew(t);
    }

    void remove(const Lookup& l) {
        set.remove(l);
    }

    friend void AutoGCRooter::trace(JSTracer* trc);

  private:
    AutoHashSetRooter(const AutoHashSetRooter& hmr) = delete;
    AutoHashSetRooter& operator=(const AutoHashSetRooter& hmr) = delete;

    HashSetImpl set;

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/**
 * Custom rooting behavior for internal and external clients.
 */
class MOZ_RAII JS_PUBLIC_API(CustomAutoRooter) : private AutoGCRooter
{
  public:
    template <typename CX>
    explicit CustomAutoRooter(const CX& cx MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, CUSTOM)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    friend void AutoGCRooter::trace(JSTracer* trc);

  protected:
    virtual ~CustomAutoRooter() {}

    /** Supplied by derived class to trace roots. */
    virtual void trace(JSTracer* trc) = 0;

  private:
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/** A handle to an array of rooted values. */
class HandleValueArray
{
    const size_t length_;
    const Value * const elements_;

    HandleValueArray(size_t len, const Value* elements) : length_(len), elements_(elements) {}

  public:
    explicit HandleValueArray(HandleValue value) : length_(1), elements_(value.address()) {}

    MOZ_IMPLICIT HandleValueArray(const AutoValueVector& values)
      : length_(values.length()), elements_(values.begin()) {}

    template <size_t N>
    MOZ_IMPLICIT HandleValueArray(const AutoValueArray<N>& values) : length_(N), elements_(values.begin()) {}

    /** CallArgs must already be rooted somewhere up the stack. */
    MOZ_IMPLICIT HandleValueArray(const JS::CallArgs& args) : length_(args.length()), elements_(args.array()) {}

    /** Use with care! Only call this if the data is guaranteed to be marked. */
    static HandleValueArray fromMarkedLocation(size_t len, const Value* elements) {
        return HandleValueArray(len, elements);
    }

    static HandleValueArray subarray(const HandleValueArray& values, size_t startIndex, size_t len) {
        MOZ_ASSERT(startIndex + len <= values.length());
        return HandleValueArray(len, values.begin() + startIndex);
    }

    static HandleValueArray empty() {
        return HandleValueArray(0, nullptr);
    }

    size_t length() const { return length_; }
    const Value* begin() const { return elements_; }

    HandleValue operator[](size_t i) const {
        MOZ_ASSERT(i < length_);
        return HandleValue::fromMarkedLocation(&elements_[i]);
    }
};

}  /* namespace JS */

/************************************************************************/

struct JSFreeOp {
  protected:
    JSRuntime*  runtime_;

    explicit JSFreeOp(JSRuntime* rt)
      : runtime_(rt) { }

  public:
    JSRuntime* runtime() const {
        MOZ_ASSERT(runtime_);
        return runtime_;
    }
};

/* Callbacks and their arguments. */

/************************************************************************/

typedef bool
(* JSInterruptCallback)(JSContext* cx);

typedef JSObject*
(* JSGetIncumbentGlobalCallback)(JSContext* cx);

typedef bool
(* JSEnqueuePromiseJobCallback)(JSContext* cx, JS::HandleObject job,
                                JS::HandleObject allocationSite, JS::HandleObject incumbentGlobal,
                                void* data);

namespace JS {

enum class PromiseRejectionHandlingState {
    Unhandled,
    Handled
};

} /* namespace JS */

typedef void
(* JSPromiseRejectionTrackerCallback)(JSContext* cx, JS::HandleObject promise,
                                      JS::PromiseRejectionHandlingState state,
                                      void* data);

typedef void
(* JSProcessPromiseCallback)(JSContext* cx, JS::HandleObject promise);

/**
 * Possible exception types. These types are part of a JSErrorFormatString
 * structure. They define which error to throw in case of a runtime error.
 *
 * JSEXN_WARN is used for warnings in js.msg files (for instance because we
 * don't want to prepend 'Error:' to warning messages). This value can go away
 * if we ever decide to use an entirely separate mechanism for warnings.
 */
typedef enum JSExnType {
    JSEXN_ERR,
    JSEXN_FIRST = JSEXN_ERR,
        JSEXN_INTERNALERR,
        JSEXN_EVALERR,
        JSEXN_RANGEERR,
        JSEXN_REFERENCEERR,
        JSEXN_SYNTAXERR,
        JSEXN_TYPEERR,
        JSEXN_URIERR,
        JSEXN_DEBUGGEEWOULDRUN,
        JSEXN_WASMCOMPILEERROR,
        JSEXN_WASMLINKERROR,
        JSEXN_WASMRUNTIMEERROR,
    JSEXN_ERROR_LIMIT,
    JSEXN_WARN = JSEXN_ERROR_LIMIT,
    JSEXN_NOTE,
    JSEXN_LIMIT
} JSExnType;

struct JSErrorFormatString {
     /** The error message name in ASCII. */
    const char* name;

    /** The error format string in ASCII. */
    const char* format;

    /** The number of arguments to expand in the formatted error message. */
    uint16_t argCount;

    /** One of the JSExnType constants above. */
    int16_t exnType;
};

typedef const JSErrorFormatString*
(* JSErrorCallback)(void* userRef, const unsigned errorNumber);

typedef bool
(* JSLocaleToUpperCase)(JSContext* cx, JS::HandleString src, JS::MutableHandleValue rval);

typedef bool
(* JSLocaleToLowerCase)(JSContext* cx, JS::HandleString src, JS::MutableHandleValue rval);

typedef bool
(* JSLocaleCompare)(JSContext* cx, JS::HandleString src1, JS::HandleString src2,
                    JS::MutableHandleValue rval);

typedef bool
(* JSLocaleToUnicode)(JSContext* cx, const char* src, JS::MutableHandleValue rval);

/**
 * Callback used to ask the embedding for the cross compartment wrapper handler
 * that implements the desired prolicy for this kind of object in the
 * destination compartment. |obj| is the object to be wrapped. If |existing| is
 * non-nullptr, it will point to an existing wrapper object that should be
 * re-used if possible. |existing| is guaranteed to be a cross-compartment
 * wrapper with a lazily-defined prototype and the correct global. It is
 * guaranteed not to wrap a function.
 */
typedef JSObject*
(* JSWrapObjectCallback)(JSContext* cx, JS::HandleObject existing, JS::HandleObject obj);

/**
 * Callback used by the wrap hook to ask the embedding to prepare an object
 * for wrapping in a context. This might include unwrapping other wrappers
 * or even finding a more suitable object for the new compartment.
 */
typedef void
(* JSPreWrapCallback)(JSContext* cx, JS::HandleObject scope, JS::HandleObject obj,
                      JS::HandleObject objectPassedToWrap,
                      JS::MutableHandleObject retObj);

struct JSWrapObjectCallbacks
{
    JSWrapObjectCallback wrap;
    JSPreWrapCallback preWrap;
};

typedef void
(* JSDestroyCompartmentCallback)(JSFreeOp* fop, JSCompartment* compartment);

typedef size_t
(* JSSizeOfIncludingThisCompartmentCallback)(mozilla::MallocSizeOf mallocSizeOf,
                                             JSCompartment* compartment);

typedef void
(* JSCompartmentNameCallback)(JSContext* cx, JSCompartment* compartment,
                              char* buf, size_t bufsize);

/**
 * Callback used by memory reporting to ask the embedder how much memory an
 * external string is keeping alive.  The embedder is expected to return a value
 * that corresponds to the size of the allocation that will be released by the
 * JSStringFinalizer passed to JS_NewExternalString for this string.
 *
 * Implementations of this callback MUST NOT do anything that can cause GC.
 */
using JSExternalStringSizeofCallback =
    size_t (*)(JSString* str, mozilla::MallocSizeOf mallocSizeOf);

/**
 * Callback used to intercept JavaScript errors.
 */
struct JSErrorInterceptor {
    /**
     * This method is called whenever an error has been raised from JS code.
     *
     * This method MUST be infallible.
     */
    virtual void interceptError(JSContext* cx, const JS::Value& error) = 0;
};

/************************************************************************/

static MOZ_ALWAYS_INLINE JS::Value
JS_NumberValue(double d)
{
    int32_t i;
    d = JS::CanonicalizeNaN(d);
    if (mozilla::NumberIsInt32(d, &i))
        return JS::Int32Value(i);
    return JS::DoubleValue(d);
}

/************************************************************************/

JS_PUBLIC_API(bool)
JS_StringHasBeenPinned(JSContext* cx, JSString* str);

namespace JS {

/**
 * Container class for passing in script source buffers to the JS engine.  This
 * not only groups the buffer and length values, it also provides a way to
 * optionally pass ownership of the buffer to the JS engine without copying.
 * Rules for use:
 *
 *  1) The data array must be allocated with js_malloc() or js_realloc() if
 *     ownership is being granted to the SourceBufferHolder.
 *  2) If ownership is not given to the SourceBufferHolder, then the memory
 *     must be kept alive until the JS compilation is complete.
 *  3) Any code calling SourceBufferHolder::take() must guarantee to keep the
 *     memory alive until JS compilation completes.  Normally only the JS
 *     engine should be calling take().
 *
 * Example use:
 *
 *    size_t length = 512;
 *    char16_t* chars = static_cast<char16_t*>(js_malloc(sizeof(char16_t) * length));
 *    JS::SourceBufferHolder srcBuf(chars, length, JS::SourceBufferHolder::GiveOwnership);
 *    JS::Compile(cx, options, srcBuf);
 */
class MOZ_STACK_CLASS SourceBufferHolder final
{
  public:
    enum Ownership {
      NoOwnership,
      GiveOwnership
    };

    SourceBufferHolder(const char16_t* data, size_t dataLength, Ownership ownership)
      : data_(data),
        length_(dataLength),
        ownsChars_(ownership == GiveOwnership)
    {
        // Ensure that null buffers properly return an unowned, empty,
        // null-terminated string.
        static const char16_t NullChar_ = 0;
        if (!get()) {
            data_ = &NullChar_;
            length_ = 0;
            ownsChars_ = false;
        }
    }

    SourceBufferHolder(SourceBufferHolder&& other)
      : data_(other.data_),
        length_(other.length_),
        ownsChars_(other.ownsChars_)
    {
        other.data_ = nullptr;
        other.length_ = 0;
        other.ownsChars_ = false;
    }

    ~SourceBufferHolder() {
        if (ownsChars_)
            js_free(const_cast<char16_t*>(data_));
    }

    // Access the underlying source buffer without affecting ownership.
    const char16_t* get() const { return data_; }

    // Length of the source buffer in char16_t code units (not bytes)
    size_t length() const { return length_; }

    // Returns true if the SourceBufferHolder owns the buffer and will free
    // it upon destruction.  If true, it is legal to call take().
    bool ownsChars() const { return ownsChars_; }

    // Retrieve and take ownership of the underlying data buffer.  The caller
    // is now responsible for calling js_free() on the returned value, *but only
    // after JS script compilation has completed*.
    //
    // After the buffer has been taken the SourceBufferHolder functions as if
    // it had been constructed on an unowned buffer;  get() and length() still
    // work.  In order for this to be safe the taken buffer must be kept alive
    // until after JS script compilation completes as noted above.
    //
    // Note, it's the caller's responsibility to check ownsChars() before taking
    // the buffer.  Taking and then free'ing an unowned buffer will have dire
    // consequences.
    char16_t* take() {
        MOZ_ASSERT(ownsChars_);
        ownsChars_ = false;
        return const_cast<char16_t*>(data_);
    }

  private:
    SourceBufferHolder(SourceBufferHolder&) = delete;
    SourceBufferHolder& operator=(SourceBufferHolder&) = delete;

    const char16_t* data_;
    size_t length_;
    bool ownsChars_;
};

struct TranscodeSource;

} /* namespace JS */

/************************************************************************/

/* Property attributes, set in JSPropertySpec and passed to API functions.
 *
 * NB: The data structure in which some of these values are stored only uses
 *     a uint8_t to store the relevant information. Proceed with caution if
 *     trying to reorder or change the the first byte worth of flags.
 */

/* property is visible to for/in loop */
static const uint8_t JSPROP_ENUMERATE =        0x01;

/* not settable: assignment is no-op.  This flag is only valid when neither
   JSPROP_GETTER nor JSPROP_SETTER is set. */
static const uint8_t JSPROP_READONLY =         0x02;

/* property cannot be deleted */
static const uint8_t JSPROP_PERMANENT =        0x04;

/* Passed to JS_Define(UC)Property* and JS_DefineElement if getters/setters are
   JSGetterOp/JSSetterOp */
static const uint8_t JSPROP_PROPOP_ACCESSORS = 0x08;

/* property holds getter function */
static const uint8_t JSPROP_GETTER =           0x10;

/* property holds setter function */
static const uint8_t JSPROP_SETTER =           0x20;

/* internal JS engine use only */
static const uint8_t JSPROP_INTERNAL_USE_BIT = 0x80;

/* native that can be called as a ctor */
static const unsigned JSFUN_CONSTRUCTOR =     0x400;

/* | of all the JSFUN_* flags */
static const unsigned JSFUN_FLAGS_MASK =      0x400;

/*
 * If set, will allow redefining a non-configurable property, but only on a
 * non-DOM global.  This is a temporary hack that will need to go away in bug
 * 1105518.
 */
static const unsigned JSPROP_REDEFINE_NONCONFIGURABLE = 0x1000;

/*
 * Resolve hooks and enumerate hooks must pass this flag when calling
 * JS_Define* APIs to reify lazily-defined properties.
 *
 * JSPROP_RESOLVING is used only with property-defining APIs. It tells the
 * engine to skip the resolve hook when performing the lookup at the beginning
 * of property definition. This keeps the resolve hook from accidentally
 * triggering itself: unchecked recursion.
 *
 * For enumerate hooks, triggering the resolve hook would be merely silly, not
 * fatal, except in some cases involving non-configurable properties.
 */
static const unsigned JSPROP_RESOLVING =         0x2000;

/* ignore the value in JSPROP_ENUMERATE.  This flag only valid when defining
   over an existing property. */
static const unsigned JSPROP_IGNORE_ENUMERATE =  0x4000;

/* ignore the value in JSPROP_READONLY.  This flag only valid when defining over
   an existing property. */
static const unsigned JSPROP_IGNORE_READONLY =   0x8000;

/* ignore the value in JSPROP_PERMANENT.  This flag only valid when defining
   over an existing property. */
static const unsigned JSPROP_IGNORE_PERMANENT = 0x10000;

/* ignore the Value in the descriptor. Nothing was specified when passed to
   Object.defineProperty from script. */
static const unsigned JSPROP_IGNORE_VALUE =     0x20000;

/** Microseconds since the epoch, midnight, January 1, 1970 UTC. */
extern JS_PUBLIC_API(int64_t)
JS_Now(void);

/** Don't want to export data, so provide accessors for non-inline Values. */
extern JS_PUBLIC_API(JS::Value)
JS_GetNaNValue(JSContext* cx);

extern JS_PUBLIC_API(JS::Value)
JS_GetNegativeInfinityValue(JSContext* cx);

extern JS_PUBLIC_API(JS::Value)
JS_GetPositiveInfinityValue(JSContext* cx);

extern JS_PUBLIC_API(JS::Value)
JS_GetEmptyStringValue(JSContext* cx);

extern JS_PUBLIC_API(JSString*)
JS_GetEmptyString(JSContext* cx);

extern JS_PUBLIC_API(bool)
JS_ValueToObject(JSContext* cx, JS::HandleValue v, JS::MutableHandleObject objp);

extern JS_PUBLIC_API(JSFunction*)
JS_ValueToFunction(JSContext* cx, JS::HandleValue v);

extern JS_PUBLIC_API(JSFunction*)
JS_ValueToConstructor(JSContext* cx, JS::HandleValue v);

extern JS_PUBLIC_API(JSString*)
JS_ValueToSource(JSContext* cx, JS::Handle<JS::Value> v);

extern JS_PUBLIC_API(bool)
JS_DoubleIsInt32(double d, int32_t* ip);

extern JS_PUBLIC_API(JSType)
JS_TypeOfValue(JSContext* cx, JS::Handle<JS::Value> v);

namespace JS {

extern JS_PUBLIC_API(const char*)
InformalValueTypeName(const JS::Value& v);

} /* namespace JS */

extern JS_PUBLIC_API(bool)
JS_StrictlyEqual(JSContext* cx, JS::Handle<JS::Value> v1, JS::Handle<JS::Value> v2, bool* equal);

extern JS_PUBLIC_API(bool)
JS_LooselyEqual(JSContext* cx, JS::Handle<JS::Value> v1, JS::Handle<JS::Value> v2, bool* equal);

extern JS_PUBLIC_API(bool)
JS_SameValue(JSContext* cx, JS::Handle<JS::Value> v1, JS::Handle<JS::Value> v2, bool* same);

/** True iff fun is the global eval function. */
extern JS_PUBLIC_API(bool)
JS_IsBuiltinEvalFunction(JSFunction* fun);

/** True iff fun is the Function constructor. */
extern JS_PUBLIC_API(bool)
JS_IsBuiltinFunctionConstructor(JSFunction* fun);

/************************************************************************/

/*
 * Locking, contexts, and memory allocation.
 *
 * It is important that SpiderMonkey be initialized, and the first context
 * be created, in a single-threaded fashion.  Otherwise the behavior of the
 * library is undefined.
 * See: https://developer.mozilla.org/en-US/docs/Mozilla/Projects/SpiderMonkey/JSAPI_reference
 */

// Create a new runtime, with a single cooperative context for this thread.
// On success, the new context will be the active context for the runtime.
extern JS_PUBLIC_API(JSContext*)
JS_NewContext(uint32_t maxbytes,
              uint32_t maxNurseryBytes = JS::DefaultNurseryBytes,
              JSRuntime* parentRuntime = nullptr);

// The methods below for controlling the active context in a cooperatively
// multithreaded runtime are not threadsafe, and the caller must ensure they
// are called serially if there is a chance for contention between threads.

// Called from the active context for a runtime, yield execution so that
// this context is no longer active and can no longer use the API.
extern JS_PUBLIC_API(void)
JS_YieldCooperativeContext(JSContext* cx);

// Called from a context whose runtime has no active context, this thread
// becomes the active context for that runtime and may use the API.
extern JS_PUBLIC_API(void)
JS_ResumeCooperativeContext(JSContext* cx);

// Create a new context on this thread for cooperative multithreading in the
// same runtime as siblingContext. Called on a runtime (as indicated by
// siblingContet) which has no active context, on success the new context will
// become the runtime's active context.
extern JS_PUBLIC_API(JSContext*)
JS_NewCooperativeContext(JSContext* siblingContext);

namespace JS {

// Class to relinquish exclusive access to all zone groups in use by this
// thread. This allows other cooperative threads to enter the zone groups
// and modify their contents.
struct AutoRelinquishZoneGroups
{
    explicit AutoRelinquishZoneGroups(JSContext* cx);
    ~AutoRelinquishZoneGroups();

  private:
    JSContext* cx;
    mozilla::Vector<void*> enterList;
};

} // namespace JS

// Destroy a context allocated with JS_NewContext or JS_NewCooperativeContext.
// The context must be the current active context in the runtime, and after
// this call the runtime will have no active context.
extern JS_PUBLIC_API(void)
JS_DestroyContext(JSContext* cx);

JS_PUBLIC_API(void*)
JS_GetContextPrivate(JSContext* cx);

JS_PUBLIC_API(void)
JS_SetContextPrivate(JSContext* cx, void* data);

extern JS_PUBLIC_API(JSRuntime*)
JS_GetParentRuntime(JSContext* cx);

extern JS_PUBLIC_API(JSRuntime*)
JS_GetRuntime(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_BeginRequest(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_EndRequest(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_SetFutexCanWait(JSContext* cx);

namespace JS {

// Single threaded execution callbacks are used to notify API clients that a
// feature is in use on a context's runtime that is not yet compatible with
// cooperatively multithreaded execution.
//
// Between a call to BeginSingleThreadedExecutionCallback and a corresponding
// call to EndSingleThreadedExecutionCallback, only one thread at a time may
// enter compartments in the runtime. The begin callback may yield as necessary
// to permit other threads to finish up what they're doing, while the end
// callback may not yield or otherwise operate on the runtime (it may be called
// during GC).
//
// These callbacks may be left unspecified for runtimes which only ever have a
// single context.
typedef void (*BeginSingleThreadedExecutionCallback)(JSContext* cx);
typedef void (*EndSingleThreadedExecutionCallback)(JSContext* cx);

extern JS_PUBLIC_API(void)
SetSingleThreadedExecutionCallbacks(JSContext* cx,
                                    BeginSingleThreadedExecutionCallback begin,
                                    EndSingleThreadedExecutionCallback end);

} // namespace JS

namespace js {

void
AssertHeapIsIdle();

} /* namespace js */

class MOZ_RAII JSAutoRequest
{
  public:
    explicit JSAutoRequest(JSContext* cx
                           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : mContext(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        JS_BeginRequest(mContext);
    }
    ~JSAutoRequest() {
        JS_EndRequest(mContext);
    }

  protected:
    JSContext* mContext;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

#if 0
  private:
    static void* operator new(size_t) CPP_THROW_NEW { return 0; }
    static void operator delete(void*, size_t) { }
#endif
};

namespace JS {

class JS_PUBLIC_API(ContextOptions) {
  public:
    ContextOptions()
      : baseline_(true),
        ion_(true),
        asmJS_(true),
        wasm_(true),
        wasmBaseline_(true),
        wasmIon_(true),
        testWasmAwaitTier2_(false),
        throwOnAsmJSValidationFailure_(false),
        nativeRegExp_(true),
        asyncStack_(true),
        throwOnDebuggeeWouldRun_(true),
        dumpStackOnDebuggeeWouldRun_(false),
        werror_(false),
        strictMode_(false),
        extraWarnings_(false),
        streams_(false)
#ifdef FUZZING
        , fuzzing_(false)
#endif
        , expressionClosures_(false)
        , arrayProtoValues_(true)
    {
    }

    bool baseline() const { return baseline_; }
    ContextOptions& setBaseline(bool flag) {
        baseline_ = flag;
        return *this;
    }
    ContextOptions& toggleBaseline() {
        baseline_ = !baseline_;
        return *this;
    }

    bool ion() const { return ion_; }
    ContextOptions& setIon(bool flag) {
        ion_ = flag;
        return *this;
    }
    ContextOptions& toggleIon() {
        ion_ = !ion_;
        return *this;
    }

    bool asmJS() const { return asmJS_; }
    ContextOptions& setAsmJS(bool flag) {
        asmJS_ = flag;
        return *this;
    }
    ContextOptions& toggleAsmJS() {
        asmJS_ = !asmJS_;
        return *this;
    }

    bool wasm() const { return wasm_; }
    ContextOptions& setWasm(bool flag) {
        wasm_ = flag;
        return *this;
    }
    ContextOptions& toggleWasm() {
        wasm_ = !wasm_;
        return *this;
    }

    bool streams() const { return streams_; }
    ContextOptions& setStreams(bool flag) {
        streams_ = flag;
        return *this;
    }
    ContextOptions& toggleStreams() {
        streams_ = !streams_;
        return *this;
    }

    bool wasmBaseline() const { return wasmBaseline_; }
    ContextOptions& setWasmBaseline(bool flag) {
        wasmBaseline_ = flag;
        return *this;
    }
    ContextOptions& toggleWasmBaseline() {
        wasmBaseline_ = !wasmBaseline_;
        return *this;
    }

    bool wasmIon() const { return wasmIon_; }
    ContextOptions& setWasmIon(bool flag) {
        wasmIon_ = flag;
        return *this;
    }
    ContextOptions& toggleWasmIon() {
        wasmIon_ = !wasmIon_;
        return *this;
    }

    bool testWasmAwaitTier2() const { return testWasmAwaitTier2_; }
    ContextOptions& setTestWasmAwaitTier2(bool flag) {
        testWasmAwaitTier2_ = flag;
        return *this;
    }
    ContextOptions& toggleTestWasmAwaitTier2() {
        testWasmAwaitTier2_ = !testWasmAwaitTier2_;
        return *this;
    }

    bool throwOnAsmJSValidationFailure() const { return throwOnAsmJSValidationFailure_; }
    ContextOptions& setThrowOnAsmJSValidationFailure(bool flag) {
        throwOnAsmJSValidationFailure_ = flag;
        return *this;
    }
    ContextOptions& toggleThrowOnAsmJSValidationFailure() {
        throwOnAsmJSValidationFailure_ = !throwOnAsmJSValidationFailure_;
        return *this;
    }

    bool nativeRegExp() const { return nativeRegExp_; }
    ContextOptions& setNativeRegExp(bool flag) {
        nativeRegExp_ = flag;
        return *this;
    }

    bool asyncStack() const { return asyncStack_; }
    ContextOptions& setAsyncStack(bool flag) {
        asyncStack_ = flag;
        return *this;
    }

    bool throwOnDebuggeeWouldRun() const { return throwOnDebuggeeWouldRun_; }
    ContextOptions& setThrowOnDebuggeeWouldRun(bool flag) {
        throwOnDebuggeeWouldRun_ = flag;
        return *this;
    }

    bool dumpStackOnDebuggeeWouldRun() const { return dumpStackOnDebuggeeWouldRun_; }
    ContextOptions& setDumpStackOnDebuggeeWouldRun(bool flag) {
        dumpStackOnDebuggeeWouldRun_ = flag;
        return *this;
    }

    bool werror() const { return werror_; }
    ContextOptions& setWerror(bool flag) {
        werror_ = flag;
        return *this;
    }
    ContextOptions& toggleWerror() {
        werror_ = !werror_;
        return *this;
    }

    bool strictMode() const { return strictMode_; }
    ContextOptions& setStrictMode(bool flag) {
        strictMode_ = flag;
        return *this;
    }
    ContextOptions& toggleStrictMode() {
        strictMode_ = !strictMode_;
        return *this;
    }

    bool extraWarnings() const { return extraWarnings_; }
    ContextOptions& setExtraWarnings(bool flag) {
        extraWarnings_ = flag;
        return *this;
    }
    ContextOptions& toggleExtraWarnings() {
        extraWarnings_ = !extraWarnings_;
        return *this;
    }

#ifdef FUZZING
    bool fuzzing() const { return fuzzing_; }
    ContextOptions& setFuzzing(bool flag) {
        fuzzing_ = flag;
        return *this;
    }
#endif

    bool expressionClosures() const { return expressionClosures_; }
    ContextOptions& setExpressionClosures(bool flag) {
        expressionClosures_ = flag;
        return *this;
    }

    bool arrayProtoValues() const { return arrayProtoValues_; }
    ContextOptions& setArrayProtoValues(bool flag) {
        arrayProtoValues_ = flag;
        return *this;
    }

    void disableOptionsForSafeMode() {
        setBaseline(false);
        setIon(false);
        setAsmJS(false);
        setWasm(false);
        setWasmBaseline(false);
        setWasmIon(false);
        setNativeRegExp(false);
    }

  private:
    bool baseline_ : 1;
    bool ion_ : 1;
    bool asmJS_ : 1;
    bool wasm_ : 1;
    bool wasmBaseline_ : 1;
    bool wasmIon_ : 1;
    bool testWasmAwaitTier2_ : 1;
    bool throwOnAsmJSValidationFailure_ : 1;
    bool nativeRegExp_ : 1;
    bool asyncStack_ : 1;
    bool throwOnDebuggeeWouldRun_ : 1;
    bool dumpStackOnDebuggeeWouldRun_ : 1;
    bool werror_ : 1;
    bool strictMode_ : 1;
    bool extraWarnings_ : 1;
    bool streams_: 1;
#ifdef FUZZING
    bool fuzzing_ : 1;
#endif
    bool expressionClosures_ : 1;
    bool arrayProtoValues_ : 1;

};

JS_PUBLIC_API(ContextOptions&)
ContextOptionsRef(JSContext* cx);

/**
 * Initialize the runtime's self-hosted code. Embeddings should call this
 * exactly once per runtime/context, before the first JS_NewGlobalObject
 * call.
 */
JS_PUBLIC_API(bool)
InitSelfHostedCode(JSContext* cx);

/**
 * Asserts (in debug and release builds) that `obj` belongs to the current
 * thread's context.
 */
JS_PUBLIC_API(void)
AssertObjectBelongsToCurrentThread(JSObject* obj);

} /* namespace JS */

extern JS_PUBLIC_API(const char*)
JS_GetImplementationVersion(void);

extern JS_PUBLIC_API(void)
JS_SetDestroyCompartmentCallback(JSContext* cx, JSDestroyCompartmentCallback callback);

extern JS_PUBLIC_API(void)
JS_SetSizeOfIncludingThisCompartmentCallback(JSContext* cx,
                                             JSSizeOfIncludingThisCompartmentCallback callback);

extern JS_PUBLIC_API(void)
JS_SetCompartmentNameCallback(JSContext* cx, JSCompartmentNameCallback callback);

extern JS_PUBLIC_API(void)
JS_SetWrapObjectCallbacks(JSContext* cx, const JSWrapObjectCallbacks* callbacks);

extern JS_PUBLIC_API(void)
JS_SetExternalStringSizeofCallback(JSContext* cx, JSExternalStringSizeofCallback callback);

#if defined(NIGHTLY_BUILD)

// Set a callback that will be called whenever an error
// is thrown in this runtime. This is designed as a mechanism
// for logging errors. Note that the VM makes no attempt to sanitize
// the contents of the error (so it may contain private data)
// or to sort out among errors (so it may not be the error you
// are interested in or for the component in which you are
// interested).
//
// If the callback sets a new error, this new error
// will replace the original error.
//
// May be `nullptr`.
extern JS_PUBLIC_API(void)
JS_SetErrorInterceptorCallback(JSRuntime*, JSErrorInterceptor* callback);

extern JS_PUBLIC_API(JSErrorInterceptor*)
JS_GetErrorInterceptorCallback(JSRuntime*);

// Examine a value to determine if it is one of the built-in Error types.
// If so, return the error type.
extern JS_PUBLIC_API(mozilla::Maybe<JSExnType>)
JS_GetErrorType(const JS::Value& val);

#endif // defined(NIGHTLY_BUILD)

extern JS_PUBLIC_API(void)
JS_SetCompartmentPrivate(JSCompartment* compartment, void* data);

extern JS_PUBLIC_API(void*)
JS_GetCompartmentPrivate(JSCompartment* compartment);

extern JS_PUBLIC_API(void)
JS_SetZoneUserData(JS::Zone* zone, void* data);

extern JS_PUBLIC_API(void*)
JS_GetZoneUserData(JS::Zone* zone);

extern JS_PUBLIC_API(bool)
JS_WrapObject(JSContext* cx, JS::MutableHandleObject objp);

extern JS_PUBLIC_API(bool)
JS_WrapValue(JSContext* cx, JS::MutableHandleValue vp);

extern JS_PUBLIC_API(JSObject*)
JS_TransplantObject(JSContext* cx, JS::HandleObject origobj, JS::HandleObject target);

extern JS_PUBLIC_API(bool)
JS_RefreshCrossCompartmentWrappers(JSContext* cx, JS::Handle<JSObject*> obj);

/*
 * At any time, a JSContext has a current (possibly-nullptr) compartment.
 * Compartments are described in:
 *
 *   developer.mozilla.org/en-US/docs/SpiderMonkey/SpiderMonkey_compartments
 *
 * The current compartment of a context may be changed. The preferred way to do
 * this is with JSAutoCompartment:
 *
 *   void foo(JSContext* cx, JSObject* obj) {
 *     // in some compartment 'c'
 *     {
 *       JSAutoCompartment ac(cx, obj);  // constructor enters
 *       // in the compartment of 'obj'
 *     }                                 // destructor leaves
 *     // back in compartment 'c'
 *   }
 *
 * For more complicated uses that don't neatly fit in a C++ stack frame, the
 * compartment can entered and left using separate function calls:
 *
 *   void foo(JSContext* cx, JSObject* obj) {
 *     // in 'oldCompartment'
 *     JSCompartment* oldCompartment = JS_EnterCompartment(cx, obj);
 *     // in the compartment of 'obj'
 *     JS_LeaveCompartment(cx, oldCompartment);
 *     // back in 'oldCompartment'
 *   }
 *
 * Note: these calls must still execute in a LIFO manner w.r.t all other
 * enter/leave calls on the context. Furthermore, only the return value of a
 * JS_EnterCompartment call may be passed as the 'oldCompartment' argument of
 * the corresponding JS_LeaveCompartment call.
 *
 * Entering a compartment roots the compartment and its global object for the
 * lifetime of the JSAutoCompartment.
 */

class MOZ_RAII JS_PUBLIC_API(JSAutoCompartment)
{
    JSContext* cx_;
    JSCompartment* oldCompartment_;
  public:
    JSAutoCompartment(JSContext* cx, JSObject* target
                      MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    JSAutoCompartment(JSContext* cx, JSScript* target
                      MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    ~JSAutoCompartment();

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class MOZ_RAII JS_PUBLIC_API(JSAutoNullableCompartment)
{
    JSContext* cx_;
    JSCompartment* oldCompartment_;
  public:
    explicit JSAutoNullableCompartment(JSContext* cx, JSObject* targetOrNull
                                       MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    ~JSAutoNullableCompartment();

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/** NB: This API is infallible; a nullptr return value does not indicate error.
 *
 * Entering a compartment roots the compartment and its global object until the
 * matching JS_LeaveCompartment() call.
 */
extern JS_PUBLIC_API(JSCompartment*)
JS_EnterCompartment(JSContext* cx, JSObject* target);

extern JS_PUBLIC_API(void)
JS_LeaveCompartment(JSContext* cx, JSCompartment* oldCompartment);

typedef void (*JSIterateCompartmentCallback)(JSContext* cx, void* data, JSCompartment* compartment);

/**
 * This function calls |compartmentCallback| on every compartment. Beware that
 * there is no guarantee that the compartment will survive after the callback
 * returns. Also, barriers are disabled via the TraceSession.
 */
extern JS_PUBLIC_API(void)
JS_IterateCompartments(JSContext* cx, void* data,
                       JSIterateCompartmentCallback compartmentCallback);

/**
 * Mark a jsid after entering a new compartment. Different zones separately
 * mark the ids in a runtime, and this must be used any time an id is obtained
 * from one compartment and then used in another compartment, unless the two
 * compartments are guaranteed to be in the same zone.
 */
extern JS_PUBLIC_API(void)
JS_MarkCrossZoneId(JSContext* cx, jsid id);

/**
 * If value stores a jsid (an atomized string or symbol), mark that id as for
 * JS_MarkCrossZoneId.
 */
extern JS_PUBLIC_API(void)
JS_MarkCrossZoneIdValue(JSContext* cx, const JS::Value& value);

/**
 * Initialize standard JS class constructors, prototypes, and any top-level
 * functions and constants associated with the standard classes (e.g. isNaN
 * for Number).
 *
 * NB: This sets cx's global object to obj if it was null.
 */
extern JS_PUBLIC_API(bool)
JS_InitStandardClasses(JSContext* cx, JS::Handle<JSObject*> obj);

/**
 * Resolve id, which must contain either a string or an int, to a standard
 * class name in obj if possible, defining the class's constructor and/or
 * prototype and storing true in *resolved.  If id does not name a standard
 * class or a top-level property induced by initializing a standard class,
 * store false in *resolved and just return true.  Return false on error,
 * as usual for bool result-typed API entry points.
 *
 * This API can be called directly from a global object class's resolve op,
 * to define standard classes lazily. The class should either have an enumerate
 * hook that calls JS_EnumerateStandardClasses, or a newEnumerate hook that
 * calls JS_NewEnumerateStandardClasses. newEnumerate is preferred because it's
 * faster (does not define all standard classes).
 */
extern JS_PUBLIC_API(bool)
JS_ResolveStandardClass(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolved);

extern JS_PUBLIC_API(bool)
JS_MayResolveStandardClass(const JSAtomState& names, jsid id, JSObject* maybeObj);

extern JS_PUBLIC_API(bool)
JS_EnumerateStandardClasses(JSContext* cx, JS::HandleObject obj);

extern JS_PUBLIC_API(bool)
JS_NewEnumerateStandardClasses(JSContext* cx, JS::HandleObject obj, JS::AutoIdVector& properties,
                               bool enumerableOnly);

extern JS_PUBLIC_API(bool)
JS_GetClassObject(JSContext* cx, JSProtoKey key, JS::MutableHandle<JSObject*> objp);

extern JS_PUBLIC_API(bool)
JS_GetClassPrototype(JSContext* cx, JSProtoKey key, JS::MutableHandle<JSObject*> objp);

namespace JS {

/*
 * Determine if the given object is an instance/prototype/constructor for a standard
 * class. If so, return the associated JSProtoKey. If not, return JSProto_Null.
 */

extern JS_PUBLIC_API(JSProtoKey)
IdentifyStandardInstance(JSObject* obj);

extern JS_PUBLIC_API(JSProtoKey)
IdentifyStandardPrototype(JSObject* obj);

extern JS_PUBLIC_API(JSProtoKey)
IdentifyStandardInstanceOrPrototype(JSObject* obj);

extern JS_PUBLIC_API(JSProtoKey)
IdentifyStandardConstructor(JSObject* obj);

extern JS_PUBLIC_API(void)
ProtoKeyToId(JSContext* cx, JSProtoKey key, JS::MutableHandleId idp);

} /* namespace JS */

extern JS_PUBLIC_API(JSProtoKey)
JS_IdToProtoKey(JSContext* cx, JS::HandleId id);

/**
 * Returns the original value of |Function.prototype| from the global object in
 * which |forObj| was created.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetFunctionPrototype(JSContext* cx, JS::HandleObject forObj);

/**
 * Returns the original value of |Object.prototype| from the global object in
 * which |forObj| was created.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetObjectPrototype(JSContext* cx, JS::HandleObject forObj);

/**
 * Returns the original value of |Array.prototype| from the global object in
 * which |forObj| was created.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetArrayPrototype(JSContext* cx, JS::HandleObject forObj);

/**
 * Returns the original value of |Error.prototype| from the global
 * object of the current compartment of cx.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetErrorPrototype(JSContext* cx);

/**
 * Returns the %IteratorPrototype% object that all built-in iterator prototype
 * chains go through for the global object of the current compartment of cx.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetIteratorPrototype(JSContext* cx);

extern JS_PUBLIC_API(JSObject*)
JS_GetGlobalForObject(JSContext* cx, JSObject* obj);

extern JS_PUBLIC_API(bool)
JS_IsGlobalObject(JSObject* obj);

extern JS_PUBLIC_API(JSObject*)
JS_GlobalLexicalEnvironment(JSObject* obj);

extern JS_PUBLIC_API(bool)
JS_HasExtensibleLexicalEnvironment(JSObject* obj);

extern JS_PUBLIC_API(JSObject*)
JS_ExtensibleLexicalEnvironment(JSObject* obj);

/**
 * May return nullptr, if |c| never had a global (e.g. the atoms compartment),
 * or if |c|'s global has been collected.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetGlobalForCompartmentOrNull(JSContext* cx, JSCompartment* c);

namespace JS {

extern JS_PUBLIC_API(JSObject*)
CurrentGlobalOrNull(JSContext* cx);

} // namespace JS

/**
 * Add 'Reflect.parse', a SpiderMonkey extension, to the Reflect object on the
 * given global.
 */
extern JS_PUBLIC_API(bool)
JS_InitReflectParse(JSContext* cx, JS::HandleObject global);

/**
 * Add various profiling-related functions as properties of the given object.
 * Defined in builtin/Profilers.cpp.
 */
extern JS_PUBLIC_API(bool)
JS_DefineProfilingFunctions(JSContext* cx, JS::HandleObject obj);

/* Defined in vm/Debugger.cpp. */
extern JS_PUBLIC_API(bool)
JS_DefineDebuggerObject(JSContext* cx, JS::HandleObject obj);

#ifdef JS_HAS_CTYPES
/**
 * Initialize the 'ctypes' object on a global variable 'obj'. The 'ctypes'
 * object will be sealed.
 */
extern JS_PUBLIC_API(bool)
JS_InitCTypesClass(JSContext* cx, JS::HandleObject global);

/**
 * Convert a unicode string 'source' of length 'slen' to the platform native
 * charset, returning a null-terminated string allocated with JS_malloc. On
 * failure, this function should report an error.
 */
typedef char*
(* JSCTypesUnicodeToNativeFun)(JSContext* cx, const char16_t* source, size_t slen);

/**
 * Set of function pointers that ctypes can use for various internal functions.
 * See JS_SetCTypesCallbacks below. Providing nullptr for a function is safe,
 * and will result in the applicable ctypes functionality not being available.
 */
struct JSCTypesCallbacks {
    JSCTypesUnicodeToNativeFun unicodeToNative;
};

/**
 * Set the callbacks on the provided 'ctypesObj' object. 'callbacks' should be a
 * pointer to static data that exists for the lifetime of 'ctypesObj', but it
 * may safely be altered after calling this function and without having
 * to call this function again.
 */
extern JS_PUBLIC_API(void)
JS_SetCTypesCallbacks(JSObject* ctypesObj, const JSCTypesCallbacks* callbacks);
#endif

extern JS_PUBLIC_API(void*)
JS_malloc(JSContext* cx, size_t nbytes);

extern JS_PUBLIC_API(void*)
JS_realloc(JSContext* cx, void* p, size_t oldBytes, size_t newBytes);

/**
 * A wrapper for js_free(p) that may delay js_free(p) invocation as a
 * performance optimization.
 * cx may be nullptr.
 */
extern JS_PUBLIC_API(void)
JS_free(JSContext* cx, void* p);

/**
 * A wrapper for js_free(p) that may delay js_free(p) invocation as a
 * performance optimization as specified by the given JSFreeOp instance.
 */
extern JS_PUBLIC_API(void)
JS_freeop(JSFreeOp* fop, void* p);

extern JS_PUBLIC_API(void)
JS_updateMallocCounter(JSContext* cx, size_t nbytes);

extern JS_PUBLIC_API(char*)
JS_strdup(JSContext* cx, const char* s);

/**
 * Set the size of the native stack that should not be exceed. To disable
 * stack size checking pass 0.
 *
 * SpiderMonkey allows for a distinction between system code (such as GCs, which
 * may incidentally be triggered by script but are not strictly performed on
 * behalf of such script), trusted script (as determined by JS_SetTrustedPrincipals),
 * and untrusted script. Each kind of code may have a different stack quota,
 * allowing embedders to keep higher-priority machinery running in the face of
 * scripted stack exhaustion by something else.
 *
 * The stack quotas for each kind of code should be monotonically descending,
 * and may be specified with this function. If 0 is passed for a given kind
 * of code, it defaults to the value of the next-highest-priority kind.
 *
 * This function may only be called immediately after the runtime is initialized
 * and before any code is executed and/or interrupts requested.
 */
extern JS_PUBLIC_API(void)
JS_SetNativeStackQuota(JSContext* cx, size_t systemCodeStackSize,
                       size_t trustedScriptStackSize = 0,
                       size_t untrustedScriptStackSize = 0);

/************************************************************************/

extern JS_PUBLIC_API(bool)
JS_ValueToId(JSContext* cx, JS::HandleValue v, JS::MutableHandleId idp);

extern JS_PUBLIC_API(bool)
JS_StringToId(JSContext* cx, JS::HandleString s, JS::MutableHandleId idp);

extern JS_PUBLIC_API(bool)
JS_IdToValue(JSContext* cx, jsid id, JS::MutableHandle<JS::Value> vp);

namespace JS {

/**
 * Convert obj to a primitive value. On success, store the result in vp and
 * return true.
 *
 * The hint argument must be JSTYPE_STRING, JSTYPE_NUMBER, or
 * JSTYPE_UNDEFINED (no hint).
 *
 * Implements: ES6 7.1.1 ToPrimitive(input, [PreferredType]).
 */
extern JS_PUBLIC_API(bool)
ToPrimitive(JSContext* cx, JS::HandleObject obj, JSType hint, JS::MutableHandleValue vp);

/**
 * If args.get(0) is one of the strings "string", "number", or "default", set
 * result to JSTYPE_STRING, JSTYPE_NUMBER, or JSTYPE_UNDEFINED accordingly and
 * return true. Otherwise, return false with a TypeError pending.
 *
 * This can be useful in implementing a @@toPrimitive method.
 */
extern JS_PUBLIC_API(bool)
GetFirstArgumentAsTypeHint(JSContext* cx, CallArgs args, JSType *result);

} /* namespace JS */

template<typename T>
struct JSConstScalarSpec {
    const char* name;
    T val;
};

typedef JSConstScalarSpec<double> JSConstDoubleSpec;
typedef JSConstScalarSpec<int32_t> JSConstIntegerSpec;

struct JSJitInfo;

/**
 * Wrapper to relace JSNative for JSPropertySpecs and JSFunctionSpecs. This will
 * allow us to pass one JSJitInfo per function with the property/function spec,
 * without additional field overhead.
 */
struct JSNativeWrapper {
    JSNative        op;
    const JSJitInfo* info;
};

/*
 * Macro static initializers which make it easy to pass no JSJitInfo as part of a
 * JSPropertySpec or JSFunctionSpec.
 */
#define JSNATIVE_WRAPPER(native) { {native, nullptr} }

/**
 * Description of a property. JS_DefineProperties and JS_InitClass take arrays
 * of these and define many properties at once. JS_PSG, JS_PSGS and JS_PS_END
 * are helper macros for defining such arrays.
 */
struct JSPropertySpec {
    struct SelfHostedWrapper {
        void*       unused;
        const char* funname;
    };

    struct ValueWrapper {
        uintptr_t   type;
        union {
            const char* string;
            int32_t     int32;
        };
    };

    const char*                 name;
    uint8_t                     flags;
    union {
        struct {
            union {
                JSNativeWrapper    native;
                SelfHostedWrapper  selfHosted;
            } getter;
            union {
                JSNativeWrapper    native;
                SelfHostedWrapper  selfHosted;
            } setter;
        } accessors;
        ValueWrapper            value;
    };

    bool isAccessor() const {
        return !(flags & JSPROP_INTERNAL_USE_BIT);
    }
    JS_PUBLIC_API(bool) getValue(JSContext* cx, JS::MutableHandleValue value) const;

    bool isSelfHosted() const {
        MOZ_ASSERT(isAccessor());

#ifdef DEBUG
        // Verify that our accessors match our JSPROP_GETTER flag.
        if (flags & JSPROP_GETTER)
            checkAccessorsAreSelfHosted();
        else
            checkAccessorsAreNative();
#endif
        return (flags & JSPROP_GETTER);
    }

    static_assert(sizeof(SelfHostedWrapper) == sizeof(JSNativeWrapper),
                  "JSPropertySpec::getter/setter must be compact");
    static_assert(offsetof(SelfHostedWrapper, funname) == offsetof(JSNativeWrapper, info),
                  "JS_SELF_HOSTED* macros below require that "
                  "SelfHostedWrapper::funname overlay "
                  "JSNativeWrapper::info");
private:
    void checkAccessorsAreNative() const {
        MOZ_ASSERT(accessors.getter.native.op);
        // We may not have a setter at all.  So all we can assert here, for the
        // native case is that if we have a jitinfo for the setter then we have
        // a setter op too.  This is good enough to make sure we don't have a
        // SelfHostedWrapper for the setter.
        MOZ_ASSERT_IF(accessors.setter.native.info, accessors.setter.native.op);
    }

    void checkAccessorsAreSelfHosted() const {
        MOZ_ASSERT(!accessors.getter.selfHosted.unused);
        MOZ_ASSERT(!accessors.setter.selfHosted.unused);
    }
};

namespace JS {
namespace detail {

/* NEVER DEFINED, DON'T USE.  For use by JS_CAST_NATIVE_TO only. */
inline int CheckIsNative(JSNative native);

/* NEVER DEFINED, DON'T USE.  For use by JS_CAST_STRING_TO only. */
template<size_t N>
inline int
CheckIsCharacterLiteral(const char (&arr)[N]);

/* NEVER DEFINED, DON'T USE.  For use by JS_CAST_INT32_TO only. */
inline int CheckIsInt32(int32_t value);

/* NEVER DEFINED, DON'T USE.  For use by JS_PROPERTYOP_GETTER only. */
inline int CheckIsGetterOp(JSGetterOp op);

/* NEVER DEFINED, DON'T USE.  For use by JS_PROPERTYOP_SETTER only. */
inline int CheckIsSetterOp(JSSetterOp op);

} // namespace detail
} // namespace JS

#define JS_CAST_NATIVE_TO(v, To) \
  (static_cast<void>(sizeof(JS::detail::CheckIsNative(v))), \
   reinterpret_cast<To>(v))

#define JS_CAST_STRING_TO(s, To) \
  (static_cast<void>(sizeof(JS::detail::CheckIsCharacterLiteral(s))), \
   reinterpret_cast<To>(s))

#define JS_CAST_INT32_TO(s, To) \
  (static_cast<void>(sizeof(JS::detail::CheckIsInt32(s))), \
   reinterpret_cast<To>(s))

#define JS_CHECK_ACCESSOR_FLAGS(flags) \
  (static_cast<mozilla::EnableIf<((flags) & ~(JSPROP_ENUMERATE | JSPROP_PERMANENT)) == 0>::Type>(0), \
   (flags))

#define JS_PROPERTYOP_GETTER(v) \
  (static_cast<void>(sizeof(JS::detail::CheckIsGetterOp(v))), \
   reinterpret_cast<JSNative>(v))

#define JS_PROPERTYOP_SETTER(v) \
  (static_cast<void>(sizeof(JS::detail::CheckIsSetterOp(v))), \
   reinterpret_cast<JSNative>(v))

#define JS_PS_ACCESSOR_SPEC(name, getter, setter, flags, extraFlags) \
    { name, uint8_t(JS_CHECK_ACCESSOR_FLAGS(flags) | extraFlags), \
      { {  getter, setter  } } }
#define JS_PS_VALUE_SPEC(name, value, flags) \
    { name, uint8_t(flags | JSPROP_INTERNAL_USE_BIT), \
      { { value, JSNATIVE_WRAPPER(nullptr) } } }

#define SELFHOSTED_WRAPPER(name) \
    { { nullptr, JS_CAST_STRING_TO(name, const JSJitInfo*) } }
#define STRINGVALUE_WRAPPER(value) \
    { { reinterpret_cast<JSNative>(JSVAL_TYPE_STRING), JS_CAST_STRING_TO(value, const JSJitInfo*) } }
#define INT32VALUE_WRAPPER(value) \
    { { reinterpret_cast<JSNative>(JSVAL_TYPE_INT32), JS_CAST_INT32_TO(value, const JSJitInfo*) } }

/*
 * JSPropertySpec uses JSNativeWrapper.  These macros encapsulate the definition
 * of JSNative-backed JSPropertySpecs, by defining the JSNativeWrappers for
 * them.
 */
#define JS_PSG(name, getter, flags) \
    JS_PS_ACCESSOR_SPEC(name, JSNATIVE_WRAPPER(getter), JSNATIVE_WRAPPER(nullptr), flags, \
                        0)
#define JS_PSGS(name, getter, setter, flags) \
    JS_PS_ACCESSOR_SPEC(name, JSNATIVE_WRAPPER(getter), JSNATIVE_WRAPPER(setter), flags, \
                        0)
#define JS_SYM_GET(symbol, getter, flags) \
    JS_PS_ACCESSOR_SPEC(reinterpret_cast<const char*>(uint32_t(::JS::SymbolCode::symbol) + 1), \
                        JSNATIVE_WRAPPER(getter), JSNATIVE_WRAPPER(nullptr), flags, 0)
#define JS_SELF_HOSTED_GET(name, getterName, flags) \
    JS_PS_ACCESSOR_SPEC(name, SELFHOSTED_WRAPPER(getterName), JSNATIVE_WRAPPER(nullptr), flags, \
                        JSPROP_GETTER)
#define JS_SELF_HOSTED_GETSET(name, getterName, setterName, flags) \
    JS_PS_ACCESSOR_SPEC(name, SELFHOSTED_WRAPPER(getterName), SELFHOSTED_WRAPPER(setterName), \
                         flags, JSPROP_GETTER | JSPROP_SETTER)
#define JS_SELF_HOSTED_SYM_GET(symbol, getterName, flags) \
    JS_PS_ACCESSOR_SPEC(reinterpret_cast<const char*>(uint32_t(::JS::SymbolCode::symbol) + 1), \
                         SELFHOSTED_WRAPPER(getterName), JSNATIVE_WRAPPER(nullptr), flags, \
                         JSPROP_GETTER)
#define JS_STRING_PS(name, string, flags) \
    JS_PS_VALUE_SPEC(name, STRINGVALUE_WRAPPER(string), flags)
#define JS_STRING_SYM_PS(symbol, string, flags) \
    JS_PS_VALUE_SPEC(reinterpret_cast<const char*>(uint32_t(::JS::SymbolCode::symbol) + 1), \
                     STRINGVALUE_WRAPPER(string), flags)
#define JS_INT32_PS(name, value, flags) \
    JS_PS_VALUE_SPEC(name, INT32VALUE_WRAPPER(value), flags)
#define JS_PS_END \
    JS_PS_ACCESSOR_SPEC(nullptr, JSNATIVE_WRAPPER(nullptr), JSNATIVE_WRAPPER(nullptr), 0, 0)

/**
 * To define a native function, set call to a JSNativeWrapper. To define a
 * self-hosted function, set selfHostedName to the name of a function
 * compiled during JSRuntime::initSelfHosting.
 */
struct JSFunctionSpec {
    const char*     name;
    JSNativeWrapper call;
    uint16_t        nargs;
    uint16_t        flags;
    const char*     selfHostedName;
};

/*
 * Terminating sentinel initializer to put at the end of a JSFunctionSpec array
 * that's passed to JS_DefineFunctions or JS_InitClass.
 */
#define JS_FS_END JS_FN(nullptr,nullptr,0,0)

/*
 * Initializer macros for a JSFunctionSpec array element. JS_FNINFO allows the
 * simple adding of JSJitInfos. JS_SELF_HOSTED_FN declares a self-hosted
 * function. JS_INLINABLE_FN allows specifying an InlinableNative enum value for
 * natives inlined or specialized by the JIT. Finally JS_FNSPEC has slots for
 * all the fields.
 *
 * The _SYM variants allow defining a function with a symbol key rather than a
 * string key. For example, use JS_SYM_FN(iterator, ...) to define an
 * @@iterator method.
 */
#define JS_FN(name,call,nargs,flags)                                          \
    JS_FNSPEC(name, call, nullptr, nargs, flags, nullptr)
#define JS_INLINABLE_FN(name,call,nargs,flags,native)                         \
    JS_FNSPEC(name, call, &js::jit::JitInfo_##native, nargs, flags, nullptr)
#define JS_SYM_FN(symbol,call,nargs,flags)                                    \
    JS_SYM_FNSPEC(symbol, call, nullptr, nargs, flags, nullptr)
#define JS_FNINFO(name,call,info,nargs,flags)                                 \
    JS_FNSPEC(name, call, info, nargs, flags, nullptr)
#define JS_SELF_HOSTED_FN(name,selfHostedName,nargs,flags)                    \
    JS_FNSPEC(name, nullptr, nullptr, nargs, flags, selfHostedName)
#define JS_SELF_HOSTED_SYM_FN(symbol, selfHostedName, nargs, flags)           \
    JS_SYM_FNSPEC(symbol, nullptr, nullptr, nargs, flags, selfHostedName)
#define JS_SYM_FNSPEC(symbol, call, info, nargs, flags, selfHostedName)       \
    JS_FNSPEC(reinterpret_cast<const char*>(                                 \
                  uint32_t(::JS::SymbolCode::symbol) + 1),                    \
              call, info, nargs, flags, selfHostedName)
#define JS_FNSPEC(name,call,info,nargs,flags,selfHostedName)                  \
    {name, {call, info}, nargs, flags, selfHostedName}

extern JS_PUBLIC_API(JSObject*)
JS_InitClass(JSContext* cx, JS::HandleObject obj, JS::HandleObject parent_proto,
             const JSClass* clasp, JSNative constructor, unsigned nargs,
             const JSPropertySpec* ps, const JSFunctionSpec* fs,
             const JSPropertySpec* static_ps, const JSFunctionSpec* static_fs);

/**
 * Set up ctor.prototype = proto and proto.constructor = ctor with the
 * right property flags.
 */
extern JS_PUBLIC_API(bool)
JS_LinkConstructorAndPrototype(JSContext* cx, JS::Handle<JSObject*> ctor,
                               JS::Handle<JSObject*> proto);

extern JS_PUBLIC_API(const JSClass*)
JS_GetClass(JSObject* obj);

extern JS_PUBLIC_API(bool)
JS_InstanceOf(JSContext* cx, JS::Handle<JSObject*> obj, const JSClass* clasp, JS::CallArgs* args);

extern JS_PUBLIC_API(bool)
JS_HasInstance(JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<JS::Value> v, bool* bp);

namespace JS {

// Implementation of
// http://www.ecma-international.org/ecma-262/6.0/#sec-ordinaryhasinstance.  If
// you're looking for the equivalent of "instanceof", you want JS_HasInstance,
// not this function.
extern JS_PUBLIC_API(bool)
OrdinaryHasInstance(JSContext* cx, HandleObject objArg, HandleValue v, bool* bp);

} // namespace JS

extern JS_PUBLIC_API(void*)
JS_GetPrivate(JSObject* obj);

extern JS_PUBLIC_API(void)
JS_SetPrivate(JSObject* obj, void* data);

extern JS_PUBLIC_API(void*)
JS_GetInstancePrivate(JSContext* cx, JS::Handle<JSObject*> obj, const JSClass* clasp,
                      JS::CallArgs* args);

extern JS_PUBLIC_API(JSObject*)
JS_GetConstructor(JSContext* cx, JS::Handle<JSObject*> proto);

namespace JS {

// Specification for which zone a newly created compartment should use.
enum ZoneSpecifier {
    // Use the single runtime wide system zone. The meaning of this zone is
    // left to the embedder.
    SystemZone,

    // Use a particular existing zone.
    ExistingZone,

    // Create a new zone with its own new zone group.
    NewZoneInNewZoneGroup,

    // Create a new zone in the same zone group as the system zone.
    NewZoneInSystemZoneGroup,

    // Create a new zone in the same zone group as another existing zone.
    NewZoneInExistingZoneGroup
};

/**
 * CompartmentCreationOptions specifies options relevant to creating a new
 * compartment, that are either immutable characteristics of that compartment
 * or that are discarded after the compartment has been created.
 *
 * Access to these options on an existing compartment is read-only: if you
 * need particular selections, make them before you create the compartment.
 */
class JS_PUBLIC_API(CompartmentCreationOptions)
{
  public:
    CompartmentCreationOptions()
      : addonId_(nullptr),
        traceGlobal_(nullptr),
        zoneSpec_(NewZoneInSystemZoneGroup),
        zonePointer_(nullptr),
        invisibleToDebugger_(false),
        mergeable_(false),
        preserveJitCode_(false),
        cloneSingletons_(false),
        sharedMemoryAndAtomics_(false),
        secureContext_(false),
        clampAndJitterTime_(true)
    {}

    // A null add-on ID means that the compartment is not associated with an
    // add-on.
    JSAddonId* addonIdOrNull() const { return addonId_; }
    CompartmentCreationOptions& setAddonId(JSAddonId* id) {
        addonId_ = id;
        return *this;
    }

    JSTraceOp getTrace() const {
        return traceGlobal_;
    }
    CompartmentCreationOptions& setTrace(JSTraceOp op) {
        traceGlobal_ = op;
        return *this;
    }

    void* zonePointer() const { return zonePointer_; }
    ZoneSpecifier zoneSpecifier() const { return zoneSpec_; }

    // Set the zone to use for the compartment. See ZoneSpecifier above.
    CompartmentCreationOptions& setSystemZone();
    CompartmentCreationOptions& setExistingZone(JSObject* obj);
    CompartmentCreationOptions& setNewZoneInNewZoneGroup();
    CompartmentCreationOptions& setNewZoneInSystemZoneGroup();
    CompartmentCreationOptions& setNewZoneInExistingZoneGroup(JSObject* obj);

    // Certain scopes (i.e. XBL compilation scopes) are implementation details
    // of the embedding, and references to them should never leak out to script.
    // This flag causes the this compartment to skip firing onNewGlobalObject
    // and makes addDebuggee a no-op for this global.
    bool invisibleToDebugger() const { return invisibleToDebugger_; }
    CompartmentCreationOptions& setInvisibleToDebugger(bool flag) {
        invisibleToDebugger_ = flag;
        return *this;
    }

    // Compartments used for off-thread compilation have their contents merged
    // into a target compartment when the compilation is finished. This is only
    // allowed if this flag is set. The invisibleToDebugger flag must also be
    // set for such compartments.
    bool mergeable() const { return mergeable_; }
    CompartmentCreationOptions& setMergeable(bool flag) {
        mergeable_ = flag;
        return *this;
    }

    // Determines whether this compartment should preserve JIT code on
    // non-shrinking GCs.
    bool preserveJitCode() const { return preserveJitCode_; }
    CompartmentCreationOptions& setPreserveJitCode(bool flag) {
        preserveJitCode_ = flag;
        return *this;
    }

    bool cloneSingletons() const { return cloneSingletons_; }
    CompartmentCreationOptions& setCloneSingletons(bool flag) {
        cloneSingletons_ = flag;
        return *this;
    }

    bool getSharedMemoryAndAtomicsEnabled() const;
    CompartmentCreationOptions& setSharedMemoryAndAtomicsEnabled(bool flag);

    // This flag doesn't affect JS engine behavior.  It is used by Gecko to
    // mark whether content windows and workers are "Secure Context"s. See
    // https://w3c.github.io/webappsec-secure-contexts/
    // https://bugzilla.mozilla.org/show_bug.cgi?id=1162772#c34
    bool secureContext() const { return secureContext_; }
    CompartmentCreationOptions& setSecureContext(bool flag) {
        secureContext_ = flag;
        return *this;
    }

    bool clampAndJitterTime() const { return clampAndJitterTime_; }
    CompartmentCreationOptions& setClampAndJitterTime(bool flag) {
        clampAndJitterTime_ = flag;
        return *this;
    }

  private:
    JSAddonId* addonId_;
    JSTraceOp traceGlobal_;
    ZoneSpecifier zoneSpec_;
    void* zonePointer_; // Per zoneSpec_, either a Zone, ZoneGroup, or null.
    bool invisibleToDebugger_;
    bool mergeable_;
    bool preserveJitCode_;
    bool cloneSingletons_;
    bool sharedMemoryAndAtomics_;
    bool secureContext_;
    bool clampAndJitterTime_;
};

/**
 * CompartmentBehaviors specifies behaviors of a compartment that can be
 * changed after the compartment's been created.
 */
class JS_PUBLIC_API(CompartmentBehaviors)
{
  public:
    class Override {
      public:
        Override() : mode_(Default) {}

        bool get(bool defaultValue) const {
            if (mode_ == Default)
                return defaultValue;
            return mode_ == ForceTrue;
        }

        void set(bool overrideValue) {
            mode_ = overrideValue ? ForceTrue : ForceFalse;
        }

        void reset() {
            mode_ = Default;
        }

      private:
        enum Mode {
            Default,
            ForceTrue,
            ForceFalse
        };

        Mode mode_;
    };

    CompartmentBehaviors()
      : discardSource_(false)
      , disableLazyParsing_(false)
      , singletonsAsTemplates_(true)
    {
    }

    // For certain globals, we know enough about the code that will run in them
    // that we can discard script source entirely.
    bool discardSource() const { return discardSource_; }
    CompartmentBehaviors& setDiscardSource(bool flag) {
        discardSource_ = flag;
        return *this;
    }

    bool disableLazyParsing() const { return disableLazyParsing_; }
    CompartmentBehaviors& setDisableLazyParsing(bool flag) {
        disableLazyParsing_ = flag;
        return *this;
    }

    bool extraWarnings(JSContext* cx) const;
    Override& extraWarningsOverride() { return extraWarningsOverride_; }

    bool getSingletonsAsTemplates() const {
        return singletonsAsTemplates_;
    }
    CompartmentBehaviors& setSingletonsAsValues() {
        singletonsAsTemplates_ = false;
        return *this;
    }

  private:
    bool discardSource_;
    bool disableLazyParsing_;
    Override extraWarningsOverride_;

    // To XDR singletons, we need to ensure that all singletons are all used as
    // templates, by making JSOP_OBJECT return a clone of the JSScript
    // singleton, instead of returning the value which is baked in the JSScript.
    bool singletonsAsTemplates_;
};

/**
 * CompartmentOptions specifies compartment characteristics: both those that
 * can't be changed on a compartment once it's been created
 * (CompartmentCreationOptions), and those that can be changed on an existing
 * compartment (CompartmentBehaviors).
 */
class JS_PUBLIC_API(CompartmentOptions)
{
  public:
    explicit CompartmentOptions()
      : creationOptions_(),
        behaviors_()
    {}

    CompartmentOptions(const CompartmentCreationOptions& compartmentCreation,
                       const CompartmentBehaviors& compartmentBehaviors)
      : creationOptions_(compartmentCreation),
        behaviors_(compartmentBehaviors)
    {}

    // CompartmentCreationOptions specify fundamental compartment
    // characteristics that must be specified when the compartment is created,
    // that can't be changed after the compartment is created.
    CompartmentCreationOptions& creationOptions() {
        return creationOptions_;
    }
    const CompartmentCreationOptions& creationOptions() const {
        return creationOptions_;
    }

    // CompartmentBehaviors specify compartment characteristics that can be
    // changed after the compartment is created.
    CompartmentBehaviors& behaviors() {
        return behaviors_;
    }
    const CompartmentBehaviors& behaviors() const {
        return behaviors_;
    }

  private:
    CompartmentCreationOptions creationOptions_;
    CompartmentBehaviors behaviors_;
};

JS_PUBLIC_API(const CompartmentCreationOptions&)
CompartmentCreationOptionsRef(JSCompartment* compartment);

JS_PUBLIC_API(const CompartmentCreationOptions&)
CompartmentCreationOptionsRef(JSObject* obj);

JS_PUBLIC_API(const CompartmentCreationOptions&)
CompartmentCreationOptionsRef(JSContext* cx);

JS_PUBLIC_API(CompartmentBehaviors&)
CompartmentBehaviorsRef(JSCompartment* compartment);

JS_PUBLIC_API(CompartmentBehaviors&)
CompartmentBehaviorsRef(JSObject* obj);

JS_PUBLIC_API(CompartmentBehaviors&)
CompartmentBehaviorsRef(JSContext* cx);

/**
 * During global creation, we fire notifications to callbacks registered
 * via the Debugger API. These callbacks are arbitrary script, and can touch
 * the global in arbitrary ways. When that happens, the global should not be
 * in a half-baked state. But this creates a problem for consumers that need
 * to set slots on the global to put it in a consistent state.
 *
 * This API provides a way for consumers to set slots atomically (immediately
 * after the global is created), before any debugger hooks are fired. It's
 * unfortunately on the clunky side, but that's the way the cookie crumbles.
 *
 * If callers have no additional state on the global to set up, they may pass
 * |FireOnNewGlobalHook| to JS_NewGlobalObject, which causes that function to
 * fire the hook as its final act before returning. Otherwise, callers should
 * pass |DontFireOnNewGlobalHook|, which means that they are responsible for
 * invoking JS_FireOnNewGlobalObject upon successfully creating the global. If
 * an error occurs and the operation aborts, callers should skip firing the
 * hook. But otherwise, callers must take care to fire the hook exactly once
 * before compiling any script in the global's scope (we have assertions in
 * place to enforce this). This lets us be sure that debugger clients never miss
 * breakpoints.
 */
enum OnNewGlobalHookOption {
    FireOnNewGlobalHook,
    DontFireOnNewGlobalHook
};

} /* namespace JS */

extern JS_PUBLIC_API(JSObject*)
JS_NewGlobalObject(JSContext* cx, const JSClass* clasp, JSPrincipals* principals,
                   JS::OnNewGlobalHookOption hookOption,
                   const JS::CompartmentOptions& options);
/**
 * Spidermonkey does not have a good way of keeping track of what compartments should be marked on
 * their own. We can mark the roots unconditionally, but marking GC things only relevant in live
 * compartments is hard. To mitigate this, we create a static trace hook, installed on each global
 * object, from which we can be sure the compartment is relevant, and mark it.
 *
 * It is still possible to specify custom trace hooks for global object classes. They can be
 * provided via the CompartmentOptions passed to JS_NewGlobalObject.
 */
extern JS_PUBLIC_API(void)
JS_GlobalObjectTraceHook(JSTracer* trc, JSObject* global);

extern JS_PUBLIC_API(void)
JS_FireOnNewGlobalObject(JSContext* cx, JS::HandleObject global);

extern JS_PUBLIC_API(JSObject*)
JS_NewObject(JSContext* cx, const JSClass* clasp);

extern JS_PUBLIC_API(bool)
JS_IsNative(JSObject* obj);

/**
 * Unlike JS_NewObject, JS_NewObjectWithGivenProto does not compute a default
 * proto. If proto is nullptr, the JS object will have `null` as [[Prototype]].
 */
extern JS_PUBLIC_API(JSObject*)
JS_NewObjectWithGivenProto(JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> proto);

/** Creates a new plain object, like `new Object()`, with Object.prototype as [[Prototype]]. */
extern JS_PUBLIC_API(JSObject*)
JS_NewPlainObject(JSContext* cx);

/**
 * Freeze obj, and all objects it refers to, recursively. This will not recurse
 * through non-extensible objects, on the assumption that those are already
 * deep-frozen.
 */
extern JS_PUBLIC_API(bool)
JS_DeepFreezeObject(JSContext* cx, JS::Handle<JSObject*> obj);

/**
 * Freezes an object; see ES5's Object.freeze(obj) method.
 */
extern JS_PUBLIC_API(bool)
JS_FreezeObject(JSContext* cx, JS::Handle<JSObject*> obj);


/*** Property descriptors ************************************************************************/

namespace JS {

struct JS_PUBLIC_API(PropertyDescriptor) {
    JSObject* obj;
    unsigned attrs;
    JSGetterOp getter;
    JSSetterOp setter;
    JS::Value value;

    PropertyDescriptor()
      : obj(nullptr), attrs(0), getter(nullptr), setter(nullptr), value(JS::UndefinedValue())
    {}

    static void trace(PropertyDescriptor* self, JSTracer* trc) { self->trace(trc); }
    void trace(JSTracer* trc);
};

} // namespace JS

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<JS::PropertyDescriptor, Wrapper>
{
    const JS::PropertyDescriptor& desc() const { return static_cast<const Wrapper*>(this)->get(); }

    bool has(unsigned bit) const {
        MOZ_ASSERT(bit != 0);
        MOZ_ASSERT((bit & (bit - 1)) == 0);  // only a single bit
        return (desc().attrs & bit) != 0;
    }

    bool hasAny(unsigned bits) const {
        return (desc().attrs & bits) != 0;
    }

    bool hasAll(unsigned bits) const {
        return (desc().attrs & bits) == bits;
    }

    // Non-API attributes bit used internally for arguments objects.
    enum { SHADOWABLE = JSPROP_INTERNAL_USE_BIT };

  public:
    // Descriptors with JSGetterOp/JSSetterOp are considered data
    // descriptors. It's complicated.
    bool isAccessorDescriptor() const { return hasAny(JSPROP_GETTER | JSPROP_SETTER); }
    bool isGenericDescriptor() const {
        return (desc().attrs&
                (JSPROP_GETTER | JSPROP_SETTER | JSPROP_IGNORE_READONLY | JSPROP_IGNORE_VALUE)) ==
               (JSPROP_IGNORE_READONLY | JSPROP_IGNORE_VALUE);
    }
    bool isDataDescriptor() const { return !isAccessorDescriptor() && !isGenericDescriptor(); }

    bool hasConfigurable() const { return !has(JSPROP_IGNORE_PERMANENT); }
    bool configurable() const { MOZ_ASSERT(hasConfigurable()); return !has(JSPROP_PERMANENT); }

    bool hasEnumerable() const { return !has(JSPROP_IGNORE_ENUMERATE); }
    bool enumerable() const { MOZ_ASSERT(hasEnumerable()); return has(JSPROP_ENUMERATE); }

    bool hasValue() const { return !isAccessorDescriptor() && !has(JSPROP_IGNORE_VALUE); }
    JS::HandleValue value() const {
        return JS::HandleValue::fromMarkedLocation(&desc().value);
    }

    bool hasWritable() const { return !isAccessorDescriptor() && !has(JSPROP_IGNORE_READONLY); }
    bool writable() const { MOZ_ASSERT(hasWritable()); return !has(JSPROP_READONLY); }

    bool hasGetterObject() const { return has(JSPROP_GETTER); }
    JS::HandleObject getterObject() const {
        MOZ_ASSERT(hasGetterObject());
        return JS::HandleObject::fromMarkedLocation(
                reinterpret_cast<JSObject* const*>(&desc().getter));
    }
    bool hasSetterObject() const { return has(JSPROP_SETTER); }
    JS::HandleObject setterObject() const {
        MOZ_ASSERT(hasSetterObject());
        return JS::HandleObject::fromMarkedLocation(
                reinterpret_cast<JSObject* const*>(&desc().setter));
    }

    bool hasGetterOrSetter() const { return desc().getter || desc().setter; }

    JS::HandleObject object() const {
        return JS::HandleObject::fromMarkedLocation(&desc().obj);
    }
    unsigned attributes() const { return desc().attrs; }
    JSGetterOp getter() const { return desc().getter; }
    JSSetterOp setter() const { return desc().setter; }

    void assertValid() const {
#ifdef DEBUG
        MOZ_ASSERT((attributes() & ~(JSPROP_ENUMERATE | JSPROP_IGNORE_ENUMERATE |
                                     JSPROP_PERMANENT | JSPROP_IGNORE_PERMANENT |
                                     JSPROP_READONLY | JSPROP_IGNORE_READONLY |
                                     JSPROP_IGNORE_VALUE |
                                     JSPROP_GETTER |
                                     JSPROP_SETTER |
                                     JSPROP_REDEFINE_NONCONFIGURABLE |
                                     JSPROP_RESOLVING |
                                     SHADOWABLE)) == 0);
        MOZ_ASSERT(!hasAll(JSPROP_IGNORE_ENUMERATE | JSPROP_ENUMERATE));
        MOZ_ASSERT(!hasAll(JSPROP_IGNORE_PERMANENT | JSPROP_PERMANENT));
        if (isAccessorDescriptor()) {
            MOZ_ASSERT(!has(JSPROP_READONLY));
            MOZ_ASSERT(!has(JSPROP_IGNORE_READONLY));
            MOZ_ASSERT(!has(JSPROP_IGNORE_VALUE));
            MOZ_ASSERT(!has(SHADOWABLE));
            MOZ_ASSERT(value().isUndefined());
            MOZ_ASSERT_IF(!has(JSPROP_GETTER), !getter());
            MOZ_ASSERT_IF(!has(JSPROP_SETTER), !setter());
        } else {
            MOZ_ASSERT(!hasAll(JSPROP_IGNORE_READONLY | JSPROP_READONLY));
            MOZ_ASSERT_IF(has(JSPROP_IGNORE_VALUE), value().isUndefined());
        }

        MOZ_ASSERT_IF(has(JSPROP_RESOLVING), !has(JSPROP_IGNORE_ENUMERATE));
        MOZ_ASSERT_IF(has(JSPROP_RESOLVING), !has(JSPROP_IGNORE_PERMANENT));
        MOZ_ASSERT_IF(has(JSPROP_RESOLVING), !has(JSPROP_IGNORE_READONLY));
        MOZ_ASSERT_IF(has(JSPROP_RESOLVING), !has(JSPROP_IGNORE_VALUE));
        MOZ_ASSERT_IF(has(JSPROP_RESOLVING), !has(JSPROP_REDEFINE_NONCONFIGURABLE));
#endif
    }

    void assertComplete() const {
#ifdef DEBUG
        assertValid();
        MOZ_ASSERT((attributes() & ~(JSPROP_ENUMERATE |
                                     JSPROP_PERMANENT |
                                     JSPROP_READONLY |
                                     JSPROP_GETTER |
                                     JSPROP_SETTER |
                                     JSPROP_REDEFINE_NONCONFIGURABLE |
                                     JSPROP_RESOLVING |
                                     SHADOWABLE)) == 0);
        MOZ_ASSERT_IF(isAccessorDescriptor(), has(JSPROP_GETTER) && has(JSPROP_SETTER));
#endif
    }

    void assertCompleteIfFound() const {
#ifdef DEBUG
        if (object())
            assertComplete();
#endif
    }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<JS::PropertyDescriptor, Wrapper>
    : public js::WrappedPtrOperations<JS::PropertyDescriptor, Wrapper>
{
    JS::PropertyDescriptor& desc() { return static_cast<Wrapper*>(this)->get(); }

  public:
    void clear() {
        object().set(nullptr);
        setAttributes(0);
        setGetter(nullptr);
        setSetter(nullptr);
        value().setUndefined();
    }

    void initFields(JS::HandleObject obj, JS::HandleValue v, unsigned attrs,
                    JSGetterOp getterOp, JSSetterOp setterOp) {
        object().set(obj);
        value().set(v);
        setAttributes(attrs);
        setGetter(getterOp);
        setSetter(setterOp);
    }

    void assign(JS::PropertyDescriptor& other) {
        object().set(other.obj);
        setAttributes(other.attrs);
        setGetter(other.getter);
        setSetter(other.setter);
        value().set(other.value);
    }

    void setDataDescriptor(JS::HandleValue v, unsigned attrs) {
        MOZ_ASSERT((attrs & ~(JSPROP_ENUMERATE |
                              JSPROP_PERMANENT |
                              JSPROP_READONLY |
                              JSPROP_IGNORE_ENUMERATE |
                              JSPROP_IGNORE_PERMANENT |
                              JSPROP_IGNORE_READONLY)) == 0);
        object().set(nullptr);
        setAttributes(attrs);
        setGetter(nullptr);
        setSetter(nullptr);
        value().set(v);
    }

    JS::MutableHandleObject object() {
        return JS::MutableHandleObject::fromMarkedLocation(&desc().obj);
    }
    unsigned& attributesRef() { return desc().attrs; }
    JSGetterOp& getter() { return desc().getter; }
    JSSetterOp& setter() { return desc().setter; }
    JS::MutableHandleValue value() {
        return JS::MutableHandleValue::fromMarkedLocation(&desc().value);
    }
    void setValue(JS::HandleValue v) {
        MOZ_ASSERT(!(desc().attrs & (JSPROP_GETTER | JSPROP_SETTER)));
        attributesRef() &= ~JSPROP_IGNORE_VALUE;
        value().set(v);
    }

    void setConfigurable(bool configurable) {
        setAttributes((desc().attrs & ~(JSPROP_IGNORE_PERMANENT | JSPROP_PERMANENT)) |
                      (configurable ? 0 : JSPROP_PERMANENT));
    }
    void setEnumerable(bool enumerable) {
        setAttributes((desc().attrs & ~(JSPROP_IGNORE_ENUMERATE | JSPROP_ENUMERATE)) |
                      (enumerable ? JSPROP_ENUMERATE : 0));
    }
    void setWritable(bool writable) {
        MOZ_ASSERT(!(desc().attrs & (JSPROP_GETTER | JSPROP_SETTER)));
        setAttributes((desc().attrs & ~(JSPROP_IGNORE_READONLY | JSPROP_READONLY)) |
                      (writable ? 0 : JSPROP_READONLY));
    }
    void setAttributes(unsigned attrs) { desc().attrs = attrs; }

    void setGetter(JSGetterOp op) {
        desc().getter = op;
    }
    void setSetter(JSSetterOp op) {
        desc().setter = op;
    }
    void setGetterObject(JSObject* obj) {
        desc().getter = reinterpret_cast<JSGetterOp>(obj);
        desc().attrs &= ~(JSPROP_IGNORE_VALUE | JSPROP_IGNORE_READONLY | JSPROP_READONLY);
        desc().attrs |= JSPROP_GETTER;
    }
    void setSetterObject(JSObject* obj) {
        desc().setter = reinterpret_cast<JSSetterOp>(obj);
        desc().attrs &= ~(JSPROP_IGNORE_VALUE | JSPROP_IGNORE_READONLY | JSPROP_READONLY);
        desc().attrs |= JSPROP_SETTER;
    }

    JS::MutableHandleObject getterObject() {
        MOZ_ASSERT(this->hasGetterObject());
        return JS::MutableHandleObject::fromMarkedLocation(
                reinterpret_cast<JSObject**>(&desc().getter));
    }
    JS::MutableHandleObject setterObject() {
        MOZ_ASSERT(this->hasSetterObject());
        return JS::MutableHandleObject::fromMarkedLocation(
                reinterpret_cast<JSObject**>(&desc().setter));
    }
};

} // namespace js

namespace JS {

extern JS_PUBLIC_API(bool)
ObjectToCompletePropertyDescriptor(JSContext* cx,
                                   JS::HandleObject obj,
                                   JS::HandleValue descriptor,
                                   JS::MutableHandle<PropertyDescriptor> desc);

/*
 * ES6 draft rev 32 (2015 Feb 2) 6.2.4.4 FromPropertyDescriptor(Desc).
 *
 * If desc.object() is null, then vp is set to undefined.
 */
extern JS_PUBLIC_API(bool)
FromPropertyDescriptor(JSContext* cx,
                       JS::Handle<JS::PropertyDescriptor> desc,
                       JS::MutableHandleValue vp);

} // namespace JS


/*** Standard internal methods ********************************************************************
 *
 * The functions below are the fundamental operations on objects.
 *
 * ES6 specifies 14 internal methods that define how objects behave.  The
 * standard is actually quite good on this topic, though you may have to read
 * it a few times. See ES6 sections 6.1.7.2 and 6.1.7.3.
 *
 * When 'obj' is an ordinary object, these functions have boring standard
 * behavior as specified by ES6 section 9.1; see the section about internal
 * methods in js/src/vm/NativeObject.h.
 *
 * Proxies override the behavior of internal methods. So when 'obj' is a proxy,
 * any one of the functions below could do just about anything. See
 * js/public/Proxy.h.
 */

/**
 * Get the prototype of obj, storing it in result.
 *
 * Implements: ES6 [[GetPrototypeOf]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_GetPrototype(JSContext* cx, JS::HandleObject obj, JS::MutableHandleObject result);

/**
 * If |obj| (underneath any functionally-transparent wrapper proxies) has as
 * its [[GetPrototypeOf]] trap the ordinary [[GetPrototypeOf]] behavior defined
 * for ordinary objects, set |*isOrdinary = true| and store |obj|'s prototype
 * in |result|.  Otherwise set |*isOrdinary = false|.  In case of error, both
 * outparams have unspecified value.
 */
extern JS_PUBLIC_API(bool)
JS_GetPrototypeIfOrdinary(JSContext* cx, JS::HandleObject obj, bool* isOrdinary,
                          JS::MutableHandleObject result);

/**
 * Change the prototype of obj.
 *
 * Implements: ES6 [[SetPrototypeOf]] internal method.
 *
 * In cases where ES6 [[SetPrototypeOf]] returns false without an exception,
 * JS_SetPrototype throws a TypeError and returns false.
 *
 * Performance warning: JS_SetPrototype is very bad for performance. It may
 * cause compiled jit-code to be invalidated. It also causes not only obj but
 * all other objects in the same "group" as obj to be permanently deoptimized.
 * It's better to create the object with the right prototype from the start.
 */
extern JS_PUBLIC_API(bool)
JS_SetPrototype(JSContext* cx, JS::HandleObject obj, JS::HandleObject proto);

/**
 * Determine whether obj is extensible. Extensible objects can have new
 * properties defined on them. Inextensible objects can't, and their
 * [[Prototype]] slot is fixed as well.
 *
 * Implements: ES6 [[IsExtensible]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_IsExtensible(JSContext* cx, JS::HandleObject obj, bool* extensible);

/**
 * Attempt to make |obj| non-extensible.
 *
 * Not all failures are treated as errors. See the comment on
 * JS::ObjectOpResult in js/public/Class.h.
 *
 * Implements: ES6 [[PreventExtensions]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_PreventExtensions(JSContext* cx, JS::HandleObject obj, JS::ObjectOpResult& result);

/**
 * Attempt to make the [[Prototype]] of |obj| immutable, such that any attempt
 * to modify it will fail.  If an error occurs during the attempt, return false
 * (with a pending exception set, depending upon the nature of the error).  If
 * no error occurs, return true with |*succeeded| set to indicate whether the
 * attempt successfully made the [[Prototype]] immutable.
 *
 * This is a nonstandard internal method.
 */
extern JS_PUBLIC_API(bool)
JS_SetImmutablePrototype(JSContext* cx, JS::HandleObject obj, bool* succeeded);

/**
 * Get a description of one of obj's own properties. If no such property exists
 * on obj, return true with desc.object() set to null.
 *
 * Implements: ES6 [[GetOwnProperty]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_GetOwnPropertyDescriptorById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                                JS::MutableHandle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API(bool)
JS_GetOwnPropertyDescriptor(JSContext* cx, JS::HandleObject obj, const char* name,
                            JS::MutableHandle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API(bool)
JS_GetOwnUCPropertyDescriptor(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                              JS::MutableHandle<JS::PropertyDescriptor> desc);

/**
 * Like JS_GetOwnPropertyDescriptorById, but also searches the prototype chain
 * if no own property is found directly on obj. The object on which the
 * property is found is returned in desc.object(). If the property is not found
 * on the prototype chain, this returns true with desc.object() set to null.
 */
extern JS_PUBLIC_API(bool)
JS_GetPropertyDescriptorById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                             JS::MutableHandle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API(bool)
JS_GetPropertyDescriptor(JSContext* cx, JS::HandleObject obj, const char* name,
                         JS::MutableHandle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API(bool)
JS_GetUCPropertyDescriptor(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                           JS::MutableHandle<JS::PropertyDescriptor> desc);

/**
 * Define a property on obj.
 *
 * This function uses JS::ObjectOpResult to indicate conditions that ES6
 * specifies as non-error failures. This is inconvenient at best, so use this
 * function only if you are implementing a proxy handler's defineProperty()
 * method. For all other purposes, use one of the many DefineProperty functions
 * below that throw an exception in all failure cases.
 *
 * Implements: ES6 [[DefineOwnProperty]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                      JS::Handle<JS::PropertyDescriptor> desc,
                      JS::ObjectOpResult& result);

/**
 * Define a property on obj, throwing a TypeError if the attempt fails.
 * This is the C++ equivalent of `Object.defineProperty(obj, id, desc)`.
 */
extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                      JS::Handle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue value,
                      unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JSNative getter,
                      JSNative setter, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleObject value,
                      unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleString value,
                      unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, int32_t value,
                      unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, uint32_t value,
                      unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, double value,
                      unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::HandleValue value,
                  unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, JSNative getter,
                  JSNative setter, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::HandleObject value,
                  unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::HandleString value,
                  unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, int32_t value,
                  unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, uint32_t value,
                  unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, double value,
                  unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JS::Handle<JS::PropertyDescriptor> desc,
                    JS::ObjectOpResult& result);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JS::Handle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JS::HandleValue value, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JSNative getter, JSNative setter, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JS::HandleObject value, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JS::HandleString value, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    int32_t value, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    uint32_t value, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    double value, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleValue value,
                 unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JSNative getter,
                 JSNative setter, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleObject value,
                 unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleString value,
                 unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, int32_t value,
                 unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, uint32_t value,
                 unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, double value,
                 unsigned attrs);

/**
 * Compute the expression `id in obj`.
 *
 * If obj has an own or inherited property obj[id], set *foundp = true and
 * return true. If not, set *foundp = false and return true. On error, return
 * false with an exception pending.
 *
 * Implements: ES6 [[Has]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_HasPropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* foundp);

extern JS_PUBLIC_API(bool)
JS_HasProperty(JSContext* cx, JS::HandleObject obj, const char* name, bool* foundp);

extern JS_PUBLIC_API(bool)
JS_HasUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                 bool* vp);

extern JS_PUBLIC_API(bool)
JS_HasElement(JSContext* cx, JS::HandleObject obj, uint32_t index, bool* foundp);

/**
 * Determine whether obj has an own property with the key `id`.
 *
 * Implements: ES6 7.3.11 HasOwnProperty(O, P).
 */
extern JS_PUBLIC_API(bool)
JS_HasOwnPropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* foundp);

extern JS_PUBLIC_API(bool)
JS_HasOwnProperty(JSContext* cx, JS::HandleObject obj, const char* name, bool* foundp);

/**
 * Get the value of the property `obj[id]`, or undefined if no such property
 * exists. This is the C++ equivalent of `vp = Reflect.get(obj, id, receiver)`.
 *
 * Most callers don't need the `receiver` argument. Consider using
 * JS_GetProperty instead. (But if you're implementing a proxy handler's set()
 * method, it's often correct to call this function and pass the receiver
 * through.)
 *
 * Implements: ES6 [[Get]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_ForwardGetPropertyTo(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                        JS::HandleValue receiver, JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_ForwardGetElementTo(JSContext* cx, JS::HandleObject obj, uint32_t index,
                       JS::HandleObject receiver, JS::MutableHandleValue vp);

/**
 * Get the value of the property `obj[id]`, or undefined if no such property
 * exists. The result is stored in vp.
 *
 * Implements: ES6 7.3.1 Get(O, P).
 */
extern JS_PUBLIC_API(bool)
JS_GetPropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                   JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_GetProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_GetUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                 JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_GetElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::MutableHandleValue vp);

/**
 * Perform the same property assignment as `Reflect.set(obj, id, v, receiver)`.
 *
 * This function has a `receiver` argument that most callers don't need.
 * Consider using JS_SetProperty instead.
 *
 * Implements: ES6 [[Set]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_ForwardSetPropertyTo(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue v,
                        JS::HandleValue receiver, JS::ObjectOpResult& result);

/**
 * Perform the assignment `obj[id] = v`.
 *
 * This function performs non-strict assignment, so if the property is
 * read-only, nothing happens and no error is thrown.
 */
extern JS_PUBLIC_API(bool)
JS_SetPropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue v);

extern JS_PUBLIC_API(bool)
JS_SetProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::HandleValue v);

extern JS_PUBLIC_API(bool)
JS_SetUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                 JS::HandleValue v);

extern JS_PUBLIC_API(bool)
JS_SetElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleValue v);

extern JS_PUBLIC_API(bool)
JS_SetElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleObject v);

extern JS_PUBLIC_API(bool)
JS_SetElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleString v);

extern JS_PUBLIC_API(bool)
JS_SetElement(JSContext* cx, JS::HandleObject obj, uint32_t index, int32_t v);

extern JS_PUBLIC_API(bool)
JS_SetElement(JSContext* cx, JS::HandleObject obj, uint32_t index, uint32_t v);

extern JS_PUBLIC_API(bool)
JS_SetElement(JSContext* cx, JS::HandleObject obj, uint32_t index, double v);

/**
 * Delete a property. This is the C++ equivalent of
 * `result = Reflect.deleteProperty(obj, id)`.
 *
 * This function has a `result` out parameter that most callers don't need.
 * Unless you can pass through an ObjectOpResult provided by your caller, it's
 * probably best to use the JS_DeletePropertyById signature with just 3
 * arguments.
 *
 * Implements: ES6 [[Delete]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_DeletePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                      JS::ObjectOpResult& result);

extern JS_PUBLIC_API(bool)
JS_DeleteProperty(JSContext* cx, JS::HandleObject obj, const char* name,
                  JS::ObjectOpResult& result);

extern JS_PUBLIC_API(bool)
JS_DeleteUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JS::ObjectOpResult& result);

extern JS_PUBLIC_API(bool)
JS_DeleteElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::ObjectOpResult& result);

/**
 * Delete a property, ignoring strict failures. This is the C++ equivalent of
 * the JS `delete obj[id]` in non-strict mode code.
 */
extern JS_PUBLIC_API(bool)
JS_DeletePropertyById(JSContext* cx, JS::HandleObject obj, jsid id);

extern JS_PUBLIC_API(bool)
JS_DeleteProperty(JSContext* cx, JS::HandleObject obj, const char* name);

extern JS_PUBLIC_API(bool)
JS_DeleteElement(JSContext* cx, JS::HandleObject obj, uint32_t index);

/**
 * Get an array of the non-symbol enumerable properties of obj.
 * This function is roughly equivalent to:
 *
 *     var result = [];
 *     for (key in obj)
 *         result.push(key);
 *     return result;
 *
 * This is the closest thing we currently have to the ES6 [[Enumerate]]
 * internal method.
 *
 * The array of ids returned by JS_Enumerate must be rooted to protect its
 * contents from garbage collection. Use JS::Rooted<JS::IdVector>.
 */
extern JS_PUBLIC_API(bool)
JS_Enumerate(JSContext* cx, JS::HandleObject obj, JS::MutableHandle<JS::IdVector> props);

/*
 * API for determining callability and constructability. [[Call]] and
 * [[Construct]] are internal methods that aren't present on all objects, so it
 * is useful to ask if they are there or not. The standard itself asks these
 * questions routinely.
 */
namespace JS {

/**
 * Return true if the given object is callable. In ES6 terms, an object is
 * callable if it has a [[Call]] internal method.
 *
 * Implements: ES6 7.2.3 IsCallable(argument).
 *
 * Functions are callable. A scripted proxy or wrapper is callable if its
 * target is callable. Most other objects aren't callable.
 */
extern JS_PUBLIC_API(bool)
IsCallable(JSObject* obj);

/**
 * Return true if the given object is a constructor. In ES6 terms, an object is
 * a constructor if it has a [[Construct]] internal method. The expression
 * `new obj()` throws a TypeError if obj is not a constructor.
 *
 * Implements: ES6 7.2.4 IsConstructor(argument).
 *
 * JS functions and classes are constructors. Arrow functions and most builtin
 * functions are not. A scripted proxy or wrapper is a constructor if its
 * target is a constructor.
 */
extern JS_PUBLIC_API(bool)
IsConstructor(JSObject* obj);

} /* namespace JS */

/**
 * Call a function, passing a this-value and arguments. This is the C++
 * equivalent of `rval = Reflect.apply(fun, obj, args)`.
 *
 * Implements: ES6 7.3.12 Call(F, V, [argumentsList]).
 * Use this function to invoke the [[Call]] internal method.
 */
extern JS_PUBLIC_API(bool)
JS_CallFunctionValue(JSContext* cx, JS::HandleObject obj, JS::HandleValue fval,
                     const JS::HandleValueArray& args, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
JS_CallFunction(JSContext* cx, JS::HandleObject obj, JS::HandleFunction fun,
                const JS::HandleValueArray& args, JS::MutableHandleValue rval);

/**
 * Perform the method call `rval = obj[name](args)`.
 */
extern JS_PUBLIC_API(bool)
JS_CallFunctionName(JSContext* cx, JS::HandleObject obj, const char* name,
                    const JS::HandleValueArray& args, JS::MutableHandleValue rval);

namespace JS {

static inline bool
Call(JSContext* cx, JS::HandleObject thisObj, JS::HandleFunction fun,
     const JS::HandleValueArray& args, MutableHandleValue rval)
{
    return !!JS_CallFunction(cx, thisObj, fun, args, rval);
}

static inline bool
Call(JSContext* cx, JS::HandleObject thisObj, JS::HandleValue fun, const JS::HandleValueArray& args,
     MutableHandleValue rval)
{
    return !!JS_CallFunctionValue(cx, thisObj, fun, args, rval);
}

static inline bool
Call(JSContext* cx, JS::HandleObject thisObj, const char* name, const JS::HandleValueArray& args,
     MutableHandleValue rval)
{
    return !!JS_CallFunctionName(cx, thisObj, name, args, rval);
}

extern JS_PUBLIC_API(bool)
Call(JSContext* cx, JS::HandleValue thisv, JS::HandleValue fun, const JS::HandleValueArray& args,
     MutableHandleValue rval);

static inline bool
Call(JSContext* cx, JS::HandleValue thisv, JS::HandleObject funObj, const JS::HandleValueArray& args,
     MutableHandleValue rval)
{
    MOZ_ASSERT(funObj);
    JS::RootedValue fun(cx, JS::ObjectValue(*funObj));
    return Call(cx, thisv, fun, args, rval);
}

/**
 * Invoke a constructor. This is the C++ equivalent of
 * `rval = Reflect.construct(fun, args, newTarget)`.
 *
 * JS::Construct() takes a `newTarget` argument that most callers don't need.
 * Consider using the four-argument Construct signature instead. (But if you're
 * implementing a subclass or a proxy handler's construct() method, this is the
 * right function to call.)
 *
 * Implements: ES6 7.3.13 Construct(F, [argumentsList], [newTarget]).
 * Use this function to invoke the [[Construct]] internal method.
 */
extern JS_PUBLIC_API(bool)
Construct(JSContext* cx, JS::HandleValue fun, HandleObject newTarget,
          const JS::HandleValueArray &args, MutableHandleObject objp);

/**
 * Invoke a constructor. This is the C++ equivalent of
 * `rval = new fun(...args)`.
 *
 * Implements: ES6 7.3.13 Construct(F, [argumentsList], [newTarget]), when
 * newTarget is omitted.
 */
extern JS_PUBLIC_API(bool)
Construct(JSContext* cx, JS::HandleValue fun, const JS::HandleValueArray& args,
          MutableHandleObject objp);

} /* namespace JS */

/**
 * Invoke a constructor, like the JS expression `new ctor(...args)`. Returns
 * the new object, or null on error.
 */
extern JS_PUBLIC_API(JSObject*)
JS_New(JSContext* cx, JS::HandleObject ctor, const JS::HandleValueArray& args);


/*** Other property-defining functions ***********************************************************/

extern JS_PUBLIC_API(JSObject*)
JS_DefineObject(JSContext* cx, JS::HandleObject obj, const char* name,
                const JSClass* clasp = nullptr, unsigned attrs = 0);

extern JS_PUBLIC_API(bool)
JS_DefineConstDoubles(JSContext* cx, JS::HandleObject obj, const JSConstDoubleSpec* cds);

extern JS_PUBLIC_API(bool)
JS_DefineConstIntegers(JSContext* cx, JS::HandleObject obj, const JSConstIntegerSpec* cis);

extern JS_PUBLIC_API(bool)
JS_DefineProperties(JSContext* cx, JS::HandleObject obj, const JSPropertySpec* ps);


/* * */

extern JS_PUBLIC_API(bool)
JS_AlreadyHasOwnPropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                             bool* foundp);

extern JS_PUBLIC_API(bool)
JS_AlreadyHasOwnProperty(JSContext* cx, JS::HandleObject obj, const char* name,
                         bool* foundp);

extern JS_PUBLIC_API(bool)
JS_AlreadyHasOwnUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name,
                           size_t namelen, bool* foundp);

extern JS_PUBLIC_API(bool)
JS_AlreadyHasOwnElement(JSContext* cx, JS::HandleObject obj, uint32_t index, bool* foundp);

extern JS_PUBLIC_API(JSObject*)
JS_NewArrayObject(JSContext* cx, const JS::HandleValueArray& contents);

extern JS_PUBLIC_API(JSObject*)
JS_NewArrayObject(JSContext* cx, size_t length);

/**
 * Returns true and sets |*isArray| indicating whether |value| is an Array
 * object or a wrapper around one, otherwise returns false on failure.
 *
 * This method returns true with |*isArray == false| when passed a proxy whose
 * target is an Array, or when passed a revoked proxy.
 */
extern JS_PUBLIC_API(bool)
JS_IsArrayObject(JSContext* cx, JS::HandleValue value, bool* isArray);

/**
 * Returns true and sets |*isArray| indicating whether |obj| is an Array object
 * or a wrapper around one, otherwise returns false on failure.
 *
 * This method returns true with |*isArray == false| when passed a proxy whose
 * target is an Array, or when passed a revoked proxy.
 */
extern JS_PUBLIC_API(bool)
JS_IsArrayObject(JSContext* cx, JS::HandleObject obj, bool* isArray);

extern JS_PUBLIC_API(bool)
JS_GetArrayLength(JSContext* cx, JS::Handle<JSObject*> obj, uint32_t* lengthp);

extern JS_PUBLIC_API(bool)
JS_SetArrayLength(JSContext* cx, JS::Handle<JSObject*> obj, uint32_t length);

namespace JS {

/**
 * Returns true and sets |*isMap| indicating whether |obj| is an Map object
 * or a wrapper around one, otherwise returns false on failure.
 *
 * This method returns true with |*isMap == false| when passed a proxy whose
 * target is an Map, or when passed a revoked proxy.
 */
extern JS_PUBLIC_API(bool)
IsMapObject(JSContext* cx, JS::HandleObject obj, bool* isMap);

/**
 * Returns true and sets |*isSet| indicating whether |obj| is an Set object
 * or a wrapper around one, otherwise returns false on failure.
 *
 * This method returns true with |*isSet == false| when passed a proxy whose
 * target is an Set, or when passed a revoked proxy.
 */
extern JS_PUBLIC_API(bool)
IsSetObject(JSContext* cx, JS::HandleObject obj, bool* isSet);

} /* namespace JS */

/**
 * Assign 'undefined' to all of the object's non-reserved slots. Note: this is
 * done for all slots, regardless of the associated property descriptor.
 */
JS_PUBLIC_API(void)
JS_SetAllNonReservedSlotsToUndefined(JSContext* cx, JSObject* objArg);

/**
 * Create a new array buffer with the given contents. It must be legal to pass
 * these contents to free(). On success, the ownership is transferred to the
 * new array buffer.
 */
extern JS_PUBLIC_API(JSObject*)
JS_NewArrayBufferWithContents(JSContext* cx, size_t nbytes, void* contents);

namespace JS {

using BufferContentsRefFunc = void (*)(void* contents, void* userData);

}  /* namespace JS */

/**
 * Create a new array buffer with the given contents. The ref and unref
 * functions should increment or decrement the reference count of the contents.
 * These functions allow array buffers to be used with embedder objects that
 * use reference counting, for example. The contents must not be modified by
 * any reference holders, internal or external.
 *
 * On success, the new array buffer takes a reference, and |ref(contents,
 * refUserData)| will be called. When the array buffer is ready to be disposed
 * of, |unref(contents, refUserData)| will be called to release the array
 * buffer's reference on the contents.
 *
 * The ref and unref functions must not call any JSAPI functions that could
 * cause a garbage collection.
 *
 * The ref function is optional. If it is nullptr, the caller is responsible
 * for incrementing the reference count before passing the contents to this
 * function. This also allows using non-reference-counted contents that must be
 * freed with some function other than free().
 *
 * The ref function may also be called in case the buffer is cloned in some
 * way. Currently this is not used, but it may be in the future. If the ref
 * function is nullptr, any operation where an extra reference would otherwise
 * be taken, will either copy the data, or throw an exception.
 */
extern JS_PUBLIC_API(JSObject*)
JS_NewExternalArrayBuffer(JSContext* cx, size_t nbytes, void* contents,
                          JS::BufferContentsRefFunc ref, JS::BufferContentsRefFunc unref,
                          void* refUserData = nullptr);

/**
 * Create a new array buffer with the given contents.  The array buffer does not take ownership of
 * contents, and JS_DetachArrayBuffer must be called before the contents are disposed of.
 */
extern JS_PUBLIC_API(JSObject*)
JS_NewArrayBufferWithExternalContents(JSContext* cx, size_t nbytes, void* contents);

/**
 * Steal the contents of the given array buffer. The array buffer has its
 * length set to 0 and its contents array cleared. The caller takes ownership
 * of the return value and must free it or transfer ownership via
 * JS_NewArrayBufferWithContents when done using it.
 */
extern JS_PUBLIC_API(void*)
JS_StealArrayBufferContents(JSContext* cx, JS::HandleObject obj);

/**
 * Returns a pointer to the ArrayBuffer |obj|'s data.  |obj| and its views will store and expose
 * the data in the returned pointer: assigning into the returned pointer will affect values exposed
 * by views of |obj| and vice versa.
 *
 * The caller must ultimately deallocate the returned pointer to avoid leaking.  The memory is
 * *not* garbage-collected with |obj|.  These steps must be followed to deallocate:
 *
 * 1. The ArrayBuffer |obj| must be detached using JS_DetachArrayBuffer.
 * 2. The returned pointer must be freed using JS_free.
 *
 * To perform step 1, callers *must* hold a reference to |obj| until they finish using the returned
 * pointer.  They *must not* attempt to let |obj| be GC'd, then JS_free the pointer.
 *
 * If |obj| isn't an ArrayBuffer, this function returns null and reports an error.
 */
extern JS_PUBLIC_API(void*)
JS_ExternalizeArrayBufferContents(JSContext* cx, JS::HandleObject obj);

/**
 * Create a new mapped array buffer with the given memory mapped contents. It
 * must be legal to free the contents pointer by unmapping it. On success,
 * ownership is transferred to the new mapped array buffer.
 */
extern JS_PUBLIC_API(JSObject*)
JS_NewMappedArrayBufferWithContents(JSContext* cx, size_t nbytes, void* contents);

/**
 * Create memory mapped array buffer contents.
 * Caller must take care of closing fd after calling this function.
 */
extern JS_PUBLIC_API(void*)
JS_CreateMappedArrayBufferContents(int fd, size_t offset, size_t length);

/**
 * Release the allocated resource of mapped array buffer contents before the
 * object is created.
 * If a new object has been created by JS_NewMappedArrayBufferWithContents()
 * with this content, then JS_DetachArrayBuffer() should be used instead to
 * release the resource used by the object.
 */
extern JS_PUBLIC_API(void)
JS_ReleaseMappedArrayBufferContents(void* contents, size_t length);

extern JS_PUBLIC_API(JS::Value)
JS_GetReservedSlot(JSObject* obj, uint32_t index);

extern JS_PUBLIC_API(void)
JS_SetReservedSlot(JSObject* obj, uint32_t index, const JS::Value& v);


/************************************************************************/

/*
 * Functions and scripts.
 */
extern JS_PUBLIC_API(JSFunction*)
JS_NewFunction(JSContext* cx, JSNative call, unsigned nargs, unsigned flags,
               const char* name);

namespace JS {

extern JS_PUBLIC_API(JSFunction*)
GetSelfHostedFunction(JSContext* cx, const char* selfHostedName, HandleId id,
                      unsigned nargs);

/**
 * Create a new function based on the given JSFunctionSpec, *fs.
 * id is the result of a successful call to
 * `PropertySpecNameToPermanentId(cx, fs->name, &id)`.
 *
 * Unlike JS_DefineFunctions, this does not treat fs as an array.
 * *fs must not be JS_FS_END.
 */
extern JS_PUBLIC_API(JSFunction*)
NewFunctionFromSpec(JSContext* cx, const JSFunctionSpec* fs, HandleId id);

} /* namespace JS */

extern JS_PUBLIC_API(JSObject*)
JS_GetFunctionObject(JSFunction* fun);

/**
 * Return the function's identifier as a JSString, or null if fun is unnamed.
 * The returned string lives as long as fun, so you don't need to root a saved
 * reference to it if fun is well-connected or rooted, and provided you bound
 * the use of the saved reference by fun's lifetime.
 */
extern JS_PUBLIC_API(JSString*)
JS_GetFunctionId(JSFunction* fun);

/**
 * Return a function's display name. This is the defined name if one was given
 * where the function was defined, or it could be an inferred name by the JS
 * engine in the case that the function was defined to be anonymous. This can
 * still return nullptr if a useful display name could not be inferred. The
 * same restrictions on rooting as those in JS_GetFunctionId apply.
 */
extern JS_PUBLIC_API(JSString*)
JS_GetFunctionDisplayId(JSFunction* fun);

/*
 * Return the arity (length) of fun.
 */
extern JS_PUBLIC_API(uint16_t)
JS_GetFunctionArity(JSFunction* fun);

/**
 * Infallible predicate to test whether obj is a function object (faster than
 * comparing obj's class name to "Function", but equivalent unless someone has
 * overwritten the "Function" identifier with a different constructor and then
 * created instances using that constructor that might be passed in as obj).
 */
extern JS_PUBLIC_API(bool)
JS_ObjectIsFunction(JSContext* cx, JSObject* obj);

extern JS_PUBLIC_API(bool)
JS_IsNativeFunction(JSObject* funobj, JSNative call);

/** Return whether the given function is a valid constructor. */
extern JS_PUBLIC_API(bool)
JS_IsConstructor(JSFunction* fun);

extern JS_PUBLIC_API(bool)
JS_DefineFunctions(JSContext* cx, JS::Handle<JSObject*> obj, const JSFunctionSpec* fs);

extern JS_PUBLIC_API(JSFunction*)
JS_DefineFunction(JSContext* cx, JS::Handle<JSObject*> obj, const char* name, JSNative call,
                  unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API(JSFunction*)
JS_DefineUCFunction(JSContext* cx, JS::Handle<JSObject*> obj,
                    const char16_t* name, size_t namelen, JSNative call,
                    unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API(JSFunction*)
JS_DefineFunctionById(JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id, JSNative call,
                      unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API(bool)
JS_IsFunctionBound(JSFunction* fun);

extern JS_PUBLIC_API(JSObject*)
JS_GetBoundFunctionTarget(JSFunction* fun);

namespace JS {

/**
 * Clone a top-level function into cx's global. This function will dynamically
 * fail if funobj was lexically nested inside some other function.
 */
extern JS_PUBLIC_API(JSObject*)
CloneFunctionObject(JSContext* cx, HandleObject funobj);

/**
 * As above, but providing an explicit scope chain.  scopeChain must not include
 * the global object on it; that's implicit.  It needs to contain the other
 * objects that should end up on the clone's scope chain.
 */
extern JS_PUBLIC_API(JSObject*)
CloneFunctionObject(JSContext* cx, HandleObject funobj, AutoObjectVector& scopeChain);

} // namespace JS

/**
 * Given a buffer, return false if the buffer might become a valid
 * javascript statement with the addition of more lines.  Otherwise return
 * true.  The intent is to support interactive compilation - accumulate
 * lines in a buffer until JS_BufferIsCompilableUnit is true, then pass it to
 * the compiler.
 */
extern JS_PUBLIC_API(bool)
JS_BufferIsCompilableUnit(JSContext* cx, JS::Handle<JSObject*> obj, const char* utf8,
                          size_t length);

/**
 * |script| will always be set. On failure, it will be set to nullptr.
 */
extern JS_PUBLIC_API(bool)
JS_CompileScript(JSContext* cx, const char* ascii, size_t length,
                 const JS::CompileOptions& options,
                 JS::MutableHandleScript script);

/**
 * |script| will always be set. On failure, it will be set to nullptr.
 */
extern JS_PUBLIC_API(bool)
JS_CompileUCScript(JSContext* cx, const char16_t* chars, size_t length,
                   const JS::CompileOptions& options,
                   JS::MutableHandleScript script);

extern JS_PUBLIC_API(JSObject*)
JS_GetGlobalFromScript(JSScript* script);

extern JS_PUBLIC_API(const char*)
JS_GetScriptFilename(JSScript* script);

extern JS_PUBLIC_API(unsigned)
JS_GetScriptBaseLineNumber(JSContext* cx, JSScript* script);

extern JS_PUBLIC_API(JSScript*)
JS_GetFunctionScript(JSContext* cx, JS::HandleFunction fun);

namespace JS {

/* Options for JavaScript compilation. */

/*
 * In the most common use case, a CompileOptions instance is allocated on the
 * stack, and holds non-owning references to non-POD option values: strings;
 * principals; objects; and so on. The code declaring the instance guarantees
 * that such option values will outlive the CompileOptions itself: objects are
 * otherwise rooted; principals have had their reference counts bumped; strings
 * will not be freed until the CompileOptions goes out of scope. In this
 * situation, CompileOptions only refers to things others own, so it can be
 * lightweight.
 *
 * In some cases, however, we need to hold compilation options with a
 * non-stack-like lifetime. For example, JS::CompileOffThread needs to save
 * compilation options where a worker thread can find them, and then return
 * immediately. The worker thread will come along at some later point, and use
 * the options.
 *
 * The compiler itself just needs to be able to access a collection of options;
 * it doesn't care who owns them, or what's keeping them alive. It does its own
 * addrefs/copies/tracing/etc.
 *
 * Furthermore, in some cases compile options are propagated from one entity to
 * another (e.g. from a script to a function defined in that script).  This
 * involves copying over some, but not all, of the options.
 *
 * So, we have a class hierarchy that reflects these four use cases:
 *
 * - TransitiveCompileOptions is the common base class, representing options
 *   that should get propagated from a script to functions defined in that
 *   script.  This is never instantiated directly.
 *
 * - ReadOnlyCompileOptions is the only subclass of TransitiveCompileOptions,
 *   representing a full set of compile options.  It can be used by code that
 *   simply needs to access options set elsewhere, like the compiler.  This,
 *   again, is never instantiated directly.
 *
 * - The usual CompileOptions class must be stack-allocated, and holds
 *   non-owning references to the filename, element, and so on. It's derived
 *   from ReadOnlyCompileOptions, so the compiler can use it.
 *
 * - OwningCompileOptions roots / copies / reference counts of all its values,
 *   and unroots / frees / releases them when it is destructed. It too is
 *   derived from ReadOnlyCompileOptions, so the compiler accepts it.
 */

enum class AsmJSOption : uint8_t { Enabled, Disabled, DisabledByDebugger };

/**
 * The common base class for the CompileOptions hierarchy.
 *
 * Use this in code that needs to propagate compile options from one compilation
 * unit to another.
 */
class JS_FRIEND_API(TransitiveCompileOptions)
{
  protected:
    // The Web Platform allows scripts to be loaded from arbitrary cross-origin
    // sources. This allows an attack by which a malicious website loads a
    // sensitive file (say, a bank statement) cross-origin (using the user's
    // cookies), and sniffs the generated syntax errors (via a window.onerror
    // handler) for juicy morsels of its contents.
    //
    // To counter this attack, HTML5 specifies that script errors should be
    // sanitized ("muted") when the script is not same-origin with the global
    // for which it is loaded. Callers should set this flag for cross-origin
    // scripts, and it will be propagated appropriately to child scripts and
    // passed back in JSErrorReports.
    bool mutedErrors_;
    const char* filename_;
    const char* introducerFilename_;
    const char16_t* sourceMapURL_;

    TransitiveCompileOptions()
      : mutedErrors_(false),
        filename_(nullptr),
        introducerFilename_(nullptr),
        sourceMapURL_(nullptr),
        utf8(false),
        selfHostingMode(false),
        canLazilyParse(true),
        strictOption(false),
        extraWarningsOption(false),
        expressionClosuresOption(false),
        werrorOption(false),
        asmJSOption(AsmJSOption::Disabled),
        throwOnAsmJSValidationFailureOption(false),
        forceAsync(false),
        sourceIsLazy(false),
        allowHTMLComments(true),
        isProbablySystemOrAddonCode(false),
        hideScriptFromDebugger(false),
        introductionType(nullptr),
        introductionLineno(0),
        introductionOffset(0),
        hasIntroductionInfo(false)
    { }

    // Set all POD options (those not requiring reference counts, copies,
    // rooting, or other hand-holding) to their values in |rhs|.
    void copyPODTransitiveOptions(const TransitiveCompileOptions& rhs);

  public:
    // Read-only accessors for non-POD options. The proper way to set these
    // depends on the derived type.
    bool mutedErrors() const { return mutedErrors_; }
    const char* filename() const { return filename_; }
    const char* introducerFilename() const { return introducerFilename_; }
    const char16_t* sourceMapURL() const { return sourceMapURL_; }
    virtual JSObject* element() const = 0;
    virtual JSString* elementAttributeName() const = 0;
    virtual JSScript* introductionScript() const = 0;

    // POD options.
    bool utf8;
    bool selfHostingMode;
    bool canLazilyParse;
    bool strictOption;
    bool extraWarningsOption;
    bool expressionClosuresOption;
    bool werrorOption;
    AsmJSOption asmJSOption;
    bool throwOnAsmJSValidationFailureOption;
    bool forceAsync;
    bool sourceIsLazy;
    bool allowHTMLComments;
    bool isProbablySystemOrAddonCode;
    bool hideScriptFromDebugger;

    // |introductionType| is a statically allocated C string:
    // one of "eval", "Function", or "GeneratorFunction".
    const char* introductionType;
    unsigned introductionLineno;
    uint32_t introductionOffset;
    bool hasIntroductionInfo;

  private:
    void operator=(const TransitiveCompileOptions&) = delete;
};

/**
 * The class representing a full set of compile options.
 *
 * Use this in code that only needs to access compilation options created
 * elsewhere, like the compiler. Don't instantiate this class (the constructor
 * is protected anyway); instead, create instances only of the derived classes:
 * CompileOptions and OwningCompileOptions.
 */
class JS_FRIEND_API(ReadOnlyCompileOptions) : public TransitiveCompileOptions
{
    friend class CompileOptions;

  protected:
    ReadOnlyCompileOptions()
      : TransitiveCompileOptions(),
        lineno(1),
        column(0),
        scriptSourceOffset(0),
        isRunOnce(false),
        nonSyntacticScope(false),
        noScriptRval(false)
    { }

    // Set all POD options (those not requiring reference counts, copies,
    // rooting, or other hand-holding) to their values in |rhs|.
    void copyPODOptions(const ReadOnlyCompileOptions& rhs);

  public:
    // Read-only accessors for non-POD options. The proper way to set these
    // depends on the derived type.
    bool mutedErrors() const { return mutedErrors_; }
    const char* filename() const { return filename_; }
    const char* introducerFilename() const { return introducerFilename_; }
    const char16_t* sourceMapURL() const { return sourceMapURL_; }
    virtual JSObject* element() const override = 0;
    virtual JSString* elementAttributeName() const override = 0;
    virtual JSScript* introductionScript() const override = 0;

    // POD options.
    unsigned lineno;
    unsigned column;
    // The offset within the ScriptSource's full uncompressed text of the first
    // character we're presenting for compilation with this CompileOptions.
    //
    // When we compile a LazyScript, we pass the compiler only the substring of
    // the source the lazy function occupies. With chunked decompression, we
    // may not even have the complete uncompressed source present in memory. But
    // parse node positions are offsets within the ScriptSource's full text,
    // and LazyScripts indicate their substring of the full source by its
    // starting and ending offsets within the full text. This
    // scriptSourceOffset field lets the frontend convert between these
    // offsets and offsets within the substring presented for compilation.
    unsigned scriptSourceOffset;
    // isRunOnce only applies to non-function scripts.
    bool isRunOnce;
    bool nonSyntacticScope;
    bool noScriptRval;

  private:
    void operator=(const ReadOnlyCompileOptions&) = delete;
};

/**
 * Compilation options, with dynamic lifetime. An instance of this type
 * makes a copy of / holds / roots all dynamically allocated resources
 * (principals; elements; strings) that it refers to. Its destructor frees
 * / drops / unroots them. This is heavier than CompileOptions, below, but
 * unlike CompileOptions, it can outlive any given stack frame.
 *
 * Note that this *roots* any JS values it refers to - they're live
 * unconditionally. Thus, instances of this type can't be owned, directly
 * or indirectly, by a JavaScript object: if any value that this roots ever
 * comes to refer to the object that owns this, then the whole cycle, and
 * anything else it entrains, will never be freed.
 */
class JS_FRIEND_API(OwningCompileOptions) : public ReadOnlyCompileOptions
{
    PersistentRootedObject elementRoot;
    PersistentRootedString elementAttributeNameRoot;
    PersistentRootedScript introductionScriptRoot;

  public:
    // A minimal constructor, for use with OwningCompileOptions::copy.
    explicit OwningCompileOptions(JSContext* cx);
    ~OwningCompileOptions();

    JSObject* element() const override { return elementRoot; }
    JSString* elementAttributeName() const override { return elementAttributeNameRoot; }
    JSScript* introductionScript() const override { return introductionScriptRoot; }

    // Set this to a copy of |rhs|. Return false on OOM.
    bool copy(JSContext* cx, const ReadOnlyCompileOptions& rhs);

    /* These setters make copies of their string arguments, and are fallible. */
    bool setFile(JSContext* cx, const char* f);
    bool setFileAndLine(JSContext* cx, const char* f, unsigned l);
    bool setSourceMapURL(JSContext* cx, const char16_t* s);
    bool setIntroducerFilename(JSContext* cx, const char* s);

    /* These setters are infallible, and can be chained. */
    OwningCompileOptions& setLine(unsigned l)             { lineno = l; return *this; }
    OwningCompileOptions& setElement(JSObject* e) {
        elementRoot = e;
        return *this;
    }
    OwningCompileOptions& setElementAttributeName(JSString* p) {
        elementAttributeNameRoot = p;
        return *this;
    }
    OwningCompileOptions& setIntroductionScript(JSScript* s) {
        introductionScriptRoot = s;
        return *this;
    }
    OwningCompileOptions& setMutedErrors(bool mute) {
        mutedErrors_ = mute;
        return *this;
    }
    OwningCompileOptions& setUTF8(bool u) { utf8 = u; return *this; }
    OwningCompileOptions& setColumn(unsigned c) { column = c; return *this; }
    OwningCompileOptions& setScriptSourceOffset(unsigned o) { scriptSourceOffset = o; return *this; }
    OwningCompileOptions& setIsRunOnce(bool once) { isRunOnce = once; return *this; }
    OwningCompileOptions& setNoScriptRval(bool nsr) { noScriptRval = nsr; return *this; }
    OwningCompileOptions& setSelfHostingMode(bool shm) { selfHostingMode = shm; return *this; }
    OwningCompileOptions& setCanLazilyParse(bool clp) { canLazilyParse = clp; return *this; }
    OwningCompileOptions& setSourceIsLazy(bool l) { sourceIsLazy = l; return *this; }
    OwningCompileOptions& setNonSyntacticScope(bool n) { nonSyntacticScope = n; return *this; }
    OwningCompileOptions& setIntroductionType(const char* t) { introductionType = t; return *this; }
    bool setIntroductionInfo(JSContext* cx, const char* introducerFn, const char* intro,
                             unsigned line, JSScript* script, uint32_t offset)
    {
        if (!setIntroducerFilename(cx, introducerFn))
            return false;
        introductionType = intro;
        introductionLineno = line;
        introductionScriptRoot = script;
        introductionOffset = offset;
        hasIntroductionInfo = true;
        return true;
    }

  private:
    void operator=(const CompileOptions& rhs) = delete;
};

/**
 * Compilation options stored on the stack. An instance of this type
 * simply holds references to dynamically allocated resources (element;
 * filename; source map URL) that are owned by something else. If you
 * create an instance of this type, it's up to you to guarantee that
 * everything you store in it will outlive it.
 */
class MOZ_STACK_CLASS JS_FRIEND_API(CompileOptions) final : public ReadOnlyCompileOptions
{
    RootedObject elementRoot;
    RootedString elementAttributeNameRoot;
    RootedScript introductionScriptRoot;

  public:
    explicit CompileOptions(JSContext* cx);
    CompileOptions(JSContext* cx, const ReadOnlyCompileOptions& rhs)
      : ReadOnlyCompileOptions(), elementRoot(cx), elementAttributeNameRoot(cx),
        introductionScriptRoot(cx)
    {
        copyPODOptions(rhs);

        filename_ = rhs.filename();
        introducerFilename_ = rhs.introducerFilename();
        sourceMapURL_ = rhs.sourceMapURL();
        elementRoot = rhs.element();
        elementAttributeNameRoot = rhs.elementAttributeName();
        introductionScriptRoot = rhs.introductionScript();
    }

    CompileOptions(JSContext* cx, const TransitiveCompileOptions& rhs)
      : ReadOnlyCompileOptions(), elementRoot(cx), elementAttributeNameRoot(cx),
        introductionScriptRoot(cx)
    {
        copyPODTransitiveOptions(rhs);

        filename_ = rhs.filename();
        introducerFilename_ = rhs.introducerFilename();
        sourceMapURL_ = rhs.sourceMapURL();
        elementRoot = rhs.element();
        elementAttributeNameRoot = rhs.elementAttributeName();
        introductionScriptRoot = rhs.introductionScript();
    }

    JSObject* element() const override { return elementRoot; }
    JSString* elementAttributeName() const override { return elementAttributeNameRoot; }
    JSScript* introductionScript() const override { return introductionScriptRoot; }

    CompileOptions& setFile(const char* f) { filename_ = f; return *this; }
    CompileOptions& setLine(unsigned l) { lineno = l; return *this; }
    CompileOptions& setFileAndLine(const char* f, unsigned l) {
        filename_ = f; lineno = l; return *this;
    }
    CompileOptions& setSourceMapURL(const char16_t* s) { sourceMapURL_ = s; return *this; }
    CompileOptions& setElement(JSObject* e)          { elementRoot = e; return *this; }
    CompileOptions& setElementAttributeName(JSString* p) {
        elementAttributeNameRoot = p;
        return *this;
    }
    CompileOptions& setIntroductionScript(JSScript* s) {
        introductionScriptRoot = s;
        return *this;
    }
    CompileOptions& setMutedErrors(bool mute) {
        mutedErrors_ = mute;
        return *this;
    }
    CompileOptions& setUTF8(bool u) { utf8 = u; return *this; }
    CompileOptions& setColumn(unsigned c) { column = c; return *this; }
    CompileOptions& setScriptSourceOffset(unsigned o) { scriptSourceOffset = o; return *this; }
    CompileOptions& setIsRunOnce(bool once) { isRunOnce = once; return *this; }
    CompileOptions& setNoScriptRval(bool nsr) { noScriptRval = nsr; return *this; }
    CompileOptions& setSelfHostingMode(bool shm) { selfHostingMode = shm; return *this; }
    CompileOptions& setCanLazilyParse(bool clp) { canLazilyParse = clp; return *this; }
    CompileOptions& setSourceIsLazy(bool l) { sourceIsLazy = l; return *this; }
    CompileOptions& setNonSyntacticScope(bool n) { nonSyntacticScope = n; return *this; }
    CompileOptions& setIntroductionType(const char* t) { introductionType = t; return *this; }
    CompileOptions& setIntroductionInfo(const char* introducerFn, const char* intro,
                                        unsigned line, JSScript* script, uint32_t offset)
    {
        introducerFilename_ = introducerFn;
        introductionType = intro;
        introductionLineno = line;
        introductionScriptRoot = script;
        introductionOffset = offset;
        hasIntroductionInfo = true;
        return *this;
    }
    CompileOptions& maybeMakeStrictMode(bool strict) {
        strictOption = strictOption || strict;
        return *this;
    }

  private:
    void operator=(const CompileOptions& rhs) = delete;
};

/**
 * |script| will always be set. On failure, it will be set to nullptr.
 */
extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
        SourceBufferHolder& srcBuf, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
        const char* bytes, size_t length, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
        const char16_t* chars, size_t length, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
        FILE* file, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
        const char* filename, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
CompileForNonSyntacticScope(JSContext* cx, const ReadOnlyCompileOptions& options,
                            SourceBufferHolder& srcBuf, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
CompileForNonSyntacticScope(JSContext* cx, const ReadOnlyCompileOptions& options,
                            const char* bytes, size_t length, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
CompileForNonSyntacticScope(JSContext* cx, const ReadOnlyCompileOptions& options,
                            const char16_t* chars, size_t length, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
CompileForNonSyntacticScope(JSContext* cx, const ReadOnlyCompileOptions& options,
                            FILE* file, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
CompileForNonSyntacticScope(JSContext* cx, const ReadOnlyCompileOptions& options,
                            const char* filename, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
CanCompileOffThread(JSContext* cx, const ReadOnlyCompileOptions& options, size_t length);

extern JS_PUBLIC_API(bool)
CanDecodeOffThread(JSContext* cx, const ReadOnlyCompileOptions& options, size_t length);

/*
 * Off thread compilation control flow.
 *
 * After successfully triggering an off thread compile of a script, the
 * callback will eventually be invoked with the specified data and a token
 * for the compilation. The callback will be invoked while off thread,
 * so must ensure that its operations are thread safe. Afterwards, one of the
 * following functions must be invoked on the runtime's active thread:
 *
 * - FinishOffThreadScript, to get the result script (or nullptr on failure).
 * - CancelOffThreadScript, to free the resources without creating a script.
 *
 * The characters passed in to CompileOffThread must remain live until the
 * callback is invoked, and the resulting script will be rooted until the call
 * to FinishOffThreadScript.
 */

extern JS_PUBLIC_API(bool)
CompileOffThread(JSContext* cx, const ReadOnlyCompileOptions& options,
                 const char16_t* chars, size_t length,
                 OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API(JSScript*)
FinishOffThreadScript(JSContext* cx, void* token);

extern JS_PUBLIC_API(void)
CancelOffThreadScript(JSContext* cx, void* token);

extern JS_PUBLIC_API(bool)
CompileOffThreadModule(JSContext* cx, const ReadOnlyCompileOptions& options,
                       const char16_t* chars, size_t length,
                       OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API(JSObject*)
FinishOffThreadModule(JSContext* cx, void* token);

extern JS_PUBLIC_API(void)
CancelOffThreadModule(JSContext* cx, void* token);

extern JS_PUBLIC_API(bool)
DecodeOffThreadScript(JSContext* cx, const ReadOnlyCompileOptions& options,
                      mozilla::Vector<uint8_t>& buffer /* TranscodeBuffer& */, size_t cursor,
                      OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API(bool)
DecodeOffThreadScript(JSContext* cx, const ReadOnlyCompileOptions& options,
                      const mozilla::Range<uint8_t>& range /* TranscodeRange& */,
                      OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API(JSScript*)
FinishOffThreadScriptDecoder(JSContext* cx, void* token);

extern JS_PUBLIC_API(void)
CancelOffThreadScriptDecoder(JSContext* cx, void* token);

extern JS_PUBLIC_API(bool)
DecodeMultiOffThreadScripts(JSContext* cx, const ReadOnlyCompileOptions& options,
                            mozilla::Vector<TranscodeSource>& sources,
                            OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API(bool)
FinishMultiOffThreadScriptsDecoder(JSContext* cx, void* token, JS::MutableHandle<JS::ScriptVector> scripts);

extern JS_PUBLIC_API(void)
CancelMultiOffThreadScriptsDecoder(JSContext* cx, void* token);

/**
 * Compile a function with envChain plus the global as its scope chain.
 * envChain must contain objects in the current compartment of cx.  The actual
 * scope chain used for the function will consist of With wrappers for those
 * objects, followed by the current global of the compartment cx is in.  This
 * global must not be explicitly included in the scope chain.
 */
extern JS_PUBLIC_API(bool)
CompileFunction(JSContext* cx, AutoObjectVector& envChain,
                const ReadOnlyCompileOptions& options,
                const char* name, unsigned nargs, const char* const* argnames,
                const char16_t* chars, size_t length, JS::MutableHandleFunction fun);

/**
 * Same as above, but taking a SourceBufferHolder for the function body.
 */
extern JS_PUBLIC_API(bool)
CompileFunction(JSContext* cx, AutoObjectVector& envChain,
                const ReadOnlyCompileOptions& options,
                const char* name, unsigned nargs, const char* const* argnames,
                SourceBufferHolder& srcBuf, JS::MutableHandleFunction fun);

/**
 * Same as above, but taking a const char * for the function body.
 */
extern JS_PUBLIC_API(bool)
CompileFunction(JSContext* cx, AutoObjectVector& envChain,
                const ReadOnlyCompileOptions& options,
                const char* name, unsigned nargs, const char* const* argnames,
                const char* bytes, size_t length, JS::MutableHandleFunction fun);

/*
 * Associate an element wrapper and attribute name with a previously compiled
 * script, for debugging purposes. Calling this function is optional, but should
 * be done before script execution if it is required.
 */
extern JS_PUBLIC_API(bool)
InitScriptSourceElement(JSContext* cx, HandleScript script,
                        HandleObject element, HandleString elementAttrName = nullptr);

/*
 * For a script compiled with the hideScriptFromDebugger option, expose the
 * script to the debugger by calling the debugger's onNewScript hook.
 */
extern JS_PUBLIC_API(void)
ExposeScriptToDebugger(JSContext* cx, HandleScript script);

} /* namespace JS */

extern JS_PUBLIC_API(JSString*)
JS_DecompileScript(JSContext* cx, JS::Handle<JSScript*> script);

extern JS_PUBLIC_API(JSString*)
JS_DecompileFunction(JSContext* cx, JS::Handle<JSFunction*> fun);


/*
 * NB: JS_ExecuteScript and the JS::Evaluate APIs come in two flavors: either
 * they use the global as the scope, or they take an AutoObjectVector of objects
 * to use as the scope chain.  In the former case, the global is also used as
 * the "this" keyword value and the variables object (ECMA parlance for where
 * 'var' and 'function' bind names) of the execution context for script.  In the
 * latter case, the first object in the provided list is used, unless the list
 * is empty, in which case the global is used.
 *
 * Why a runtime option?  The alternative is to add APIs duplicating those
 * for the other value of flags, and that doesn't seem worth the code bloat
 * cost.  Such new entry points would probably have less obvious names, too, so
 * would not tend to be used.  The ContextOptionsRef adjustment, OTOH, can be
 * more easily hacked into existing code that does not depend on the bug; such
 * code can continue to use the familiar JS::Evaluate, etc., entry points.
 */

/**
 * Evaluate a script in the scope of the current global of cx.
 */
extern JS_PUBLIC_API(bool)
JS_ExecuteScript(JSContext* cx, JS::HandleScript script, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
JS_ExecuteScript(JSContext* cx, JS::HandleScript script);

/**
 * As above, but providing an explicit scope chain.  envChain must not include
 * the global object on it; that's implicit.  It needs to contain the other
 * objects that should end up on the script's scope chain.
 */
extern JS_PUBLIC_API(bool)
JS_ExecuteScript(JSContext* cx, JS::AutoObjectVector& envChain,
                 JS::HandleScript script, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
JS_ExecuteScript(JSContext* cx, JS::AutoObjectVector& envChain, JS::HandleScript script);

namespace JS {

/**
 * Like the above, but handles a cross-compartment script. If the script is
 * cross-compartment, it is cloned into the current compartment before executing.
 */
extern JS_PUBLIC_API(bool)
CloneAndExecuteScript(JSContext* cx, JS::Handle<JSScript*> script,
                      JS::MutableHandleValue rval);

/**
 * Like CloneAndExecuteScript above, but allows executing under a non-syntactic
 * environment chain.
 */
extern JS_PUBLIC_API(bool)
CloneAndExecuteScript(JSContext* cx, JS::AutoObjectVector& envChain,
                      JS::Handle<JSScript*> script,
                      JS::MutableHandleValue rval);

} /* namespace JS */

namespace JS {

/**
 * Evaluate the given source buffer in the scope of the current global of cx.
 */
extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, const ReadOnlyCompileOptions& options,
         SourceBufferHolder& srcBuf, JS::MutableHandleValue rval);

/**
 * As above, but providing an explicit scope chain.  envChain must not include
 * the global object on it; that's implicit.  It needs to contain the other
 * objects that should end up on the script's scope chain.
 */
extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, AutoObjectVector& envChain, const ReadOnlyCompileOptions& options,
         SourceBufferHolder& srcBuf, JS::MutableHandleValue rval);

/**
 * Evaluate the given character buffer in the scope of the current global of cx.
 */
extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, const ReadOnlyCompileOptions& options,
         const char16_t* chars, size_t length, JS::MutableHandleValue rval);

/**
 * As above, but providing an explicit scope chain.  envChain must not include
 * the global object on it; that's implicit.  It needs to contain the other
 * objects that should end up on the script's scope chain.
 */
extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, AutoObjectVector& envChain, const ReadOnlyCompileOptions& options,
         const char16_t* chars, size_t length, JS::MutableHandleValue rval);

/**
 * Evaluate the given byte buffer in the scope of the current global of cx.
 */
extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, const ReadOnlyCompileOptions& options,
         const char* bytes, size_t length, JS::MutableHandleValue rval);

/**
 * Evaluate the given file in the scope of the current global of cx.
 */
extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, const ReadOnlyCompileOptions& options,
         const char* filename, JS::MutableHandleValue rval);

/**
 * Get the HostResolveImportedModule hook for a global.
 */
extern JS_PUBLIC_API(JSFunction*)
GetModuleResolveHook(JSContext* cx);

/**
 * Set the HostResolveImportedModule hook for a global to the given function.
 */
extern JS_PUBLIC_API(void)
SetModuleResolveHook(JSContext* cx, JS::HandleFunction func);

/**
 * Parse the given source buffer as a module in the scope of the current global
 * of cx and return a source text module record.
 */
extern JS_PUBLIC_API(bool)
CompileModule(JSContext* cx, const ReadOnlyCompileOptions& options,
              SourceBufferHolder& srcBuf, JS::MutableHandleObject moduleRecord);

/**
 * Set the [[HostDefined]] field of a source text module record to the given
 * value.
 */
extern JS_PUBLIC_API(void)
SetModuleHostDefinedField(JSObject* module, const JS::Value& value);

/**
 * Get the [[HostDefined]] field of a source text module record.
 */
extern JS_PUBLIC_API(JS::Value)
GetModuleHostDefinedField(JSObject* module);

/*
 * Perform the ModuleInstantiate operation on the given source text module
 * record.
 *
 * This transitively resolves all module dependencies (calling the
 * HostResolveImportedModule hook) and initializes the environment record for
 * the module.
 */
extern JS_PUBLIC_API(bool)
ModuleInstantiate(JSContext* cx, JS::HandleObject moduleRecord);

/*
 * Perform the ModuleEvaluate operation on the given source text module record.
 *
 * This does nothing if this module has already been evaluated. Otherwise, it
 * transitively evaluates all dependences of this module and then evaluates this
 * module.
 *
 * ModuleInstantiate must have completed prior to calling this.
 */
extern JS_PUBLIC_API(bool)
ModuleEvaluate(JSContext* cx, JS::HandleObject moduleRecord);

/*
 * Get a list of the module specifiers used by a source text module
 * record to request importation of modules.
 *
 * The result is a JavaScript array of object values.  To extract the individual
 * values use only JS_GetArrayLength and JS_GetElement with indices 0 to length
 * - 1.
 *
 * The element values are objects with the following properties:
 *  - moduleSpecifier: the module specifier string
 *  - lineNumber: the line number of the import in the source text
 *  - columnNumber: the column number of the import in the source text
 *
 * These property values can be extracted with GetRequestedModuleSpecifier() and
 * GetRequestedModuleSourcePos()
 */
extern JS_PUBLIC_API(JSObject*)
GetRequestedModules(JSContext* cx, JS::HandleObject moduleRecord);

extern JS_PUBLIC_API(JSString*)
GetRequestedModuleSpecifier(JSContext* cx, JS::HandleValue requestedModuleObject);

extern JS_PUBLIC_API(void)
GetRequestedModuleSourcePos(JSContext* cx, JS::HandleValue requestedModuleObject,
                            uint32_t* lineNumber, uint32_t* columnNumber);

extern JS_PUBLIC_API(JSScript*)
GetModuleScript(JS::HandleObject moduleRecord);

} /* namespace JS */

extern JS_PUBLIC_API(bool)
JS_CheckForInterrupt(JSContext* cx);

/*
 * These functions allow setting an interrupt callback that will be called
 * from the JS thread some time after any thread triggered the callback using
 * JS_RequestInterruptCallback(cx).
 *
 * To schedule the GC and for other activities the engine internally triggers
 * interrupt callbacks. The embedding should thus not rely on callbacks being
 * triggered through the external API only.
 *
 * Important note: Additional callbacks can occur inside the callback handler
 * if it re-enters the JS engine. The embedding must ensure that the callback
 * is disconnected before attempting such re-entry.
 */
extern JS_PUBLIC_API(bool)
JS_AddInterruptCallback(JSContext* cx, JSInterruptCallback callback);

extern JS_PUBLIC_API(bool)
JS_DisableInterruptCallback(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_ResetInterruptCallback(JSContext* cx, bool enable);

extern JS_PUBLIC_API(void)
JS_RequestInterruptCallback(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_RequestInterruptCallbackCanWait(JSContext* cx);

namespace JS {

/**
 * Sets the callback that's invoked whenever an incumbent global is required.
 *
 * SpiderMonkey doesn't itself have a notion of incumbent globals as defined
 * by the html spec, so we need the embedding to provide this.
 * See dom/base/ScriptSettings.h for details.
 */
extern JS_PUBLIC_API(void)
SetGetIncumbentGlobalCallback(JSContext* cx, JSGetIncumbentGlobalCallback callback);

/**
 * Sets the callback that's invoked whenever a Promise job should be enqeued.
 *
 * SpiderMonkey doesn't schedule Promise resolution jobs itself; instead,
 * using this function the embedding can provide a callback to do that
 * scheduling. The provided `callback` is invoked with the promise job,
 * the corresponding Promise's allocation stack, and the `data` pointer
 * passed here as arguments.
 */
extern JS_PUBLIC_API(void)
SetEnqueuePromiseJobCallback(JSContext* cx, JSEnqueuePromiseJobCallback callback,
                             void* data = nullptr);

/**
 * Sets the callback that's invoked whenever a Promise is rejected without
 * a rejection handler, and when a Promise that was previously rejected
 * without a handler gets a handler attached.
 */
extern JS_PUBLIC_API(void)
SetPromiseRejectionTrackerCallback(JSContext* cx, JSPromiseRejectionTrackerCallback callback,
                                   void* data = nullptr);

/**
 * Returns a new instance of the Promise builtin class in the current
 * compartment, with the right slot layout.
 *
 * The `executor` can be a `nullptr`. In that case, the only way to resolve or
 * reject the returned promise is via the `JS::ResolvePromise` and
 * `JS::RejectPromise` JSAPI functions.
 *
 * If a `proto` is passed, that gets set as the instance's [[Prototype]]
 * instead of the original value of `Promise.prototype`.
 */
extern JS_PUBLIC_API(JSObject*)
NewPromiseObject(JSContext* cx, JS::HandleObject executor, JS::HandleObject proto = nullptr);

/**
 * Returns true if the given object is an unwrapped PromiseObject, false
 * otherwise.
 */
extern JS_PUBLIC_API(bool)
IsPromiseObject(JS::HandleObject obj);

/**
 * Returns the current compartment's original Promise constructor.
 */
extern JS_PUBLIC_API(JSObject*)
GetPromiseConstructor(JSContext* cx);

/**
 * Returns the current compartment's original Promise.prototype.
 */
extern JS_PUBLIC_API(JSObject*)
GetPromisePrototype(JSContext* cx);

// Keep this in sync with the PROMISE_STATE defines in SelfHostingDefines.h.
enum class PromiseState {
    Pending,
    Fulfilled,
    Rejected
};

/**
 * Returns the given Promise's state as a JS::PromiseState enum value.
 *
 * Returns JS::PromiseState::Pending if the given object is a wrapper that
 * can't safely be unwrapped.
 */
extern JS_PUBLIC_API(PromiseState)
GetPromiseState(JS::HandleObject promise);

/**
 * Returns the given Promise's process-unique ID.
 */
JS_PUBLIC_API(uint64_t)
GetPromiseID(JS::HandleObject promise);

/**
 * Returns the given Promise's result: either the resolution value for
 * fulfilled promises, or the rejection reason for rejected ones.
 */
extern JS_PUBLIC_API(JS::Value)
GetPromiseResult(JS::HandleObject promise);

/**
 * Returns a js::SavedFrame linked list of the stack that lead to the given
 * Promise's allocation.
 */
extern JS_PUBLIC_API(JSObject*)
GetPromiseAllocationSite(JS::HandleObject promise);

extern JS_PUBLIC_API(JSObject*)
GetPromiseResolutionSite(JS::HandleObject promise);

#ifdef DEBUG
extern JS_PUBLIC_API(void)
DumpPromiseAllocationSite(JSContext* cx, JS::HandleObject promise);

extern JS_PUBLIC_API(void)
DumpPromiseResolutionSite(JSContext* cx, JS::HandleObject promise);
#endif

/**
 * Calls the current compartment's original Promise.resolve on the original
 * Promise constructor, with `resolutionValue` passed as an argument.
 */
extern JS_PUBLIC_API(JSObject*)
CallOriginalPromiseResolve(JSContext* cx, JS::HandleValue resolutionValue);

/**
 * Calls the current compartment's original Promise.reject on the original
 * Promise constructor, with `resolutionValue` passed as an argument.
 */
extern JS_PUBLIC_API(JSObject*)
CallOriginalPromiseReject(JSContext* cx, JS::HandleValue rejectionValue);

/**
 * Resolves the given Promise with the given `resolutionValue`.
 *
 * Calls the `resolve` function that was passed to the executor function when
 * the Promise was created.
 */
extern JS_PUBLIC_API(bool)
ResolvePromise(JSContext* cx, JS::HandleObject promiseObj, JS::HandleValue resolutionValue);

/**
 * Rejects the given `promise` with the given `rejectionValue`.
 *
 * Calls the `reject` function that was passed to the executor function when
 * the Promise was created.
 */
extern JS_PUBLIC_API(bool)
RejectPromise(JSContext* cx, JS::HandleObject promiseObj, JS::HandleValue rejectionValue);

/**
 * Calls the current compartment's original Promise.prototype.then on the
 * given `promise`, with `onResolve` and `onReject` passed as arguments.
 *
 * Asserts if the passed-in `promise` object isn't an unwrapped instance of
 * `Promise` or a subclass or `onResolve` and `onReject` aren't both either
 * `nullptr` or callable objects.
 */
extern JS_PUBLIC_API(JSObject*)
CallOriginalPromiseThen(JSContext* cx, JS::HandleObject promise,
                        JS::HandleObject onResolve, JS::HandleObject onReject);

/**
 * Unforgeable, optimized version of the JS builtin Promise.prototype.then.
 *
 * Takes a Promise instance and `onResolve`, `onReject` callables to enqueue
 * as reactions for that promise. In difference to Promise.prototype.then,
 * this doesn't create and return a new Promise instance.
 *
 * Asserts if the passed-in `promise` object isn't an unwrapped instance of
 * `Promise` or a subclass or `onResolve` and `onReject` aren't both callable
 * objects.
 */
extern JS_PUBLIC_API(bool)
AddPromiseReactions(JSContext* cx, JS::HandleObject promise,
                    JS::HandleObject onResolve, JS::HandleObject onReject);

/**
 * Unforgeable version of the JS builtin Promise.all.
 *
 * Takes an AutoObjectVector of Promise objects and returns a promise that's
 * resolved with an array of resolution values when all those promises have
 * been resolved, or rejected with the rejection value of the first rejected
 * promise.
 *
 * Asserts that all objects in the `promises` vector are, maybe wrapped,
 * instances of `Promise` or a subclass of `Promise`.
 */
extern JS_PUBLIC_API(JSObject*)
GetWaitForAllPromise(JSContext* cx, const JS::AutoObjectVector& promises);

/**
 * The Dispatchable interface allows the embedding to call SpiderMonkey
 * on a JSContext thread when requested via DispatchToEventLoopCallback.
 */
class JS_PUBLIC_API(Dispatchable)
{
  protected:
    // Dispatchables are created and destroyed by SpiderMonkey.
    Dispatchable() = default;
    virtual ~Dispatchable()  = default;

  public:
    // ShuttingDown indicates that SpiderMonkey should abort async tasks to
    // expedite shutdown.
    enum MaybeShuttingDown { NotShuttingDown, ShuttingDown };

    // Called by the embedding after DispatchToEventLoopCallback succeeds.
    virtual void run(JSContext* cx, MaybeShuttingDown maybeShuttingDown) = 0;
};

/**
 * DispatchToEventLoopCallback may be called from any thread, being passed the
 * same 'closure' passed to InitDispatchToEventLoop() and Dispatchable from the
 * same JSRuntime. If the embedding returns 'true', the embedding must call
 * Dispatchable::run() on an active JSContext thread for the same JSRuntime on
 * which 'closure' was registered. If DispatchToEventLoopCallback returns
 * 'false', SpiderMonkey will assume a shutdown of the JSRuntime is in progress.
 * This contract implies that, by the time the final JSContext is destroyed in
 * the JSRuntime, the embedding must have (1) run all Dispatchables for which
 * DispatchToEventLoopCallback returned true, (2) already started returning
 * false from calls to DispatchToEventLoopCallback.
 */

typedef bool
(*DispatchToEventLoopCallback)(void* closure, Dispatchable* dispatchable);

extern JS_PUBLIC_API(void)
InitDispatchToEventLoop(JSContext* cx, DispatchToEventLoopCallback callback, void* closure);

/**
 * The ConsumeStreamCallback is called from an active JSContext, passing a
 * StreamConsumer that wishes to consume the given host object as a stream of
 * bytes with the given MIME type. On failure, the embedding must report the
 * appropriate error on 'cx'. On success, the embedding must call
 * consumer->consumeChunk() repeatedly on any thread until exactly one of:
 *  - consumeChunk() returns false
 *  - the embedding calls consumer->streamClosed()
 * before JS_DestroyContext(cx) or JS::ShutdownAsyncTasks(cx) is called.
 *
 * Note: consumeChunk() and streamClosed() may be called synchronously by
 * ConsumeStreamCallback.
 */

class JS_PUBLIC_API(StreamConsumer)
{
  protected:
    // AsyncStreamConsumers are created and destroyed by SpiderMonkey.
    StreamConsumer() = default;
    virtual ~StreamConsumer() = default;

  public:
    // Called by the embedding as each chunk of bytes becomes available.
    // If this function returns 'false', the stream must drop all pointers to
    // this StreamConsumer.
    virtual bool consumeChunk(const uint8_t* begin, size_t length) = 0;

    // Called by the embedding when the stream is closed according to the
    // contract described above.
    enum CloseReason { EndOfFile, Error };
    virtual void streamClosed(CloseReason reason) = 0;

    // Provides optional stream attributes such as base or source mapping URLs.
    // Necessarily called before consumeChunk() or streamClosed(). The caller
    // retains ownership of the given strings.
    virtual void noteResponseURLs(const char* maybeUrl, const char* maybeSourceMapUrl) = 0;
};

enum class MimeType { Wasm };

typedef bool
(*ConsumeStreamCallback)(JSContext* cx, JS::HandleObject obj, MimeType mimeType,
                         StreamConsumer* consumer);

extern JS_PUBLIC_API(void)
InitConsumeStreamCallback(JSContext* cx, ConsumeStreamCallback callback);

/**
 * When a JSRuntime is destroyed it implicitly cancels all async tasks in
 * progress, releasing any roots held by the task. However, this is not soon
 * enough for cycle collection, which needs to have roots dropped earlier so
 * that the cycle collector can transitively remove roots for a future GC. For
 * these and other cases, the set of pending async tasks can be canceled
 * with this call earlier than JSRuntime destruction.
 */

extern JS_PUBLIC_API(void)
ShutdownAsyncTasks(JSContext* cx);

/**
 * Supply an alternative stack to incorporate into captured SavedFrame
 * backtraces as the imputed caller of asynchronous JavaScript calls, like async
 * function resumptions and DOM callbacks.
 *
 * When one async function awaits the result of another, it's natural to think
 * of that as a sort of function call: just as execution resumes from an
 * ordinary call expression when the callee returns, with the return value
 * providing the value of the call expression, execution resumes from an 'await'
 * expression after the awaited asynchronous function call returns, passing the
 * return value along.
 *
 * Call the two async functions in such a situation the 'awaiter' and the
 * 'awaitee'.
 *
 * As an async function, the awaitee contains 'await' expressions of its own.
 * Whenever it executes after its first 'await', there are never any actual
 * frames on the JavaScript stack under it; its awaiter is certainly not there.
 * An await expression's continuation is invoked as a promise callback, and
 * those are always called directly from the event loop in their own microtick.
 * (Ignore unusual cases like nested event loops.)
 *
 * But because await expressions bear such a strong resemblance to calls (and
 * deliberately so!), it would be unhelpful for stacks captured within the
 * awaitee to be empty; instead, they should present the awaiter as the caller.
 *
 * The AutoSetAsyncStackForNewCalls RAII class supplies a SavedFrame stack to
 * treat as the caller of any JavaScript invocations that occur within its
 * lifetime. Any SavedFrame stack captured during such an invocation uses the
 * SavedFrame passed to the constructor's 'stack' parameter as the 'asyncParent'
 * property of the SavedFrame for the invocation's oldest frame. Its 'parent'
 * property will be null, so stack-walking code can distinguish this
 * awaiter/awaitee transition from an ordinary caller/callee transition.
 *
 * The constructor's 'asyncCause' parameter supplies a string explaining what
 * sort of asynchronous call caused 'stack' to be spliced into the backtrace;
 * for example, async function resumptions use the string "async". This appears
 * as the 'asyncCause' property of the 'asyncParent' SavedFrame.
 *
 * Async callers are distinguished in the string form of a SavedFrame chain by
 * including the 'asyncCause' string in the frame. It appears before the
 * function name, with the two separated by a '*'.
 *
 * Note that, as each compartment has its own set of SavedFrames, the
 * 'asyncParent' may actually point to a copy of 'stack', rather than the exact
 * SavedFrame object passed.
 *
 * The youngest frame of 'stack' is not mutated to take the asyncCause string as
 * its 'asyncCause' property; SavedFrame objects are immutable. Rather, a fresh
 * clone of the frame is created with the needed 'asyncCause' property.
 *
 * The 'kind' argument specifies how aggressively 'stack' supplants any
 * JavaScript frames older than this AutoSetAsyncStackForNewCalls object. If
 * 'kind' is 'EXPLICIT', then all captured SavedFrame chains take on 'stack' as
 * their 'asyncParent' where the chain crosses this object's scope. If 'kind' is
 * 'IMPLICIT', then 'stack' is only included in captured chains if there are no
 * other JavaScript frames on the stack --- that is, only if the stack would
 * otherwise end at that point.
 *
 * AutoSetAsyncStackForNewCalls affects only SavedFrame chains; it does not
 * affect Debugger.Frame or js::FrameIter. SavedFrame chains are used for
 * Error.stack, allocation profiling, Promise debugging, and so on.
 *
 * See also `js/src/doc/SavedFrame/SavedFrame.md` for documentation on async
 * stack frames.
 */
class MOZ_STACK_CLASS JS_PUBLIC_API(AutoSetAsyncStackForNewCalls)
{
    JSContext* cx;
    RootedObject oldAsyncStack;
    const char* oldAsyncCause;
    bool oldAsyncCallIsExplicit;

  public:
    enum class AsyncCallKind {
        // The ordinary kind of call, where we may apply an async
        // parent if there is no ordinary parent.
        IMPLICIT,
        // An explicit async parent, e.g., callFunctionWithAsyncStack,
        // where we always want to override any ordinary parent.
        EXPLICIT
    };

    // The stack parameter cannot be null by design, because it would be
    // ambiguous whether that would clear any scheduled async stack and make the
    // normal stack reappear in the new call, or just keep the async stack
    // already scheduled for the new call, if any.
    //
    // asyncCause is owned by the caller and its lifetime must outlive the
    // lifetime of the AutoSetAsyncStackForNewCalls object. It is strongly
    // encouraged that asyncCause be a string constant or similar statically
    // allocated string.
    AutoSetAsyncStackForNewCalls(JSContext* cx, HandleObject stack,
                                 const char* asyncCause,
                                 AsyncCallKind kind = AsyncCallKind::IMPLICIT);
    ~AutoSetAsyncStackForNewCalls();
};

} // namespace JS

/************************************************************************/

/*
 * Strings.
 *
 * NB: JS_NewUCString takes ownership of bytes on success, avoiding a copy;
 * but on error (signified by null return), it leaves chars owned by the
 * caller. So the caller must free bytes in the error case, if it has no use
 * for them. In contrast, all the JS_New*StringCopy* functions do not take
 * ownership of the character memory passed to them -- they copy it.
 */
extern JS_PUBLIC_API(JSString*)
JS_NewStringCopyN(JSContext* cx, const char* s, size_t n);

extern JS_PUBLIC_API(JSString*)
JS_NewStringCopyZ(JSContext* cx, const char* s);

extern JS_PUBLIC_API(JSString*)
JS_NewStringCopyUTF8Z(JSContext* cx, const JS::ConstUTF8CharsZ s);

extern JS_PUBLIC_API(JSString*)
JS_NewStringCopyUTF8N(JSContext* cx, const JS::UTF8Chars s);

extern JS_PUBLIC_API(JSString*)
JS_AtomizeAndPinJSString(JSContext* cx, JS::HandleString str);

extern JS_PUBLIC_API(JSString*)
JS_AtomizeStringN(JSContext* cx, const char* s, size_t length);

extern JS_PUBLIC_API(JSString*)
JS_AtomizeString(JSContext* cx, const char* s);

extern JS_PUBLIC_API(JSString*)
JS_AtomizeAndPinStringN(JSContext* cx, const char* s, size_t length);

extern JS_PUBLIC_API(JSString*)
JS_AtomizeAndPinString(JSContext* cx, const char* s);

extern JS_PUBLIC_API(JSString*)
JS_NewLatin1String(JSContext* cx, JS::Latin1Char* chars, size_t length);

extern JS_PUBLIC_API(JSString*)
JS_NewUCString(JSContext* cx, char16_t* chars, size_t length);

extern JS_PUBLIC_API(JSString*)
JS_NewUCStringCopyN(JSContext* cx, const char16_t* s, size_t n);

extern JS_PUBLIC_API(JSString*)
JS_NewUCStringCopyZ(JSContext* cx, const char16_t* s);

extern JS_PUBLIC_API(JSString*)
JS_AtomizeUCStringN(JSContext* cx, const char16_t* s, size_t length);

extern JS_PUBLIC_API(JSString*)
JS_AtomizeUCString(JSContext* cx, const char16_t* s);

extern JS_PUBLIC_API(JSString*)
JS_AtomizeAndPinUCStringN(JSContext* cx, const char16_t* s, size_t length);

extern JS_PUBLIC_API(JSString*)
JS_AtomizeAndPinUCString(JSContext* cx, const char16_t* s);

extern JS_PUBLIC_API(bool)
JS_CompareStrings(JSContext* cx, JSString* str1, JSString* str2, int32_t* result);

extern JS_PUBLIC_API(bool)
JS_StringEqualsAscii(JSContext* cx, JSString* str, const char* asciiBytes, bool* match);

extern JS_PUBLIC_API(size_t)
JS_PutEscapedString(JSContext* cx, char* buffer, size_t size, JSString* str, char quote);

extern JS_PUBLIC_API(bool)
JS_FileEscapedString(FILE* fp, JSString* str, char quote);

/*
 * Extracting string characters and length.
 *
 * While getting the length of a string is infallible, getting the chars can
 * fail. As indicated by the lack of a JSContext parameter, there are two
 * special cases where getting the chars is infallible:
 *
 * The first case is for strings that have been atomized, e.g. directly by
 * JS_AtomizeAndPinString or implicitly because it is stored in a jsid.
 *
 * The second case is "flat" strings that have been explicitly prepared in a
 * fallible context by JS_FlattenString. To catch errors, a separate opaque
 * JSFlatString type is returned by JS_FlattenString and expected by
 * JS_GetFlatStringChars. Note, though, that this is purely a syntactic
 * distinction: the input and output of JS_FlattenString are the same actual
 * GC-thing. If a JSString is known to be flat, JS_ASSERT_STRING_IS_FLAT can be
 * used to make a debug-checked cast. Example:
 *
 *   // in a fallible context
 *   JSFlatString* fstr = JS_FlattenString(cx, str);
 *   if (!fstr)
 *     return false;
 *   MOZ_ASSERT(fstr == JS_ASSERT_STRING_IS_FLAT(str));
 *
 *   // in an infallible context, for the same 'str'
 *   AutoCheckCannotGC nogc;
 *   const char16_t* chars = JS_GetTwoByteFlatStringChars(nogc, fstr)
 *   MOZ_ASSERT(chars);
 *
 * Flat strings and interned strings are always null-terminated, so
 * JS_FlattenString can be used to get a null-terminated string.
 *
 * Additionally, string characters are stored as either Latin1Char (8-bit)
 * or char16_t (16-bit). Clients can use JS_StringHasLatin1Chars and can then
 * call either the Latin1* or TwoByte* functions. Some functions like
 * JS_CopyStringChars and JS_GetStringCharAt accept both Latin1 and TwoByte
 * strings.
 */

extern JS_PUBLIC_API(size_t)
JS_GetStringLength(JSString* str);

extern JS_PUBLIC_API(bool)
JS_StringIsFlat(JSString* str);

/** Returns true iff the string's characters are stored as Latin1. */
extern JS_PUBLIC_API(bool)
JS_StringHasLatin1Chars(JSString* str);

extern JS_PUBLIC_API(const JS::Latin1Char*)
JS_GetLatin1StringCharsAndLength(JSContext* cx, const JS::AutoRequireNoGC& nogc, JSString* str,
                                 size_t* length);

extern JS_PUBLIC_API(const char16_t*)
JS_GetTwoByteStringCharsAndLength(JSContext* cx, const JS::AutoRequireNoGC& nogc, JSString* str,
                                  size_t* length);

extern JS_PUBLIC_API(bool)
JS_GetStringCharAt(JSContext* cx, JSString* str, size_t index, char16_t* res);

extern JS_PUBLIC_API(char16_t)
JS_GetFlatStringCharAt(JSFlatString* str, size_t index);

extern JS_PUBLIC_API(const char16_t*)
JS_GetTwoByteExternalStringChars(JSString* str);

extern JS_PUBLIC_API(bool)
JS_CopyStringChars(JSContext* cx, mozilla::Range<char16_t> dest, JSString* str);

extern JS_PUBLIC_API(JSFlatString*)
JS_FlattenString(JSContext* cx, JSString* str);

extern JS_PUBLIC_API(const JS::Latin1Char*)
JS_GetLatin1FlatStringChars(const JS::AutoRequireNoGC& nogc, JSFlatString* str);

extern JS_PUBLIC_API(const char16_t*)
JS_GetTwoByteFlatStringChars(const JS::AutoRequireNoGC& nogc, JSFlatString* str);

static MOZ_ALWAYS_INLINE JSFlatString*
JSID_TO_FLAT_STRING(jsid id)
{
    MOZ_ASSERT(JSID_IS_STRING(id));
    return (JSFlatString*)(JSID_BITS(id));
}

static MOZ_ALWAYS_INLINE JSFlatString*
JS_ASSERT_STRING_IS_FLAT(JSString* str)
{
    MOZ_ASSERT(JS_StringIsFlat(str));
    return (JSFlatString*)str;
}

static MOZ_ALWAYS_INLINE JSString*
JS_FORGET_STRING_FLATNESS(JSFlatString* fstr)
{
    return (JSString*)fstr;
}

/*
 * Additional APIs that avoid fallibility when given a flat string.
 */

extern JS_PUBLIC_API(bool)
JS_FlatStringEqualsAscii(JSFlatString* str, const char* asciiBytes);

extern JS_PUBLIC_API(size_t)
JS_PutEscapedFlatString(char* buffer, size_t size, JSFlatString* str, char quote);

/**
 * Create a dependent string, i.e., a string that owns no character storage,
 * but that refers to a slice of another string's chars.  Dependent strings
 * are mutable by definition, so the thread safety comments above apply.
 */
extern JS_PUBLIC_API(JSString*)
JS_NewDependentString(JSContext* cx, JS::HandleString str, size_t start,
                      size_t length);

/**
 * Concatenate two strings, possibly resulting in a rope.
 * See above for thread safety comments.
 */
extern JS_PUBLIC_API(JSString*)
JS_ConcatStrings(JSContext* cx, JS::HandleString left, JS::HandleString right);

/**
 * For JS_DecodeBytes, set *dstlenp to the size of the destination buffer before
 * the call; on return, *dstlenp contains the number of characters actually
 * stored. To determine the necessary destination buffer size, make a sizing
 * call that passes nullptr for dst.
 *
 * On errors, the functions report the error. In that case, *dstlenp contains
 * the number of characters or bytes transferred so far.  If cx is nullptr, no
 * error is reported on failure, and the functions simply return false.
 *
 * NB: This function does not store an additional zero byte or char16_t after the
 * transcoded string.
 */
JS_PUBLIC_API(bool)
JS_DecodeBytes(JSContext* cx, const char* src, size_t srclen, char16_t* dst,
               size_t* dstlenp);

/**
 * A variation on JS_EncodeCharacters where a null terminated string is
 * returned that you are expected to call JS_free on when done.
 */
JS_PUBLIC_API(char*)
JS_EncodeString(JSContext* cx, JSString* str);

/**
 * Same behavior as JS_EncodeString(), but encode into UTF-8 string
 */
JS_PUBLIC_API(char*)
JS_EncodeStringToUTF8(JSContext* cx, JS::HandleString str);

/**
 * Get number of bytes in the string encoding (without accounting for a
 * terminating zero bytes. The function returns (size_t) -1 if the string
 * can not be encoded into bytes and reports an error using cx accordingly.
 */
JS_PUBLIC_API(size_t)
JS_GetStringEncodingLength(JSContext* cx, JSString* str);

/**
 * Encode string into a buffer. The function does not stores an additional
 * zero byte. The function returns (size_t) -1 if the string can not be
 * encoded into bytes with no error reported. Otherwise it returns the number
 * of bytes that are necessary to encode the string. If that exceeds the
 * length parameter, the string will be cut and only length bytes will be
 * written into the buffer.
 */
JS_PUBLIC_API(size_t)
JS_EncodeStringToBuffer(JSContext* cx, JSString* str, char* buffer, size_t length);

class MOZ_RAII JSAutoByteString
{
  public:
    JSAutoByteString(JSContext* cx, JSString* str
                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : mBytes(JS_EncodeString(cx, str))
    {
        MOZ_ASSERT(cx);
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    explicit JSAutoByteString(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM)
      : mBytes(nullptr)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    ~JSAutoByteString() {
        JS_free(nullptr, mBytes);
    }

    /* Take ownership of the given byte array. */
    void initBytes(JS::UniqueChars&& bytes) {
        MOZ_ASSERT(!mBytes);
        mBytes = bytes.release();
    }

    char* encodeLatin1(JSContext* cx, JSString* str) {
        MOZ_ASSERT(!mBytes);
        MOZ_ASSERT(cx);
        mBytes = JS_EncodeString(cx, str);
        return mBytes;
    }

    char* encodeUtf8(JSContext* cx, JS::HandleString str) {
        MOZ_ASSERT(!mBytes);
        MOZ_ASSERT(cx);
        mBytes = JS_EncodeStringToUTF8(cx, str);
        return mBytes;
    }

    void clear() {
        js_free(mBytes);
        mBytes = nullptr;
    }

    char* ptr() const {
        return mBytes;
    }

    bool operator!() const {
        return !mBytes;
    }

    size_t length() const {
        if (!mBytes)
            return 0;
        return strlen(mBytes);
    }

  private:
    char* mBytes;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    /* Copy and assignment are not supported. */
    JSAutoByteString(const JSAutoByteString& another);
    JSAutoByteString& operator=(const JSAutoByteString& another);
};

namespace JS {

extern JS_PUBLIC_API(JSAddonId*)
NewAddonId(JSContext* cx, JS::HandleString str);

extern JS_PUBLIC_API(JSString*)
StringOfAddonId(JSAddonId* id);

extern JS_PUBLIC_API(JSAddonId*)
AddonIdOfObject(JSObject* obj);

} // namespace JS

/************************************************************************/
/*
 * Symbols
 */

namespace JS {

/**
 * Create a new Symbol with the given description. This function never returns
 * a Symbol that is in the Runtime-wide symbol registry.
 *
 * If description is null, the new Symbol's [[Description]] attribute is
 * undefined.
 */
JS_PUBLIC_API(Symbol*)
NewSymbol(JSContext* cx, HandleString description);

/**
 * Symbol.for as specified in ES6.
 *
 * Get a Symbol with the description 'key' from the Runtime-wide symbol registry.
 * If there is not already a Symbol with that description in the registry, a new
 * Symbol is created and registered. 'key' must not be null.
 */
JS_PUBLIC_API(Symbol*)
GetSymbolFor(JSContext* cx, HandleString key);

/**
 * Get the [[Description]] attribute of the given symbol.
 *
 * This function is infallible. If it returns null, that means the symbol's
 * [[Description]] is undefined.
 */
JS_PUBLIC_API(JSString*)
GetSymbolDescription(HandleSymbol symbol);

/* Well-known symbols. */
#define JS_FOR_EACH_WELL_KNOWN_SYMBOL(macro) \
    macro(isConcatSpreadable) \
    macro(iterator) \
    macro(match) \
    macro(replace) \
    macro(search) \
    macro(species) \
    macro(hasInstance) \
    macro(split) \
    macro(toPrimitive) \
    macro(toStringTag) \
    macro(unscopables) \
    macro(asyncIterator)

enum class SymbolCode : uint32_t {
    // There is one SymbolCode for each well-known symbol.
#define JS_DEFINE_SYMBOL_ENUM(name) name,
    JS_FOR_EACH_WELL_KNOWN_SYMBOL(JS_DEFINE_SYMBOL_ENUM)  // SymbolCode::iterator, etc.
#undef JS_DEFINE_SYMBOL_ENUM
    Limit,
    InSymbolRegistry = 0xfffffffe,  // created by Symbol.for() or JS::GetSymbolFor()
    UniqueSymbol = 0xffffffff       // created by Symbol() or JS::NewSymbol()
};

/* For use in loops that iterate over the well-known symbols. */
const size_t WellKnownSymbolLimit = size_t(SymbolCode::Limit);

/**
 * Return the SymbolCode telling what sort of symbol `symbol` is.
 *
 * A symbol's SymbolCode never changes once it is created.
 */
JS_PUBLIC_API(SymbolCode)
GetSymbolCode(Handle<Symbol*> symbol);

/**
 * Get one of the well-known symbols defined by ES6. A single set of well-known
 * symbols is shared by all compartments in a JSRuntime.
 *
 * `which` must be in the range [0, WellKnownSymbolLimit).
 */
JS_PUBLIC_API(Symbol*)
GetWellKnownSymbol(JSContext* cx, SymbolCode which);

/**
 * Return true if the given JSPropertySpec::name or JSFunctionSpec::name value
 * is actually a symbol code and not a string. See JS_SYM_FN.
 */
inline bool
PropertySpecNameIsSymbol(const char* name)
{
    uintptr_t u = reinterpret_cast<uintptr_t>(name);
    return u != 0 && u - 1 < WellKnownSymbolLimit;
}

JS_PUBLIC_API(bool)
PropertySpecNameEqualsId(const char* name, HandleId id);

/**
 * Create a jsid that does not need to be marked for GC.
 *
 * 'name' is a JSPropertySpec::name or JSFunctionSpec::name value. The
 * resulting jsid, on success, is either an interned string or a well-known
 * symbol; either way it is immune to GC so there is no need to visit *idp
 * during GC marking.
 */
JS_PUBLIC_API(bool)
PropertySpecNameToPermanentId(JSContext* cx, const char* name, jsid* idp);

} /* namespace JS */

/************************************************************************/
/*
 * JSON functions
 */
typedef bool (* JSONWriteCallback)(const char16_t* buf, uint32_t len, void* data);

/**
 * JSON.stringify as specified by ES5.
 */
JS_PUBLIC_API(bool)
JS_Stringify(JSContext* cx, JS::MutableHandleValue value, JS::HandleObject replacer,
             JS::HandleValue space, JSONWriteCallback callback, void* data);

namespace JS {

/**
 * An API akin to JS_Stringify but with the goal of not having observable
 * side-effects when the stringification is performed.  This means it does not
 * allow a replacer or a custom space, and has the following constraints on its
 * input:
 *
 * 1) The input must be a plain object or array, not an abitrary value.
 * 2) Every value in the graph reached by the algorithm starting with this
 *    object must be one of the following: null, undefined, a string (NOT a
 *    string object!), a boolean, a finite number (i.e. no NaN or Infinity or
 *    -Infinity), a plain object with no accessor properties, or an Array with
 *    no holes.
 *
 * The actual behavior differs from JS_Stringify only in asserting the above and
 * NOT attempting to get the "toJSON" property from things, since that could
 * clearly have side-effects.
 */
JS_PUBLIC_API(bool)
ToJSONMaybeSafely(JSContext* cx, JS::HandleObject input,
                  JSONWriteCallback callback, void* data);

} /* namespace JS */

/**
 * JSON.parse as specified by ES5.
 */
JS_PUBLIC_API(bool)
JS_ParseJSON(JSContext* cx, const char16_t* chars, uint32_t len, JS::MutableHandleValue vp);

JS_PUBLIC_API(bool)
JS_ParseJSON(JSContext* cx, JS::HandleString str, JS::MutableHandleValue vp);

JS_PUBLIC_API(bool)
JS_ParseJSONWithReviver(JSContext* cx, const char16_t* chars, uint32_t len, JS::HandleValue reviver,
                        JS::MutableHandleValue vp);

JS_PUBLIC_API(bool)
JS_ParseJSONWithReviver(JSContext* cx, JS::HandleString str, JS::HandleValue reviver,
                        JS::MutableHandleValue vp);

/************************************************************************/

/**
 * The default locale for the ECMAScript Internationalization API
 * (Intl.Collator, Intl.NumberFormat, Intl.DateTimeFormat).
 * Note that the Internationalization API encourages clients to
 * specify their own locales.
 * The locale string remains owned by the caller.
 */
extern JS_PUBLIC_API(bool)
JS_SetDefaultLocale(JSRuntime* rt, const char* locale);

/**
 * Look up the default locale for the ECMAScript Internationalization API.
 * NB: The locale information is retrieved from cx's runtime.
 */
extern JS_PUBLIC_API(JS::UniqueChars)
JS_GetDefaultLocale(JSContext* cx);

/**
 * Reset the default locale to OS defaults.
 */
extern JS_PUBLIC_API(void)
JS_ResetDefaultLocale(JSRuntime* rt);

/**
 * Locale specific string conversion and error message callbacks.
 */
struct JSLocaleCallbacks {
    JSLocaleToUpperCase     localeToUpperCase; // not used #if EXPOSE_INTL_API
    JSLocaleToLowerCase     localeToLowerCase; // not used #if EXPOSE_INTL_API
    JSLocaleCompare         localeCompare; // not used #if EXPOSE_INTL_API
    JSLocaleToUnicode       localeToUnicode;
};

/**
 * Establish locale callbacks. The pointer must persist as long as the
 * JSContext.  Passing nullptr restores the default behaviour.
 */
extern JS_PUBLIC_API(void)
JS_SetLocaleCallbacks(JSRuntime* rt, const JSLocaleCallbacks* callbacks);

/**
 * Return the address of the current locale callbacks struct, which may
 * be nullptr.
 */
extern JS_PUBLIC_API(const JSLocaleCallbacks*)
JS_GetLocaleCallbacks(JSRuntime* rt);

/************************************************************************/

/*
 * Error reporting.
 *
 * There are four encoding variants for the error reporting API:
 *   UTF-8
 *     JSAPI's default encoding for error handling.  Use this when the encoding
 *     of the error message, format string, and arguments is UTF-8.
 *   ASCII
 *     Equivalent to UTF-8, but also asserts that the error message, format
 *     string, and arguments are all ASCII.  Because ASCII is a subset of UTF-8,
 *     any use of this encoding variant *could* be replaced with use of the
 *     UTF-8 variant.  This variant exists solely to double-check the
 *     developer's assumption that all these strings truly are ASCII, given that
 *     UTF-8 and ASCII strings regrettably have the same C++ type.
 *   UC = UTF-16
 *     Use this when arguments are UTF-16.  The format string must be UTF-8.
 *   Latin1 (planned to be removed)
 *     In this variant, all strings are interpreted byte-for-byte as the
 *     corresponding Unicode codepoint.  This encoding may *safely* be used on
 *     any null-terminated string, regardless of its encoding.  (You shouldn't
 *     *actually* be uncertain, but in the real world, a string's encoding -- if
 *     promised at all -- may be more...aspirational...than reality.)  This
 *     encoding variant will eventually be removed -- work to convert your uses
 *     to UTF-8 as you're able.
 */

namespace JS {
const uint16_t MaxNumErrorArguments = 10;
};

/**
 * Report an exception represented by the sprintf-like conversion of format
 * and its arguments.
 */
extern JS_PUBLIC_API(void)
JS_ReportErrorASCII(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API(void)
JS_ReportErrorLatin1(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API(void)
JS_ReportErrorUTF8(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

/*
 * Use an errorNumber to retrieve the format string, args are char*
 */
extern JS_PUBLIC_API(void)
JS_ReportErrorNumberASCII(JSContext* cx, JSErrorCallback errorCallback,
                          void* userRef, const unsigned errorNumber, ...);

extern JS_PUBLIC_API(void)
JS_ReportErrorNumberASCIIVA(JSContext* cx, JSErrorCallback errorCallback,
                            void* userRef, const unsigned errorNumber, va_list ap);

extern JS_PUBLIC_API(void)
JS_ReportErrorNumberLatin1(JSContext* cx, JSErrorCallback errorCallback,
                           void* userRef, const unsigned errorNumber, ...);

#ifdef va_start
extern JS_PUBLIC_API(void)
JS_ReportErrorNumberLatin1VA(JSContext* cx, JSErrorCallback errorCallback,
                             void* userRef, const unsigned errorNumber, va_list ap);
#endif

extern JS_PUBLIC_API(void)
JS_ReportErrorNumberUTF8(JSContext* cx, JSErrorCallback errorCallback,
                           void* userRef, const unsigned errorNumber, ...);

#ifdef va_start
extern JS_PUBLIC_API(void)
JS_ReportErrorNumberUTF8VA(JSContext* cx, JSErrorCallback errorCallback,
                           void* userRef, const unsigned errorNumber, va_list ap);
#endif

/*
 * Use an errorNumber to retrieve the format string, args are char16_t*
 */
extern JS_PUBLIC_API(void)
JS_ReportErrorNumberUC(JSContext* cx, JSErrorCallback errorCallback,
                     void* userRef, const unsigned errorNumber, ...);

extern JS_PUBLIC_API(void)
JS_ReportErrorNumberUCArray(JSContext* cx, JSErrorCallback errorCallback,
                            void* userRef, const unsigned errorNumber,
                            const char16_t** args);

/**
 * As above, but report a warning instead (JSREPORT_IS_WARNING(report.flags)).
 * Return true if there was no error trying to issue the warning, and if the
 * warning was not converted into an error due to the JSOPTION_WERROR option
 * being set, false otherwise.
 */
extern JS_PUBLIC_API(bool)
JS_ReportWarningASCII(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API(bool)
JS_ReportWarningLatin1(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API(bool)
JS_ReportWarningUTF8(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API(bool)
JS_ReportErrorFlagsAndNumberASCII(JSContext* cx, unsigned flags,
                                  JSErrorCallback errorCallback, void* userRef,
                                  const unsigned errorNumber, ...);

extern JS_PUBLIC_API(bool)
JS_ReportErrorFlagsAndNumberLatin1(JSContext* cx, unsigned flags,
                                   JSErrorCallback errorCallback, void* userRef,
                                   const unsigned errorNumber, ...);

extern JS_PUBLIC_API(bool)
JS_ReportErrorFlagsAndNumberUTF8(JSContext* cx, unsigned flags,
                                 JSErrorCallback errorCallback, void* userRef,
                                 const unsigned errorNumber, ...);

extern JS_PUBLIC_API(bool)
JS_ReportErrorFlagsAndNumberUC(JSContext* cx, unsigned flags,
                               JSErrorCallback errorCallback, void* userRef,
                               const unsigned errorNumber, ...);

/**
 * Complain when out of memory.
 */
extern JS_PUBLIC_API(void)
JS_ReportOutOfMemory(JSContext* cx);

/**
 * Complain when an allocation size overflows the maximum supported limit.
 */
extern JS_PUBLIC_API(void)
JS_ReportAllocationOverflow(JSContext* cx);

/**
 * Base class that implements parts shared by JSErrorReport and
 * JSErrorNotes::Note.
 */
class JSErrorBase
{
    // The (default) error message.
    // If ownsMessage_ is true, the it is freed in destructor.
    JS::ConstUTF8CharsZ message_;

  public:
    JSErrorBase()
      : filename(nullptr), lineno(0), column(0),
        errorNumber(0),
        ownsMessage_(false)
    {}

    ~JSErrorBase() {
        freeMessage();
    }

    // Source file name, URL, etc., or null.
    const char* filename;

    // Source line number.
    unsigned lineno;

    // Zero-based column index in line.
    unsigned column;

    // the error number, e.g. see js.msg.
    unsigned errorNumber;

  private:
    bool ownsMessage_ : 1;

  public:
    const JS::ConstUTF8CharsZ message() const {
        return message_;
    }

    void initOwnedMessage(const char* messageArg) {
        initBorrowedMessage(messageArg);
        ownsMessage_ = true;
    }
    void initBorrowedMessage(const char* messageArg) {
        MOZ_ASSERT(!message_);
        message_ = JS::ConstUTF8CharsZ(messageArg, strlen(messageArg));
    }

    JSString* newMessageString(JSContext* cx);

  private:
    void freeMessage();
};

/**
 * Notes associated with JSErrorReport.
 */
class JSErrorNotes
{
  public:
    class Note : public JSErrorBase
    {};

  private:
    // Stores pointers to each note.
    js::Vector<js::UniquePtr<Note>, 1, js::SystemAllocPolicy> notes_;

  public:
    JSErrorNotes();
    ~JSErrorNotes();

    // Add an note to the given position.
    bool addNoteASCII(JSContext* cx,
                      const char* filename, unsigned lineno, unsigned column,
                      JSErrorCallback errorCallback, void* userRef,
                      const unsigned errorNumber, ...);
    bool addNoteLatin1(JSContext* cx,
                       const char* filename, unsigned lineno, unsigned column,
                       JSErrorCallback errorCallback, void* userRef,
                       const unsigned errorNumber, ...);
    bool addNoteUTF8(JSContext* cx,
                     const char* filename, unsigned lineno, unsigned column,
                     JSErrorCallback errorCallback, void* userRef,
                     const unsigned errorNumber, ...);

    JS_PUBLIC_API(size_t) length();

    // Create a deep copy of notes.
    js::UniquePtr<JSErrorNotes> copy(JSContext* cx);

    class iterator : public std::iterator<std::input_iterator_tag, js::UniquePtr<Note>>
    {
        js::UniquePtr<Note>* note_;
      public:
        explicit iterator(js::UniquePtr<Note>* note = nullptr) : note_(note)
        {}

        bool operator==(iterator other) const {
            return note_ == other.note_;
        }
        bool operator!=(iterator other) const {
            return !(*this == other);
        }
        iterator& operator++() {
            note_++;
            return *this;
        }
        reference operator*() {
            return *note_;
        }
    };
    JS_PUBLIC_API(iterator) begin();
    JS_PUBLIC_API(iterator) end();
};

/**
 * Describes a single error or warning that occurs in the execution of script.
 */
class JSErrorReport : public JSErrorBase
{
    // Offending source line without final '\n'.
    // If ownsLinebuf_ is true, the buffer is freed in destructor.
    const char16_t* linebuf_;

    // Number of chars in linebuf_. Does not include trailing '\0'.
    size_t linebufLength_;

    // The 0-based offset of error token in linebuf_.
    size_t tokenOffset_;

  public:
    JSErrorReport()
      : linebuf_(nullptr), linebufLength_(0), tokenOffset_(0),
        notes(nullptr),
        flags(0), exnType(0), isMuted(false),
        ownsLinebuf_(false)
    {}

    ~JSErrorReport() {
        freeLinebuf();
    }

    // Associated notes, or nullptr if there's no note.
    js::UniquePtr<JSErrorNotes> notes;

    // error/warning, etc.
    unsigned flags;

    // One of the JSExnType constants.
    int16_t exnType;

    // See the comment in TransitiveCompileOptions.
    bool isMuted : 1;

  private:
    bool ownsLinebuf_ : 1;

  public:
    const char16_t* linebuf() const {
        return linebuf_;
    }
    size_t linebufLength() const {
        return linebufLength_;
    }
    size_t tokenOffset() const {
        return tokenOffset_;
    }
    void initOwnedLinebuf(const char16_t* linebufArg, size_t linebufLengthArg,
                          size_t tokenOffsetArg) {
        initBorrowedLinebuf(linebufArg, linebufLengthArg, tokenOffsetArg);
        ownsLinebuf_ = true;
    }
    void initBorrowedLinebuf(const char16_t* linebufArg, size_t linebufLengthArg,
                             size_t tokenOffsetArg);

  private:
    void freeLinebuf();
};

/*
 * JSErrorReport flag values.  These may be freely composed.
 */
#define JSREPORT_ERROR      0x0     /* pseudo-flag for default case */
#define JSREPORT_WARNING    0x1     /* reported via JS_ReportWarning */
#define JSREPORT_EXCEPTION  0x2     /* exception was thrown */
#define JSREPORT_STRICT     0x4     /* error or warning due to strict option */

#define JSREPORT_USER_1     0x8     /* user-defined flag */

/*
 * If JSREPORT_EXCEPTION is set, then a JavaScript-catchable exception
 * has been thrown for this runtime error, and the host should ignore it.
 * Exception-aware hosts should also check for JS_IsExceptionPending if
 * JS_ExecuteScript returns failure, and signal or propagate the exception, as
 * appropriate.
 */
#define JSREPORT_IS_WARNING(flags)      (((flags) & JSREPORT_WARNING) != 0)
#define JSREPORT_IS_EXCEPTION(flags)    (((flags) & JSREPORT_EXCEPTION) != 0)
#define JSREPORT_IS_STRICT(flags)       (((flags) & JSREPORT_STRICT) != 0)

namespace JS {

using WarningReporter = void (*)(JSContext* cx, JSErrorReport* report);

extern JS_PUBLIC_API(WarningReporter)
SetWarningReporter(JSContext* cx, WarningReporter reporter);

extern JS_PUBLIC_API(WarningReporter)
GetWarningReporter(JSContext* cx);

extern JS_PUBLIC_API(bool)
CreateError(JSContext* cx, JSExnType type, HandleObject stack,
            HandleString fileName, uint32_t lineNumber, uint32_t columnNumber,
            JSErrorReport* report, HandleString message, MutableHandleValue rval);

/************************************************************************/

/*
 * Weak Maps.
 */

extern JS_PUBLIC_API(JSObject*)
NewWeakMapObject(JSContext* cx);

extern JS_PUBLIC_API(bool)
IsWeakMapObject(JSObject* obj);

extern JS_PUBLIC_API(bool)
GetWeakMapEntry(JSContext* cx, JS::HandleObject mapObj, JS::HandleObject key,
                JS::MutableHandleValue val);

extern JS_PUBLIC_API(bool)
SetWeakMapEntry(JSContext* cx, JS::HandleObject mapObj, JS::HandleObject key,
                JS::HandleValue val);

/*
 * Map
 */
extern JS_PUBLIC_API(JSObject*)
NewMapObject(JSContext* cx);

extern JS_PUBLIC_API(uint32_t)
MapSize(JSContext* cx, HandleObject obj);

extern JS_PUBLIC_API(bool)
MapGet(JSContext* cx, HandleObject obj,
       HandleValue key, MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
MapHas(JSContext* cx, HandleObject obj, HandleValue key, bool* rval);

extern JS_PUBLIC_API(bool)
MapSet(JSContext* cx, HandleObject obj, HandleValue key, HandleValue val);

extern JS_PUBLIC_API(bool)
MapDelete(JSContext *cx, HandleObject obj, HandleValue key, bool *rval);

extern JS_PUBLIC_API(bool)
MapClear(JSContext* cx, HandleObject obj);

extern JS_PUBLIC_API(bool)
MapKeys(JSContext* cx, HandleObject obj, MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
MapValues(JSContext* cx, HandleObject obj, MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
MapEntries(JSContext* cx, HandleObject obj, MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
MapForEach(JSContext *cx, HandleObject obj, HandleValue callbackFn, HandleValue thisVal);

/*
 * Set
 */
extern JS_PUBLIC_API(JSObject *)
NewSetObject(JSContext *cx);

extern JS_PUBLIC_API(uint32_t)
SetSize(JSContext *cx, HandleObject obj);

extern JS_PUBLIC_API(bool)
SetHas(JSContext *cx, HandleObject obj, HandleValue key, bool *rval);

extern JS_PUBLIC_API(bool)
SetDelete(JSContext *cx, HandleObject obj, HandleValue key, bool *rval);

extern JS_PUBLIC_API(bool)
SetAdd(JSContext *cx, HandleObject obj, HandleValue key);

extern JS_PUBLIC_API(bool)
SetClear(JSContext *cx, HandleObject obj);

extern JS_PUBLIC_API(bool)
SetKeys(JSContext *cx, HandleObject obj, MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
SetValues(JSContext *cx, HandleObject obj, MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
SetEntries(JSContext *cx, HandleObject obj, MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
SetForEach(JSContext *cx, HandleObject obj, HandleValue callbackFn, HandleValue thisVal);

} /* namespace JS */

/*
 * Dates.
 */

extern JS_PUBLIC_API(JSObject*)
JS_NewDateObject(JSContext* cx, int year, int mon, int mday, int hour, int min, int sec);

/**
 * Returns true and sets |*isDate| indicating whether |obj| is a Date object or
 * a wrapper around one, otherwise returns false on failure.
 *
 * This method returns true with |*isDate == false| when passed a proxy whose
 * target is a Date, or when passed a revoked proxy.
 */
extern JS_PUBLIC_API(bool)
JS_ObjectIsDate(JSContext* cx, JS::HandleObject obj, bool* isDate);

/************************************************************************/

/*
 * Regular Expressions.
 */
#define JSREG_FOLD      0x01u   /* fold uppercase to lowercase */
#define JSREG_GLOB      0x02u   /* global exec, creates array of matches */
#define JSREG_MULTILINE 0x04u   /* treat ^ and $ as begin and end of line */
#define JSREG_STICKY    0x08u   /* only match starting at lastIndex */
#define JSREG_UNICODE   0x10u   /* unicode */

extern JS_PUBLIC_API(JSObject*)
JS_NewRegExpObject(JSContext* cx, const char* bytes, size_t length, unsigned flags);

extern JS_PUBLIC_API(JSObject*)
JS_NewUCRegExpObject(JSContext* cx, const char16_t* chars, size_t length, unsigned flags);

extern JS_PUBLIC_API(bool)
JS_SetRegExpInput(JSContext* cx, JS::HandleObject obj, JS::HandleString input);

extern JS_PUBLIC_API(bool)
JS_ClearRegExpStatics(JSContext* cx, JS::HandleObject obj);

extern JS_PUBLIC_API(bool)
JS_ExecuteRegExp(JSContext* cx, JS::HandleObject obj, JS::HandleObject reobj,
                 char16_t* chars, size_t length, size_t* indexp, bool test,
                 JS::MutableHandleValue rval);

/* RegExp interface for clients without a global object. */

extern JS_PUBLIC_API(bool)
JS_ExecuteRegExpNoStatics(JSContext* cx, JS::HandleObject reobj, char16_t* chars, size_t length,
                          size_t* indexp, bool test, JS::MutableHandleValue rval);

/**
 * Returns true and sets |*isRegExp| indicating whether |obj| is a RegExp
 * object or a wrapper around one, otherwise returns false on failure.
 *
 * This method returns true with |*isRegExp == false| when passed a proxy whose
 * target is a RegExp, or when passed a revoked proxy.
 */
extern JS_PUBLIC_API(bool)
JS_ObjectIsRegExp(JSContext* cx, JS::HandleObject obj, bool* isRegExp);

extern JS_PUBLIC_API(unsigned)
JS_GetRegExpFlags(JSContext* cx, JS::HandleObject obj);

extern JS_PUBLIC_API(JSString*)
JS_GetRegExpSource(JSContext* cx, JS::HandleObject obj);

/************************************************************************/

extern JS_PUBLIC_API(bool)
JS_IsExceptionPending(JSContext* cx);

extern JS_PUBLIC_API(bool)
JS_GetPendingException(JSContext* cx, JS::MutableHandleValue vp);

extern JS_PUBLIC_API(void)
JS_SetPendingException(JSContext* cx, JS::HandleValue v);

extern JS_PUBLIC_API(void)
JS_ClearPendingException(JSContext* cx);

namespace JS {

/**
 * Save and later restore the current exception state of a given JSContext.
 * This is useful for implementing behavior in C++ that's like try/catch
 * or try/finally in JS.
 *
 * Typical usage:
 *
 *     bool ok = JS::Evaluate(cx, ...);
 *     AutoSaveExceptionState savedExc(cx);
 *     ... cleanup that might re-enter JS ...
 *     return ok;
 */
class JS_PUBLIC_API(AutoSaveExceptionState)
{
  private:
    JSContext* context;
    bool wasPropagatingForcedReturn;
    bool wasOverRecursed;
    bool wasThrowing;
    RootedValue exceptionValue;

  public:
    /*
     * Take a snapshot of cx's current exception state. Then clear any current
     * pending exception in cx.
     */
    explicit AutoSaveExceptionState(JSContext* cx);

    /*
     * If neither drop() nor restore() was called, restore the exception
     * state only if no exception is currently pending on cx.
     */
    ~AutoSaveExceptionState();

    /*
     * Discard any stored exception state.
     * If this is called, the destructor is a no-op.
     */
    void drop() {
        wasPropagatingForcedReturn = false;
        wasOverRecursed = false;
        wasThrowing = false;
        exceptionValue.setUndefined();
    }

    /*
     * Replace cx's exception state with the stored exception state. Then
     * discard the stored exception state. If this is called, the
     * destructor is a no-op.
     */
    void restore();
};

} /* namespace JS */

/* Deprecated API. Use AutoSaveExceptionState instead. */
extern JS_PUBLIC_API(JSExceptionState*)
JS_SaveExceptionState(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_RestoreExceptionState(JSContext* cx, JSExceptionState* state);

extern JS_PUBLIC_API(void)
JS_DropExceptionState(JSContext* cx, JSExceptionState* state);

/**
 * If the given object is an exception object, the exception will have (or be
 * able to lazily create) an error report struct, and this function will return
 * the address of that struct.  Otherwise, it returns nullptr. The lifetime
 * of the error report struct that might be returned is the same as the
 * lifetime of the exception object.
 */
extern JS_PUBLIC_API(JSErrorReport*)
JS_ErrorFromException(JSContext* cx, JS::HandleObject obj);

namespace JS {
/**
 * If the given object is an exception object (or an unwrappable
 * cross-compartment wrapper for one), return the stack for that exception, if
 * any.  Will return null if the given object is not an exception object
 * (including if it's null or a security wrapper that can't be unwrapped) or if
 * the exception has no stack.
 */
extern JS_PUBLIC_API(JSObject*)
ExceptionStackOrNull(JS::HandleObject obj);

} /* namespace JS */

/**
 * A JS context always has an "owner thread". The owner thread is set when the
 * context is created (to the current thread) and practically all entry points
 * into the JS engine check that a context (or anything contained in the
 * context: runtime, compartment, object, etc) is only touched by its owner
 * thread. Embeddings may check this invariant outside the JS engine by calling
 * JS_AbortIfWrongThread (which will abort if not on the owner thread, even for
 * non-debug builds).
 */

extern JS_PUBLIC_API(void)
JS_AbortIfWrongThread(JSContext* cx);

/************************************************************************/

/**
 * A constructor can request that the JS engine create a default new 'this'
 * object of the given class, using the callee to determine parentage and
 * [[Prototype]].
 */
extern JS_PUBLIC_API(JSObject*)
JS_NewObjectForConstructor(JSContext* cx, const JSClass* clasp, const JS::CallArgs& args);

/************************************************************************/

#ifdef JS_GC_ZEAL
#define JS_DEFAULT_ZEAL_FREQ 100

extern JS_PUBLIC_API(void)
JS_GetGCZealBits(JSContext* cx, uint32_t* zealBits, uint32_t* frequency, uint32_t* nextScheduled);

extern JS_PUBLIC_API(void)
JS_SetGCZeal(JSContext* cx, uint8_t zeal, uint32_t frequency);

extern JS_PUBLIC_API(void)
JS_ScheduleGC(JSContext* cx, uint32_t count);
#endif

extern JS_PUBLIC_API(void)
JS_SetParallelParsingEnabled(JSContext* cx, bool enabled);

extern JS_PUBLIC_API(void)
JS_SetOffthreadIonCompilationEnabled(JSContext* cx, bool enabled);

#define JIT_COMPILER_OPTIONS(Register)                                      \
    Register(BASELINE_WARMUP_TRIGGER, "baseline.warmup.trigger")            \
    Register(ION_WARMUP_TRIGGER, "ion.warmup.trigger")                      \
    Register(ION_GVN_ENABLE, "ion.gvn.enable")                              \
    Register(ION_FORCE_IC, "ion.forceinlineCaches")                         \
    Register(ION_ENABLE, "ion.enable")                                      \
    Register(ION_INTERRUPT_WITHOUT_SIGNAL, "ion.interrupt-without-signals") \
    Register(ION_CHECK_RANGE_ANALYSIS, "ion.check-range-analysis")          \
    Register(BASELINE_ENABLE, "baseline.enable")                            \
    Register(OFFTHREAD_COMPILATION_ENABLE, "offthread-compilation.enable")  \
    Register(FULL_DEBUG_CHECKS, "jit.full-debug-checks")                    \
    Register(JUMP_THRESHOLD, "jump-threshold")                              \
    Register(SIMULATOR_ALWAYS_INTERRUPT, "simulator.always-interrupt")      \
    Register(SPECTRE_INDEX_MASKING, "spectre.index-masking")                \
    Register(SPECTRE_OBJECT_MITIGATIONS_BARRIERS, "spectre.object-mitigations.barriers") \
    Register(SPECTRE_OBJECT_MITIGATIONS_MISC, "spectre.object-mitigations.misc") \
    Register(SPECTRE_STRING_MITIGATIONS, "spectre.string-mitigations")      \
    Register(SPECTRE_VALUE_MASKING, "spectre.value-masking")                \
    Register(SPECTRE_JIT_TO_CXX_CALLS, "spectre.jit-to-C++-calls")          \
    Register(ASMJS_ATOMICS_ENABLE, "asmjs.atomics.enable")                  \
    Register(WASM_FOLD_OFFSETS, "wasm.fold-offsets")                        \
    Register(WASM_DELAY_TIER2, "wasm.delay-tier2")

typedef enum JSJitCompilerOption {
#define JIT_COMPILER_DECLARE(key, str) \
    JSJITCOMPILER_ ## key,

    JIT_COMPILER_OPTIONS(JIT_COMPILER_DECLARE)
#undef JIT_COMPILER_DECLARE

    JSJITCOMPILER_NOT_AN_OPTION
} JSJitCompilerOption;

extern JS_PUBLIC_API(void)
JS_SetGlobalJitCompilerOption(JSContext* cx, JSJitCompilerOption opt, uint32_t value);
extern JS_PUBLIC_API(bool)
JS_GetGlobalJitCompilerOption(JSContext* cx, JSJitCompilerOption opt, uint32_t* valueOut);

/**
 * Convert a uint32_t index into a jsid.
 */
extern JS_PUBLIC_API(bool)
JS_IndexToId(JSContext* cx, uint32_t index, JS::MutableHandleId);

/**
 * Convert chars into a jsid.
 *
 * |chars| may not be an index.
 */
extern JS_PUBLIC_API(bool)
JS_CharsToId(JSContext* cx, JS::TwoByteChars chars, JS::MutableHandleId);

/**
 *  Test if the given string is a valid ECMAScript identifier
 */
extern JS_PUBLIC_API(bool)
JS_IsIdentifier(JSContext* cx, JS::HandleString str, bool* isIdentifier);

/**
 * Test whether the given chars + length are a valid ECMAScript identifier.
 * This version is infallible, so just returns whether the chars are an
 * identifier.
 */
extern JS_PUBLIC_API(bool)
JS_IsIdentifier(const char16_t* chars, size_t length);

namespace js {
class ScriptSource;
} // namespace js

namespace JS {

class MOZ_RAII JS_PUBLIC_API(AutoFilename)
{
  private:
    js::ScriptSource* ss_;
    mozilla::Variant<const char*, UniqueChars> filename_;

    AutoFilename(const AutoFilename&) = delete;
    AutoFilename& operator=(const AutoFilename&) = delete;

  public:
    AutoFilename()
      : ss_(nullptr),
        filename_(mozilla::AsVariant<const char*>(nullptr))
    {}

    ~AutoFilename() {
        reset();
    }

    void reset();

    void setOwned(UniqueChars&& filename);
    void setUnowned(const char* filename);
    void setScriptSource(js::ScriptSource* ss);

    const char* get() const;
};

/**
 * Return the current filename, line number and column number of the most
 * currently running frame. Returns true if a scripted frame was found, false
 * otherwise.
 *
 * If a the embedding has hidden the scripted caller for the topmost activation
 * record, this will also return false.
 */
extern JS_PUBLIC_API(bool)
DescribeScriptedCaller(JSContext* cx, AutoFilename* filename = nullptr,
                       unsigned* lineno = nullptr, unsigned* column = nullptr);

extern JS_PUBLIC_API(JSObject*)
GetScriptedCallerGlobal(JSContext* cx);

/**
 * Informs the JS engine that the scripted caller should be hidden. This can be
 * used by the embedding to maintain an override of the scripted caller in its
 * calculations, by hiding the scripted caller in the JS engine and pushing data
 * onto a separate stack, which it inspects when DescribeScriptedCaller returns
 * null.
 *
 * We maintain a counter on each activation record. Add() increments the counter
 * of the topmost activation, and Remove() decrements it. The count may never
 * drop below zero, and must always be exactly zero when the activation is
 * popped from the stack.
 */
extern JS_PUBLIC_API(void)
HideScriptedCaller(JSContext* cx);

extern JS_PUBLIC_API(void)
UnhideScriptedCaller(JSContext* cx);

class MOZ_RAII AutoHideScriptedCaller
{
  public:
    explicit AutoHideScriptedCaller(JSContext* cx
                                    MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : mContext(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        HideScriptedCaller(mContext);
    }
    ~AutoHideScriptedCaller() {
        UnhideScriptedCaller(mContext);
    }

  protected:
    JSContext* mContext;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * Encode/Decode interpreted scripts and functions to/from memory.
 */

typedef mozilla::Vector<uint8_t> TranscodeBuffer;
typedef mozilla::Range<uint8_t> TranscodeRange;

struct TranscodeSource
{
    TranscodeSource(const TranscodeRange& range_, const char* file, uint32_t line)
        : range(range_), filename(file), lineno(line)
    {}

    const TranscodeRange range;
    const char* filename;
    const uint32_t lineno;
};

typedef mozilla::Vector<JS::TranscodeSource> TranscodeSources;

enum TranscodeResult
{
    // Successful encoding / decoding.
    TranscodeResult_Ok = 0,

    // A warning message, is set to the message out-param.
    TranscodeResult_Failure = 0x100,
    TranscodeResult_Failure_BadBuildId =          TranscodeResult_Failure | 0x1,
    TranscodeResult_Failure_RunOnceNotSupported = TranscodeResult_Failure | 0x2,
    TranscodeResult_Failure_AsmJSNotSupported =   TranscodeResult_Failure | 0x3,
    TranscodeResult_Failure_BadDecode =           TranscodeResult_Failure | 0x4,
    TranscodeResult_Failure_WrongCompileOption =  TranscodeResult_Failure | 0x5,
    TranscodeResult_Failure_NotInterpretedFun =   TranscodeResult_Failure | 0x6,

    // There is a pending exception on the context.
    TranscodeResult_Throw = 0x200
};

extern JS_PUBLIC_API(TranscodeResult)
EncodeScript(JSContext* cx, TranscodeBuffer& buffer, JS::HandleScript script);

extern JS_PUBLIC_API(TranscodeResult)
EncodeInterpretedFunction(JSContext* cx, TranscodeBuffer& buffer, JS::HandleObject funobj);

extern JS_PUBLIC_API(TranscodeResult)
DecodeScript(JSContext* cx, TranscodeBuffer& buffer, JS::MutableHandleScript scriptp,
             size_t cursorIndex = 0);

extern JS_PUBLIC_API(TranscodeResult)
DecodeScript(JSContext* cx, const TranscodeRange& range, JS::MutableHandleScript scriptp);

extern JS_PUBLIC_API(TranscodeResult)
DecodeInterpretedFunction(JSContext* cx, TranscodeBuffer& buffer, JS::MutableHandleFunction funp,
                          size_t cursorIndex = 0);

// Register an encoder on the given script source, such that all functions can
// be encoded as they are parsed. This strategy is used to avoid blocking the
// active thread in a non-interruptible way.
//
// The |script| argument of |StartIncrementalEncoding| and
// |FinishIncrementalEncoding| should be the top-level script returned either as
// an out-param of any of the |Compile| functions, or the result of
// |FinishOffThreadScript|.
//
// The |buffer| argument of |FinishIncrementalEncoding| is used for appending
// the encoded bytecode into the buffer. If any of these functions failed, the
// content of |buffer| would be undefined.
extern JS_PUBLIC_API(bool)
StartIncrementalEncoding(JSContext* cx, JS::HandleScript script);

extern JS_PUBLIC_API(bool)
FinishIncrementalEncoding(JSContext* cx, JS::HandleScript script, TranscodeBuffer& buffer);

} /* namespace JS */

namespace js {

enum class StackFormat { SpiderMonkey, V8, Default };

/*
 * Sets the format used for stringifying Error stacks.
 *
 * The default format is StackFormat::SpiderMonkey.  Use StackFormat::V8
 * in order to emulate V8's stack formatting.  StackFormat::Default can't be
 * used here.
 */
extern JS_PUBLIC_API(void)
SetStackFormat(JSContext* cx, StackFormat format);

extern JS_PUBLIC_API(StackFormat)
GetStackFormat(JSContext* cx);

}

namespace JS {

/*
 * This callback represents a request by the JS engine to open for reading the
 * existing cache entry for the given global and char range that may contain a
 * module. If a cache entry exists, the callback shall return 'true' and return
 * the size, base address and an opaque file handle as outparams. If the
 * callback returns 'true', the JS engine guarantees a call to
 * CloseAsmJSCacheEntryForReadOp, passing the same base address, size and
 * handle.
 */
typedef bool
(* OpenAsmJSCacheEntryForReadOp)(HandleObject global, const char16_t* begin, const char16_t* limit,
                                 size_t* size, const uint8_t** memory, intptr_t* handle);
typedef void
(* CloseAsmJSCacheEntryForReadOp)(size_t size, const uint8_t* memory, intptr_t handle);

/** The list of reasons why an asm.js module may not be stored in the cache. */
enum AsmJSCacheResult
{
    AsmJSCache_Success,
    AsmJSCache_MIN = AsmJSCache_Success,
    AsmJSCache_ModuleTooSmall,
    AsmJSCache_SynchronousScript,
    AsmJSCache_QuotaExceeded,
    AsmJSCache_StorageInitFailure,
    AsmJSCache_Disabled_Internal,
    AsmJSCache_Disabled_ShellFlags,
    AsmJSCache_Disabled_JitInspector,
    AsmJSCache_InternalError,
    AsmJSCache_Disabled_PrivateBrowsing,
    AsmJSCache_LIMIT
};

/*
 * This callback represents a request by the JS engine to open for writing a
 * cache entry of the given size for the given global and char range containing
 * the just-compiled module. If cache entry space is available, the callback
 * shall return 'true' and return the base address and an opaque file handle as
 * outparams. If the callback returns 'true', the JS engine guarantees a call
 * to CloseAsmJSCacheEntryForWriteOp passing the same base address, size and
 * handle.
 */
typedef AsmJSCacheResult
(* OpenAsmJSCacheEntryForWriteOp)(HandleObject global, const char16_t* begin,
                                  const char16_t* end, size_t size,
                                  uint8_t** memory, intptr_t* handle);
typedef void
(* CloseAsmJSCacheEntryForWriteOp)(size_t size, uint8_t* memory, intptr_t handle);

struct AsmJSCacheOps
{
    OpenAsmJSCacheEntryForReadOp openEntryForRead;
    CloseAsmJSCacheEntryForReadOp closeEntryForRead;
    OpenAsmJSCacheEntryForWriteOp openEntryForWrite;
    CloseAsmJSCacheEntryForWriteOp closeEntryForWrite;
};

extern JS_PUBLIC_API(void)
SetAsmJSCacheOps(JSContext* cx, const AsmJSCacheOps* callbacks);

/**
 * Return the buildId (represented as a sequence of characters) associated with
 * the currently-executing build. If the JS engine is embedded such that a
 * single cache entry can be observed by different compiled versions of the JS
 * engine, it is critical that the buildId shall change for each new build of
 * the JS engine.
 */
typedef js::Vector<char, 0, js::SystemAllocPolicy> BuildIdCharVector;

typedef bool
(* BuildIdOp)(BuildIdCharVector* buildId);

extern JS_PUBLIC_API(void)
SetBuildIdOp(JSContext* cx, BuildIdOp buildIdOp);

/**
 * The WasmModule interface allows the embedding to hold a reference to the
 * underying C++ implementation of a JS WebAssembly.Module object for purposes
 * of efficient postMessage() and (de)serialization from a random thread.
 *
 * For postMessage() sharing:
 *
 * - GetWasmModule() is called when making a structured clone of payload
 * containing a WebAssembly.Module object. The structured clone buffer holds a
 * refcount of the JS::WasmModule until createObject() is called in the target
 * agent's JSContext. The new WebAssembly.Module object continues to hold the
 * JS::WasmModule and thus the final reference of a JS::WasmModule may be
 * dropped from any thread and so the virtual destructor (and all internal
 * methods of the C++ module) must be thread-safe.
 *
 * For (de)serialization:
 *
 * - Serialization starts when WebAssembly.Module is passed to the
 * structured-clone algorithm. JS::GetWasmModule is called on the JSRuntime
 * thread that initiated the structured clone to get the JS::WasmModule.
 * This interface is then taken to a background thread where the bytecode and
 * compiled code are written into separate files: a bytecode file that always
 * allows successful deserialization and a compiled-code file keyed on cpu- and
 * build-id that may become invalid if either of these change between
 * serialization and deserialization. Due to tiering, the serialization must
 * asynchronously wait for compilation to complete before requesting the
 * module's compiled code. After serialization, a reference is dropped from a
 * separate thread so the virtual destructor must be thread-safe.
 *
 * - Deserialization starts when the structured clone algorithm encounters a
 * serialized WebAssembly.Module. On a background thread, the compiled-code file
 * is opened and CompiledWasmModuleAssumptionsMatch is called to see if it is
 * still valid (as described above). DeserializeWasmModule is then called to
 * construct a JS::WasmModule (also on the background thread), passing the
 * bytecode file descriptor and, if valid, the compiled-code file descriptor.
 * The JS::WasmObject is then transported to a JSContext thread and the wrapping
 * WebAssembly.Module object is created by calling createObject().
 */

class WasmModuleListener
{
  protected:
    virtual ~WasmModuleListener() {}

  public:
    // These method signatures are chosen to exactly match nsISupports so that a
    // plain nsISupports-implementing class can trivially implement this
    // interface too. We can't simply #include "nsISupports.h" so we use MFBT
    // equivalents for all the platform-dependent types.
    virtual MozExternalRefCountType MOZ_XPCOM_ABI AddRef() = 0;
    virtual MozExternalRefCountType MOZ_XPCOM_ABI Release() = 0;

    virtual void onCompilationComplete() = 0;
};

struct WasmModule : js::AtomicRefCounted<WasmModule>
{
    virtual ~WasmModule() {}

    virtual size_t bytecodeSerializedSize() const = 0;
    virtual void bytecodeSerialize(uint8_t* bytecodeBegin, size_t bytecodeSize) const = 0;

    // Compilation must complete before the serialized code is requested. If
    // compilation is not complete, the embedding must wait until notified by
    // implementing WasmModuleListener. SpiderMonkey will hold a RefPtr to
    // 'listener' until onCompilationComplete() is called.
    virtual bool compilationComplete() const = 0;
    virtual bool notifyWhenCompilationComplete(WasmModuleListener* listener) = 0;
    virtual size_t compiledSerializedSize() const = 0;
    virtual void compiledSerialize(uint8_t* compiledBegin, size_t compiledSize) const = 0;

    virtual JSObject* createObject(JSContext* cx) = 0;
};

extern JS_PUBLIC_API(bool)
IsWasmModuleObject(HandleObject obj);

extern JS_PUBLIC_API(RefPtr<WasmModule>)
GetWasmModule(HandleObject obj);

extern JS_PUBLIC_API(bool)
CompiledWasmModuleAssumptionsMatch(PRFileDesc* compiled, BuildIdCharVector&& buildId);

extern JS_PUBLIC_API(RefPtr<WasmModule>)
DeserializeWasmModule(PRFileDesc* bytecode, PRFileDesc* maybeCompiled, BuildIdCharVector&& buildId,
                      JS::UniqueChars filename, unsigned line, unsigned column);

/**
 * Convenience class for imitating a JS level for-of loop. Typical usage:
 *
 *     ForOfIterator it(cx);
 *     if (!it.init(iterable))
 *       return false;
 *     RootedValue val(cx);
 *     while (true) {
 *       bool done;
 *       if (!it.next(&val, &done))
 *         return false;
 *       if (done)
 *         break;
 *       if (!DoStuff(cx, val))
 *         return false;
 *     }
 */
class MOZ_STACK_CLASS JS_PUBLIC_API(ForOfIterator) {
  protected:
    JSContext* cx_;
    /*
     * Use the ForOfPIC on the global object (see vm/GlobalObject.h) to try
     * to optimize iteration across arrays.
     *
     *  Case 1: Regular Iteration
     *      iterator - pointer to the iterator object.
     *      nextMethod - value of |iterator|.next.
     *      index - fixed to NOT_ARRAY (== UINT32_MAX)
     *
     *  Case 2: Optimized Array Iteration
     *      iterator - pointer to the array object.
     *      nextMethod - the undefined value.
     *      index - current position in array.
     *
     * The cases are distinguished by whether or not |index| is equal to NOT_ARRAY.
     */
    JS::RootedObject iterator;
    JS::RootedValue nextMethod;
    uint32_t index;

    static const uint32_t NOT_ARRAY = UINT32_MAX;

    ForOfIterator(const ForOfIterator&) = delete;
    ForOfIterator& operator=(const ForOfIterator&) = delete;

  public:
    explicit ForOfIterator(JSContext* cx)
      : cx_(cx), iterator(cx_), nextMethod(cx), index(NOT_ARRAY)
    { }

    enum NonIterableBehavior {
        ThrowOnNonIterable,
        AllowNonIterable
    };

    /**
     * Initialize the iterator.  If AllowNonIterable is passed then if getting
     * the @@iterator property from iterable returns undefined init() will just
     * return true instead of throwing.  Callers must then check
     * valueIsIterable() before continuing with the iteration.
     */
    bool init(JS::HandleValue iterable,
              NonIterableBehavior nonIterableBehavior = ThrowOnNonIterable);

    /**
     * Get the next value from the iterator.  If false *done is true
     * after this call, do not examine val.
     */
    bool next(JS::MutableHandleValue val, bool* done);

    /**
     * Close the iterator.
     * For the case that completion type is throw.
     */
    void closeThrow();

    /**
     * If initialized with throwOnNonCallable = false, check whether
     * the value is iterable.
     */
    bool valueIsIterable() const {
        return iterator;
    }

  private:
    inline bool nextFromOptimizedArray(MutableHandleValue val, bool* done);
};


/**
 * If a large allocation fails when calling pod_{calloc,realloc}CanGC, the JS
 * engine may call the large-allocation-failure callback, if set, to allow the
 * embedding to flush caches, possibly perform shrinking GCs, etc. to make some
 * room. The allocation will then be retried (and may still fail.) This callback
 * can be called on any thread and must be set at most once in a process.
 */

typedef void
(* LargeAllocationFailureCallback)();

extern JS_PUBLIC_API(void)
SetProcessLargeAllocationFailureCallback(LargeAllocationFailureCallback afc);

/**
 * Unlike the error reporter, which is only called if the exception for an OOM
 * bubbles up and is not caught, the OutOfMemoryCallback is called immediately
 * at the OOM site to allow the embedding to capture the current state of heap
 * allocation before anything is freed. If the large-allocation-failure callback
 * is called at all (not all allocation sites call the large-allocation-failure
 * callback on failure), it is called before the out-of-memory callback; the
 * out-of-memory callback is only called if the allocation still fails after the
 * large-allocation-failure callback has returned.
 */

typedef void
(* OutOfMemoryCallback)(JSContext* cx, void* data);

extern JS_PUBLIC_API(void)
SetOutOfMemoryCallback(JSContext* cx, OutOfMemoryCallback cb, void* data);

/**
 * Capture all frames.
 */
struct AllFrames { };

/**
 * Capture at most this many frames.
 */
struct MaxFrames
{
    uint32_t maxFrames;

    explicit MaxFrames(uint32_t max)
      : maxFrames(max)
    {
        MOZ_ASSERT(max > 0);
    }
};

/**
 * Capture the first frame with the given principals. By default, do not
 * consider self-hosted frames with the given principals as satisfying the stack
 * capture.
 */
struct JS_PUBLIC_API(FirstSubsumedFrame)
{
    JSContext* cx;
    JSPrincipals* principals;
    bool ignoreSelfHosted;

    /**
     * Use the cx's current compartment's principals.
     */
    explicit FirstSubsumedFrame(JSContext* cx, bool ignoreSelfHostedFrames = true);

    explicit FirstSubsumedFrame(JSContext* ctx, JSPrincipals* p, bool ignoreSelfHostedFrames = true)
      : cx(ctx)
      , principals(p)
      , ignoreSelfHosted(ignoreSelfHostedFrames)
    {
        if (principals)
            JS_HoldPrincipals(principals);
    }

    // No copying because we want to avoid holding and dropping principals
    // unnecessarily.
    FirstSubsumedFrame(const FirstSubsumedFrame&) = delete;
    FirstSubsumedFrame& operator=(const FirstSubsumedFrame&) = delete;

    FirstSubsumedFrame(FirstSubsumedFrame&& rhs)
      : principals(rhs.principals)
      , ignoreSelfHosted(rhs.ignoreSelfHosted)
    {
        MOZ_ASSERT(this != &rhs, "self move disallowed");
        rhs.principals = nullptr;
    }

    FirstSubsumedFrame& operator=(FirstSubsumedFrame&& rhs) {
        new (this) FirstSubsumedFrame(mozilla::Move(rhs));
        return *this;
    }

    ~FirstSubsumedFrame() {
        if (principals)
            JS_DropPrincipals(cx, principals);
    }
};

using StackCapture = mozilla::Variant<AllFrames, MaxFrames, FirstSubsumedFrame>;

/**
 * Capture the current call stack as a chain of SavedFrame JSObjects, and set
 * |stackp| to the SavedFrame for the youngest stack frame, or nullptr if there
 * are no JS frames on the stack.
 *
 * The |capture| parameter describes the portion of the JS stack to capture:
 *
 *   * |JS::AllFrames|: Capture all frames on the stack.
 *
 *   * |JS::MaxFrames|: Capture no more than |JS::MaxFrames::maxFrames| from the
 *      stack.
 *
 *   * |JS::FirstSubsumedFrame|: Capture the first frame whose principals are
 *     subsumed by |JS::FirstSubsumedFrame::principals|. By default, do not
 *     consider self-hosted frames; this can be controlled via the
 *     |JS::FirstSubsumedFrame::ignoreSelfHosted| flag. Do not capture any async
 *     stack.
 */
extern JS_PUBLIC_API(bool)
CaptureCurrentStack(JSContext* cx, MutableHandleObject stackp,
                    StackCapture&& capture = StackCapture(AllFrames()));

/*
 * This is a utility function for preparing an async stack to be used
 * by some other object.  This may be used when you need to treat a
 * given stack trace as an async parent.  If you just need to capture
 * the current stack, async parents and all, use CaptureCurrentStack
 * instead.
 *
 * Here |asyncStack| is the async stack to prepare.  It is copied into
 * |cx|'s current compartment, and the newest frame is given
 * |asyncCause| as its asynchronous cause.  If |maxFrameCount| is
 * |Some(n)|, capture at most the youngest |n| frames.  The
 * new stack object is written to |stackp|.  Returns true on success,
 * or sets an exception and returns |false| on error.
 */
extern JS_PUBLIC_API(bool)
CopyAsyncStack(JSContext* cx, HandleObject asyncStack,
               HandleString asyncCause, MutableHandleObject stackp,
               const mozilla::Maybe<size_t>& maxFrameCount);

/*
 * Accessors for working with SavedFrame JSObjects
 *
 * Each of these functions assert that if their `HandleObject savedFrame`
 * argument is non-null, its JSClass is the SavedFrame class (or it is a
 * cross-compartment or Xray wrapper around an object with the SavedFrame class)
 * and the object is not the SavedFrame.prototype object.
 *
 * Each of these functions will find the first SavedFrame object in the chain
 * whose underlying stack frame principals are subsumed by the cx's current
 * compartment's principals, and operate on that SavedFrame object. This
 * prevents leaking information about privileged frames to un-privileged
 * callers. As a result, the SavedFrame in parameters do _NOT_ need to be in the
 * same compartment as the cx, and the various out parameters are _NOT_
 * guaranteed to be in the same compartment as cx.
 *
 * You may consider or skip over self-hosted frames by passing
 * `SavedFrameSelfHosted::Include` or `SavedFrameSelfHosted::Exclude`
 * respectively.
 *
 * Additionally, it may be the case that there is no such SavedFrame object
 * whose captured frame's principals are subsumed by the caller's compartment's
 * principals! If the `HandleObject savedFrame` argument is null, or the
 * caller's principals do not subsume any of the chained SavedFrame object's
 * principals, `SavedFrameResult::AccessDenied` is returned and a (hopefully)
 * sane default value is chosen for the out param.
 *
 * See also `js/src/doc/SavedFrame/SavedFrame.md`.
 */

enum class SavedFrameResult {
    Ok,
    AccessDenied
};

enum class SavedFrameSelfHosted {
    Include,
    Exclude
};

/**
 * Given a SavedFrame JSObject, get its source property. Defaults to the empty
 * string.
 */
extern JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameSource(JSContext* cx, HandleObject savedFrame, MutableHandleString sourcep,
                    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its line property. Defaults to 0.
 */
extern JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameLine(JSContext* cx, HandleObject savedFrame, uint32_t* linep,
                  SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its column property. Defaults to 0.
 */
extern JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameColumn(JSContext* cx, HandleObject savedFrame, uint32_t* columnp,
                    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its functionDisplayName string, or nullptr
 * if SpiderMonkey was unable to infer a name for the captured frame's
 * function. Defaults to nullptr.
 */
extern JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameFunctionDisplayName(JSContext* cx, HandleObject savedFrame, MutableHandleString namep,
                                 SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its asyncCause string. Defaults to nullptr.
 */
extern JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameAsyncCause(JSContext* cx, HandleObject savedFrame, MutableHandleString asyncCausep,
                        SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its asyncParent SavedFrame object or nullptr
 * if there is no asyncParent. The `asyncParentp` out parameter is _NOT_
 * guaranteed to be in the cx's compartment. Defaults to nullptr.
 */
extern JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameAsyncParent(JSContext* cx, HandleObject savedFrame, MutableHandleObject asyncParentp,
                SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its parent SavedFrame object or nullptr if
 * it is the oldest frame in the stack. The `parentp` out parameter is _NOT_
 * guaranteed to be in the cx's compartment. Defaults to nullptr.
 */
extern JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameParent(JSContext* cx, HandleObject savedFrame, MutableHandleObject parentp,
                    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject stack, stringify it in the same format as
 * Error.prototype.stack. The stringified stack out parameter is placed in the
 * cx's compartment. Defaults to the empty string.
 *
 * The same notes above about SavedFrame accessors applies here as well: cx
 * doesn't need to be in stack's compartment, and stack can be null, a
 * SavedFrame object, or a wrapper (CCW or Xray) around a SavedFrame object.
 *
 * Optional indent parameter specifies the number of white spaces to indent
 * each line.
 */
extern JS_PUBLIC_API(bool)
BuildStackString(JSContext* cx, HandleObject stack, MutableHandleString stringp,
                 size_t indent = 0, js::StackFormat stackFormat = js::StackFormat::Default);

/**
 * Return true iff the given object is either a SavedFrame object or wrapper
 * around a SavedFrame object, and it is not the SavedFrame.prototype object.
 */
extern JS_PUBLIC_API(bool)
IsSavedFrame(JSObject* obj);

} /* namespace JS */


/* Stopwatch-based performance monitoring. */

namespace js {

class AutoStopwatch;

/**
 * Abstract base class for a representation of the performance of a
 * component. Embeddings interested in performance monitoring should
 * provide a concrete implementation of this class, as well as the
 * relevant callbacks (see below).
 */
struct JS_PUBLIC_API(PerformanceGroup) {
    PerformanceGroup();

    // The current iteration of the event loop.
    uint64_t iteration() const;

    // `true` if an instance of `AutoStopwatch` is already monitoring
    // the performance of this performance group for this iteration
    // of the event loop, `false` otherwise.
    bool isAcquired(uint64_t it) const;

    // `true` if a specific instance of `AutoStopwatch` is already monitoring
    // the performance of this performance group for this iteration
    // of the event loop, `false` otherwise.
    bool isAcquired(uint64_t it, const AutoStopwatch* owner) const;

    // Mark that an instance of `AutoStopwatch` is monitoring
    // the performance of this group for a given iteration.
    void acquire(uint64_t it, const AutoStopwatch* owner);

    // Mark that no `AutoStopwatch` is monitoring the
    // performance of this group for the iteration.
    void release(uint64_t it, const AutoStopwatch* owner);

    // The number of cycles spent in this group during this iteration
    // of the event loop. Note that cycles are not a reliable measure,
    // especially over short intervals. See Stopwatch.* for a more
    // complete discussion on the imprecision of cycle measurement.
    uint64_t recentCycles(uint64_t iteration) const;
    void addRecentCycles(uint64_t iteration, uint64_t cycles);

    // The number of times this group has been activated during this
    // iteration of the event loop.
    uint64_t recentTicks(uint64_t iteration) const;
    void addRecentTicks(uint64_t iteration, uint64_t ticks);

    // The number of microseconds spent doing CPOW during this
    // iteration of the event loop.
    uint64_t recentCPOW(uint64_t iteration) const;
    void addRecentCPOW(uint64_t iteration, uint64_t CPOW);

    // Get rid of any data that pretends to be recent.
    void resetRecentData();

    // `true` if new measures should be added to this group, `false`
    // otherwise.
    bool isActive() const;
    void setIsActive(bool);

    // `true` if this group has been used in the current iteration,
    // `false` otherwise.
    bool isUsedInThisIteration() const;
    void setIsUsedInThisIteration(bool);
  protected:
    // An implementation of `delete` for this object. Must be provided
    // by the embedding.
    virtual void Delete() = 0;

  private:
    // The number of cycles spent in this group during this iteration
    // of the event loop. Note that cycles are not a reliable measure,
    // especially over short intervals. See Runtime.cpp for a more
    // complete discussion on the imprecision of cycle measurement.
    uint64_t recentCycles_;

    // The number of times this group has been activated during this
    // iteration of the event loop.
    uint64_t recentTicks_;

    // The number of microseconds spent doing CPOW during this
    // iteration of the event loop.
    uint64_t recentCPOW_;

    // The current iteration of the event loop. If necessary,
    // may safely overflow.
    uint64_t iteration_;

    // `true` if new measures should be added to this group, `false`
    // otherwise.
    bool isActive_;

    // `true` if this group has been used in the current iteration,
    // `false` otherwise.
    bool isUsedInThisIteration_;

    // The stopwatch currently monitoring the group,
    // or `nullptr` if none. Used ony for comparison.
    const AutoStopwatch* owner_;

  public:
    // Compatibility with RefPtr<>
    void AddRef();
    void Release();
    uint64_t refCount_;
};

using PerformanceGroupVector = mozilla::Vector<RefPtr<js::PerformanceGroup>, 8, SystemAllocPolicy>;

/**
 * Commit any Performance Monitoring data.
 *
 * Until `FlushMonitoring` has been called, all PerformanceMonitoring data is invisible
 * to the outside world and can cancelled with a call to `ResetMonitoring`.
 */
extern JS_PUBLIC_API(bool)
FlushPerformanceMonitoring(JSContext*);

/**
 * Cancel any measurement that hasn't been committed.
 */
extern JS_PUBLIC_API(void)
ResetPerformanceMonitoring(JSContext*);

/**
 * Cleanup any memory used by performance monitoring.
 */
extern JS_PUBLIC_API(void)
DisposePerformanceMonitoring(JSContext*);

/**
 * Turn on/off stopwatch-based CPU monitoring.
 *
 * `SetStopwatchIsMonitoringCPOW` or `SetStopwatchIsMonitoringJank`
 * may return `false` if monitoring could not be activated, which may
 * happen if we are out of memory.
 */
extern JS_PUBLIC_API(bool)
SetStopwatchIsMonitoringCPOW(JSContext*, bool);
extern JS_PUBLIC_API(bool)
GetStopwatchIsMonitoringCPOW(JSContext*);
extern JS_PUBLIC_API(bool)
SetStopwatchIsMonitoringJank(JSContext*, bool);
extern JS_PUBLIC_API(bool)
GetStopwatchIsMonitoringJank(JSContext*);

// Extract the CPU rescheduling data.
extern JS_PUBLIC_API(void)
GetPerfMonitoringTestCpuRescheduling(JSContext*, uint64_t* stayed, uint64_t* moved);


/**
 * Add a number of microseconds to the time spent waiting on CPOWs
 * since process start.
 */
extern JS_PUBLIC_API(void)
AddCPOWPerformanceDelta(JSContext*, uint64_t delta);

typedef bool
(*StopwatchStartCallback)(uint64_t, void*);
extern JS_PUBLIC_API(bool)
SetStopwatchStartCallback(JSContext*, StopwatchStartCallback, void*);

typedef bool
(*StopwatchCommitCallback)(uint64_t, PerformanceGroupVector&, void*);
extern JS_PUBLIC_API(bool)
SetStopwatchCommitCallback(JSContext*, StopwatchCommitCallback, void*);

typedef bool
(*GetGroupsCallback)(JSContext*, PerformanceGroupVector&, void*);
extern JS_PUBLIC_API(bool)
SetGetPerformanceGroupsCallback(JSContext*, GetGroupsCallback, void*);

/**
 * Hint that we expect a crash. Currently, the only thing that cares is the
 * breakpad injector, which (if loaded) will suppress minidump generation.
 */
extern JS_PUBLIC_API(void)
NoteIntentionalCrash();

} /* namespace js */

namespace js {

enum class CompletionKind {
    Normal,
    Return,
    Throw
};

} /* namespace js */

#endif /* jsapi_h */
