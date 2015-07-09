/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript API. */

#ifndef jsapi_h
#define jsapi_h

#include "mozilla/FloatingPoint.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Range.h"
#include "mozilla/RangedPtr.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "jsalloc.h"
#include "jspubtd.h"

#include "js/CallArgs.h"
#include "js/Class.h"
#include "js/HashTable.h"
#include "js/Id.h"
#include "js/Principals.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "js/Vector.h"

/************************************************************************/

namespace JS {

class Latin1CharsZ;
class TwoByteChars;

#ifdef JS_DEBUG

class JS_PUBLIC_API(AutoCheckRequestDepth)
{
    JSContext* cx;
  public:
    explicit AutoCheckRequestDepth(JSContext* cx);
    explicit AutoCheckRequestDepth(js::ContextFriendFields* cx);
    ~AutoCheckRequestDepth();
};

# define CHECK_REQUEST(cx) \
    JS::AutoCheckRequestDepth _autoCheckRequestDepth(cx)

#else

# define CHECK_REQUEST(cx) \
    ((void) 0)

#endif /* JS_DEBUG */

/* AutoValueArray roots an internal fixed-size array of Values. */
template <size_t N>
class AutoValueArray : public AutoGCRooter
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

template<class T>
class AutoVectorRooter : protected AutoGCRooter
{
    typedef js::Vector<T, 8> VectorImpl;
    VectorImpl vector;

  public:
    explicit AutoVectorRooter(JSContext* cx, ptrdiff_t tag
                              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, tag), vector(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    explicit AutoVectorRooter(js::ContextFriendFields* cx, ptrdiff_t tag
                              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, tag), vector(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    typedef T ElementType;
    typedef typename VectorImpl::Range Range;

    size_t length() const { return vector.length(); }
    bool empty() const { return vector.empty(); }

    bool append(const T& v) { return vector.append(v); }
    bool append(const T* ptr, size_t len) { return vector.append(ptr, len); }
    bool appendAll(const AutoVectorRooter<T>& other) {
        return vector.appendAll(other.vector);
    }

    bool insert(T* p, const T& val) { return vector.insert(p, val); }

    /* For use when space has already been reserved. */
    void infallibleAppend(const T& v) { vector.infallibleAppend(v); }

    void popBack() { vector.popBack(); }
    T popCopy() { return vector.popCopy(); }

    bool growBy(size_t inc) {
        size_t oldLength = vector.length();
        if (!vector.growByUninitialized(inc))
            return false;
        makeRangeGCSafe(oldLength);
        return true;
    }

    bool resize(size_t newLength) {
        size_t oldLength = vector.length();
        if (newLength <= oldLength) {
            vector.shrinkBy(oldLength - newLength);
            return true;
        }
        if (!vector.growByUninitialized(newLength - oldLength))
            return false;
        makeRangeGCSafe(oldLength);
        return true;
    }

    void clear() { vector.clear(); }

    bool reserve(size_t newLength) {
        return vector.reserve(newLength);
    }

    JS::MutableHandle<T> operator[](size_t i) {
        return JS::MutableHandle<T>::fromMarkedLocation(&vector[i]);
    }
    JS::Handle<T> operator[](size_t i) const {
        return JS::Handle<T>::fromMarkedLocation(&vector[i]);
    }

    const T* begin() const { return vector.begin(); }
    T* begin() { return vector.begin(); }

    const T* end() const { return vector.end(); }
    T* end() { return vector.end(); }

    Range all() { return vector.all(); }

    const T& back() const { return vector.back(); }

    friend void AutoGCRooter::trace(JSTracer* trc);

  private:
    void makeRangeGCSafe(size_t oldLength) {
        T* t = vector.begin() + oldLength;
        for (size_t i = oldLength; i < vector.length(); ++i, ++t)
            memset(t, 0, sizeof(T));
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

template<class Key, class Value>
class AutoHashMapRooter : protected AutoGCRooter
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

    uint32_t generation() const {
        return map.generation();
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
class AutoHashSetRooter : protected AutoGCRooter
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

    uint32_t generation() const {
        return set.generation();
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

class MOZ_STACK_CLASS AutoValueVector : public AutoVectorRooter<Value>
{
  public:
    explicit AutoValueVector(JSContext* cx
                             MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<Value>(cx, VALVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoIdVector : public AutoVectorRooter<jsid>
{
  public:
    explicit AutoIdVector(JSContext* cx
                          MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<jsid>(cx, IDVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoObjectVector : public AutoVectorRooter<JSObject*>
{
  public:
    explicit AutoObjectVector(JSContext* cx
                              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<JSObject*>(cx, OBJVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoFunctionVector : public AutoVectorRooter<JSFunction*>
{
  public:
    explicit AutoFunctionVector(JSContext* cx
                                MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<JSFunction*>(cx, FUNVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    explicit AutoFunctionVector(js::ContextFriendFields* cx
                                MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<JSFunction*>(cx, FUNVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoScriptVector : public AutoVectorRooter<JSScript*>
{
  public:
    explicit AutoScriptVector(JSContext* cx
                              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<JSScript*>(cx, SCRIPTVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * Custom rooting behavior for internal and external clients.
 */
class JS_PUBLIC_API(CustomAutoRooter) : private AutoGCRooter
{
  public:
    template <typename CX>
    explicit CustomAutoRooter(CX* cx MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, CUSTOM)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    friend void AutoGCRooter::trace(JSTracer* trc);

  protected:
    /* Supplied by derived class to trace roots. */
    virtual void trace(JSTracer* trc) = 0;

  private:
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * RootedGeneric<T> allows a class to instantiate its own Rooted type by
 * including the method:
 *
 *    void trace(JSTracer* trc);
 *
 * The trace() method must trace all of the class's fields.
 */
template <class T>
class RootedGeneric : private CustomAutoRooter
{
  public:
    template <typename CX>
    explicit RootedGeneric(CX* cx MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : CustomAutoRooter(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    template <typename CX>
    explicit RootedGeneric(CX* cx, const T& initial MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : CustomAutoRooter(cx), value(initial)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    operator const T&() const { return value; }
    T operator->() const { return value; }

  private:
    virtual void trace(JSTracer* trc) { value->trace(trc); }

    T value;

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/* A handle to an array of rooted values. */
class HandleValueArray
{
    const size_t length_;
    const Value * const elements_;

    HandleValueArray(size_t len, const Value* elements) : length_(len), elements_(elements) {}

  public:
    explicit HandleValueArray(const RootedValue& value) : length_(1), elements_(value.address()) {}

    MOZ_IMPLICIT HandleValueArray(const AutoValueVector& values)
      : length_(values.length()), elements_(values.begin()) {}

    template <size_t N>
    HandleValueArray(const AutoValueArray<N>& values) : length_(N), elements_(values.begin()) {}

    /* CallArgs must already be rooted somewhere up the stack. */
    MOZ_IMPLICIT HandleValueArray(const JS::CallArgs& args) : length_(args.length()), elements_(args.array()) {}

    /* Use with care! Only call this if the data is guaranteed to be marked. */
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
  private:
    JSRuntime*  runtime_;

  protected:
    explicit JSFreeOp(JSRuntime* rt)
      : runtime_(rt) { }

  public:
    JSRuntime* runtime() const {
        return runtime_;
    }
};

/* Callbacks and their arguments. */

/************************************************************************/

typedef enum JSContextOp {
    JSCONTEXT_NEW,
    JSCONTEXT_DESTROY
} JSContextOp;

/*
 * The possible values for contextOp when the runtime calls the callback are:
 *   JSCONTEXT_NEW      JS_NewContext successfully created a new JSContext
 *                      instance. The callback can initialize the instance as
 *                      required. If the callback returns false, the instance
 *                      will be destroyed and JS_NewContext returns null. In
 *                      this case the callback is not called again.
 *   JSCONTEXT_DESTROY  One of JS_DestroyContext* methods is called. The
 *                      callback may perform its own cleanup and must always
 *                      return true.
 *   Any other value    For future compatibility the callback must do nothing
 *                      and return true in this case.
 */
typedef bool
(* JSContextCallback)(JSContext* cx, unsigned contextOp, void* data);

typedef enum JSGCStatus {
    JSGC_BEGIN,
    JSGC_END
} JSGCStatus;

typedef void
(* JSGCCallback)(JSRuntime* rt, JSGCStatus status, void* data);

typedef enum JSFinalizeStatus {
    /*
     * Called when preparing to sweep a group of compartments, before anything
     * has been swept.  The collector will not yield to the mutator before
     * calling the callback with JSFINALIZE_GROUP_END status.
     */
    JSFINALIZE_GROUP_START,

    /*
     * Called when preparing to sweep a group of compartments. Weak references
     * to unmarked things have been removed and things that are not swept
     * incrementally have been finalized at this point.  The collector may yield
     * to the mutator after this point.
     */
    JSFINALIZE_GROUP_END,

    /*
     * Called at the end of collection when everything has been swept.
     */
    JSFINALIZE_COLLECTION_END
} JSFinalizeStatus;

typedef void
(* JSFinalizeCallback)(JSFreeOp* fop, JSFinalizeStatus status, bool isCompartment, void* data);

typedef void
(* JSWeakPointerCallback)(JSRuntime* rt, void* data);

typedef bool
(* JSInterruptCallback)(JSContext* cx);

typedef void
(* JSErrorReporter)(JSContext* cx, const char* message, JSErrorReport* report);

/*
 * Possible exception types. These types are part of a JSErrorFormatString
 * structure. They define which error to throw in case of a runtime error.
 * JSEXN_NONE marks an unthrowable error.
 */
typedef enum JSExnType {
    JSEXN_NONE = -1,
      JSEXN_ERR,
        JSEXN_INTERNALERR,
        JSEXN_EVALERR,
        JSEXN_RANGEERR,
        JSEXN_REFERENCEERR,
        JSEXN_SYNTAXERR,
        JSEXN_TYPEERR,
        JSEXN_URIERR,
        JSEXN_LIMIT
} JSExnType;

typedef struct JSErrorFormatString {
    /* The error format string in ASCII. */
    const char* format;

    /* The number of arguments to expand in the formatted error message. */
    uint16_t argCount;

    /* One of the JSExnType constants above. */
    int16_t exnType;
} JSErrorFormatString;

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

/*
 * Callback used to ask the embedding for the cross compartment wrapper handler
 * that implements the desired prolicy for this kind of object in the
 * destination compartment. |obj| is the object to be wrapped. If |existing| is
 * non-nullptr, it will point to an existing wrapper object that should be
 * re-used if possible. |existing| is guaranteed to be a cross-compartment
 * wrapper with a lazily-defined prototype and the correct global. It is
 * guaranteed not to wrap a function.
 */
typedef JSObject*
(* JSWrapObjectCallback)(JSContext* cx, JS::HandleObject existing, JS::HandleObject obj,
                         JS::HandleObject parent);

/*
 * Callback used by the wrap hook to ask the embedding to prepare an object
 * for wrapping in a context. This might include unwrapping other wrappers
 * or even finding a more suitable object for the new compartment.
 */
typedef JSObject*
(* JSPreWrapCallback)(JSContext* cx, JS::HandleObject scope, JS::HandleObject obj,
                      JS::HandleObject objectPassedToWrap);

struct JSWrapObjectCallbacks
{
    JSWrapObjectCallback wrap;
    JSPreWrapCallback preWrap;
};

typedef void
(* JSDestroyCompartmentCallback)(JSFreeOp* fop, JSCompartment* compartment);

typedef void
(* JSZoneCallback)(JS::Zone* zone);

typedef void
(* JSCompartmentNameCallback)(JSRuntime* rt, JSCompartment* compartment,
                              char* buf, size_t bufsize);

/************************************************************************/

static MOZ_ALWAYS_INLINE jsval
JS_NumberValue(double d)
{
    int32_t i;
    d = JS::CanonicalizeNaN(d);
    if (mozilla::NumberIsInt32(d, &i))
        return INT_TO_JSVAL(i);
    return DOUBLE_TO_JSVAL(d);
}

/************************************************************************/

JS_PUBLIC_API(bool)
JS_StringHasBeenInterned(JSContext* cx, JSString* str);

namespace JS {

// Container class for passing in script source buffers to the JS engine.  This
// not only groups the buffer and length values, it also provides a way to
// optionally pass ownership of the buffer to the JS engine without copying.
// Rules for use:
//
//  1) The data array must be allocated with js_malloc() or js_realloc() if
//     ownership is being granted to the SourceBufferHolder.
//  2) If ownership is not given to the SourceBufferHolder, then the memory
//     must be kept alive until the JS compilation is complete.
//  3) Any code calling SourceBufferHolder::take() must guarantee to keep the
//     memory alive until JS compilation completes.  Normally only the JS
//     engine should be calling take().
//
// Example use:
//
//    size_t length = 512;
//    char16_t* chars = static_cast<char16_t*>(js_malloc(sizeof(char16_t) * length));
//    JS::SourceBufferHolder srcBuf(chars, length, JS::SourceBufferHolder::GiveOwnership);
//    JS::Compile(cx, obj, options, srcBuf);
//
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

} /* namespace JS */

/************************************************************************/

/* Property attributes, set in JSPropertySpec and passed to API functions.
 *
 * NB: The data structure in which some of these values are stored only uses
 *     a uint8_t to store the relevant information. Proceed with caution if
 *     trying to reorder or change the the first byte worth of flags.
 */
#define JSPROP_ENUMERATE        0x01    /* property is visible to for/in loop */
#define JSPROP_READONLY         0x02    /* not settable: assignment is no-op.
                                           This flag is only valid when neither
                                           JSPROP_GETTER nor JSPROP_SETTER is
                                           set. */
#define JSPROP_PERMANENT        0x04    /* property cannot be deleted */
#define JSPROP_PROPOP_ACCESSORS 0x08    /* Passed to JS_Define(UC)Property* and
                                           JS_DefineElement if getters/setters
                                           are JSPropertyOp/JSStrictPropertyOp */
#define JSPROP_GETTER           0x10    /* property holds getter function */
#define JSPROP_SETTER           0x20    /* property holds setter function */
#define JSPROP_SHARED           0x40    /* don't allocate a value slot for this
                                           property; don't copy the property on
                                           set of the same-named property in an
                                           object that delegates to a prototype
                                           containing this property */
#define JSPROP_INDEX            0x80    /* name is actually (int) index */
#define JSPROP_DEFINE_LATE     0x100    /* Don't define property when initially creating
                                           the constructor. Some objects like Function/Object
                                           have self-hosted functions that can only be defined
                                           after the initialization is already finished. */
#define JSFUN_STUB_GSOPS       0x200    /* use JS_PropertyStub getter/setter
                                           instead of defaulting to class gsops
                                           for property holding function */

#define JSFUN_CONSTRUCTOR      0x400    /* native that can be called as a ctor */

#define JSPROP_REDEFINE_NONCONFIGURABLE 0x800 /* If set, will allow redefining a
                                                 non-configurable property, but
                                                 only on a non-DOM global.  This
                                                 is a temporary hack that will
                                                 need to go away in bug
                                                 1105518 */

#define JSPROP_IGNORE_ENUMERATE 0x1000  /* ignore the value in JSPROP_ENUMERATE.
                                           This flag only valid when defining over
                                           an existing property. */
#define JSPROP_IGNORE_READONLY  0x2000  /* ignore the value in JSPROP_READONLY.
                                           This flag only valid when defining over
                                           an existing property. */
#define JSPROP_IGNORE_PERMANENT 0x4000  /* ignore the value in JSPROP_PERMANENT.
                                           This flag only valid when defining over
                                           an existing property. */
#define JSPROP_IGNORE_VALUE     0x8000  /* ignore the Value in the descriptor. Nothing was
                                           specified when passed to Object.defineProperty
                                           from script. */

/*
 * Specify a generic native prototype methods, i.e., methods of a class
 * prototype that are exposed as static methods taking an extra leading
 * argument: the generic |this| parameter.
 *
 * If you set this flag in a JSFunctionSpec struct's flags initializer, then
 * that struct must live at least as long as the native static method object
 * created due to this flag by JS_DefineFunctions or JS_InitClass.  Typically
 * JSFunctionSpec structs are allocated in static arrays.
 */
#define JSFUN_GENERIC_NATIVE   0x800

#define JSFUN_FLAGS_MASK       0xe00    /* | of all the JSFUN_* flags */

/*
 * The first call to JS_CallOnce by any thread in a process will call 'func'.
 * Later calls to JS_CallOnce with the same JSCallOnceType object will be
 * suppressed.
 *
 * Equivalently: each distinct JSCallOnceType object will allow one JS_CallOnce
 * to invoke its JSInitCallback.
 */
extern JS_PUBLIC_API(bool)
JS_CallOnce(JSCallOnceType* once, JSInitCallback func);

/* Microseconds since the epoch, midnight, January 1, 1970 UTC. */
extern JS_PUBLIC_API(int64_t)
JS_Now(void);

/* Don't want to export data, so provide accessors for non-inline jsvals. */
extern JS_PUBLIC_API(jsval)
JS_GetNaNValue(JSContext* cx);

extern JS_PUBLIC_API(jsval)
JS_GetNegativeInfinityValue(JSContext* cx);

extern JS_PUBLIC_API(jsval)
JS_GetPositiveInfinityValue(JSContext* cx);

extern JS_PUBLIC_API(jsval)
JS_GetEmptyStringValue(JSContext* cx);

extern JS_PUBLIC_API(JSString*)
JS_GetEmptyString(JSRuntime* rt);

struct CompartmentTimeStats {
    char compartmentName[1024];
    JSAddonId* addonId;
    JSCompartment* compartment;
    uint64_t time;  // microseconds
    uint64_t cpowTime; // microseconds
};

typedef js::Vector<CompartmentTimeStats, 0, js::SystemAllocPolicy> CompartmentStatsVector;

extern JS_PUBLIC_API(bool)
JS_GetCompartmentStats(JSRuntime* rt, CompartmentStatsVector& stats);

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

extern JS_PUBLIC_API(bool)
JS_StrictlyEqual(JSContext* cx, JS::Handle<JS::Value> v1, JS::Handle<JS::Value> v2, bool* equal);

extern JS_PUBLIC_API(bool)
JS_LooselyEqual(JSContext* cx, JS::Handle<JS::Value> v1, JS::Handle<JS::Value> v2, bool* equal);

extern JS_PUBLIC_API(bool)
JS_SameValue(JSContext* cx, JS::Handle<JS::Value> v1, JS::Handle<JS::Value> v2, bool* same);

/* True iff fun is the global eval function. */
extern JS_PUBLIC_API(bool)
JS_IsBuiltinEvalFunction(JSFunction* fun);

/* True iff fun is the Function constructor. */
extern JS_PUBLIC_API(bool)
JS_IsBuiltinFunctionConstructor(JSFunction* fun);

/************************************************************************/

/*
 * Initialization, locking, contexts, and memory allocation.
 *
 * It is important that the first runtime and first context be created in a
 * single-threaded fashion, otherwise the behavior of the library is undefined.
 * See: http://developer.mozilla.org/en/docs/Category:JSAPI_Reference
 */

/**
 * Initialize SpiderMonkey, returning true only if initialization succeeded.
 * Once this method has succeeded, it is safe to call JS_NewRuntime and other
 * JSAPI methods.
 *
 * This method must be called before any other JSAPI method is used on any
 * thread.  Once it has been used, it is safe to call any JSAPI method, and it
 * remains safe to do so until JS_ShutDown is correctly called.
 *
 * It is currently not possible to initialize SpiderMonkey multiple times (that
 * is, calling JS_Init/JSAPI methods/JS_ShutDown in that order, then doing so
 * again).  This restriction may eventually be lifted.
 */
extern JS_PUBLIC_API(bool)
JS_Init(void);

/**
 * Destroy free-standing resources allocated by SpiderMonkey, not associated
 * with any runtime, context, or other structure.
 *
 * This method should be called after all other JSAPI data has been properly
 * cleaned up: every new runtime must have been destroyed, every new context
 * must have been destroyed, and so on.  Calling this method before all other
 * resources have been destroyed has undefined behavior.
 *
 * Failure to call this method, at present, has no adverse effects other than
 * leaking memory.  This may not always be the case; it's recommended that all
 * embedders call this method when all other JSAPI operations have completed.
 *
 * It is currently not possible to initialize SpiderMonkey multiple times (that
 * is, calling JS_Init/JSAPI methods/JS_ShutDown in that order, then doing so
 * again).  This restriction may eventually be lifted.
 */
extern JS_PUBLIC_API(void)
JS_ShutDown(void);

extern JS_PUBLIC_API(JSRuntime*)
JS_NewRuntime(uint32_t maxbytes,
              uint32_t maxNurseryBytes = JS::DefaultNurseryBytes,
              JSRuntime* parentRuntime = nullptr);

extern JS_PUBLIC_API(void)
JS_DestroyRuntime(JSRuntime* rt);

// These are equivalent to ICU's |UMemAllocFn|, |UMemReallocFn|, and
// |UMemFreeFn| types.  The first argument (called |context| in the ICU docs)
// will always be nullptr, and should be ignored.
typedef void* (*JS_ICUAllocFn)(const void*, size_t size);
typedef void* (*JS_ICUReallocFn)(const void*, void* p, size_t size);
typedef void (*JS_ICUFreeFn)(const void*, void* p);

// This function can be used to track memory used by ICU.
// Do not use it unless you know what you are doing!
extern JS_PUBLIC_API(bool)
JS_SetICUMemoryFunctions(JS_ICUAllocFn allocFn, JS_ICUReallocFn reallocFn, JS_ICUFreeFn freeFn);

JS_PUBLIC_API(void*)
JS_GetRuntimePrivate(JSRuntime* rt);

extern JS_PUBLIC_API(JSRuntime*)
JS_GetRuntime(JSContext* cx);

extern JS_PUBLIC_API(JSRuntime*)
JS_GetParentRuntime(JSContext* cx);

JS_PUBLIC_API(void)
JS_SetRuntimePrivate(JSRuntime* rt, void* data);

extern JS_PUBLIC_API(void)
JS_BeginRequest(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_EndRequest(JSContext* cx);

namespace js {

void
AssertHeapIsIdle(JSRuntime* rt);

void
AssertHeapIsIdle(JSContext* cx);

} /* namespace js */

class JSAutoRequest
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

extern JS_PUBLIC_API(void)
JS_SetContextCallback(JSRuntime* rt, JSContextCallback cxCallback, void* data);

extern JS_PUBLIC_API(JSContext*)
JS_NewContext(JSRuntime* rt, size_t stackChunkSize);

extern JS_PUBLIC_API(void)
JS_DestroyContext(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_DestroyContextNoGC(JSContext* cx);

extern JS_PUBLIC_API(void*)
JS_GetContextPrivate(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_SetContextPrivate(JSContext* cx, void* data);

extern JS_PUBLIC_API(void*)
JS_GetSecondContextPrivate(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_SetSecondContextPrivate(JSContext* cx, void* data);

extern JS_PUBLIC_API(JSRuntime*)
JS_GetRuntime(JSContext* cx);

extern JS_PUBLIC_API(JSContext*)
JS_ContextIterator(JSRuntime* rt, JSContext** iterp);

extern JS_PUBLIC_API(JSVersion)
JS_GetVersion(JSContext* cx);

// Mutate the version on the compartment. This is generally discouraged, but
// necessary to support the version mutation in the js and xpc shell command
// set.
//
// It would be nice to put this in jsfriendapi, but the linkage requirements
// of the shells make that impossible.
JS_PUBLIC_API(void)
JS_SetVersionForCompartment(JSCompartment* compartment, JSVersion version);

extern JS_PUBLIC_API(const char*)
JS_VersionToString(JSVersion version);

extern JS_PUBLIC_API(JSVersion)
JS_StringToVersion(const char* string);

namespace JS {

class JS_PUBLIC_API(RuntimeOptions) {
  public:
    RuntimeOptions()
      : baseline_(false),
        ion_(false),
        asmJS_(false),
        nativeRegExp_(false),
        unboxedObjects_(false),
        werror_(false),
        strictMode_(false),
        extraWarnings_(false),
        varObjFix_(false)
    {
    }

    bool baseline() const { return baseline_; }
    RuntimeOptions& setBaseline(bool flag) {
        baseline_ = flag;
        return *this;
    }
    RuntimeOptions& toggleBaseline() {
        baseline_ = !baseline_;
        return *this;
    }

    bool ion() const { return ion_; }
    RuntimeOptions& setIon(bool flag) {
        ion_ = flag;
        return *this;
    }
    RuntimeOptions& toggleIon() {
        ion_ = !ion_;
        return *this;
    }

    bool asmJS() const { return asmJS_; }
    RuntimeOptions& setAsmJS(bool flag) {
        asmJS_ = flag;
        return *this;
    }
    RuntimeOptions& toggleAsmJS() {
        asmJS_ = !asmJS_;
        return *this;
    }

    bool nativeRegExp() const { return nativeRegExp_; }
    RuntimeOptions& setNativeRegExp(bool flag) {
        nativeRegExp_ = flag;
        return *this;
    }

    bool unboxedObjects() const { return unboxedObjects_; }
    RuntimeOptions& setUnboxedObjects(bool flag) {
        unboxedObjects_ = flag;
        return *this;
    }

    bool werror() const { return werror_; }
    RuntimeOptions& setWerror(bool flag) {
        werror_ = flag;
        return *this;
    }
    RuntimeOptions& toggleWerror() {
        werror_ = !werror_;
        return *this;
    }

    bool strictMode() const { return strictMode_; }
    RuntimeOptions& setStrictMode(bool flag) {
        strictMode_ = flag;
        return *this;
    }
    RuntimeOptions& toggleStrictMode() {
        strictMode_ = !strictMode_;
        return *this;
    }

    bool extraWarnings() const { return extraWarnings_; }
    RuntimeOptions& setExtraWarnings(bool flag) {
        extraWarnings_ = flag;
        return *this;
    }
    RuntimeOptions& toggleExtraWarnings() {
        extraWarnings_ = !extraWarnings_;
        return *this;
    }

    bool varObjFix() const { return varObjFix_; }
    RuntimeOptions& setVarObjFix(bool flag) {
        varObjFix_ = flag;
        return *this;
    }
    RuntimeOptions& toggleVarObjFix() {
        varObjFix_ = !varObjFix_;
        return *this;
    }

  private:
    bool baseline_ : 1;
    bool ion_ : 1;
    bool asmJS_ : 1;
    bool nativeRegExp_ : 1;
    bool unboxedObjects_ : 1;
    bool werror_ : 1;
    bool strictMode_ : 1;
    bool extraWarnings_ : 1;
    bool varObjFix_ : 1;
};

JS_PUBLIC_API(RuntimeOptions&)
RuntimeOptionsRef(JSRuntime* rt);

JS_PUBLIC_API(RuntimeOptions&)
RuntimeOptionsRef(JSContext* cx);

class JS_PUBLIC_API(ContextOptions) {
  public:
    ContextOptions()
      : privateIsNSISupports_(false),
        dontReportUncaught_(false),
        autoJSAPIOwnsErrorReporting_(false)
    {
    }

    bool privateIsNSISupports() const { return privateIsNSISupports_; }
    ContextOptions& setPrivateIsNSISupports(bool flag) {
        privateIsNSISupports_ = flag;
        return *this;
    }
    ContextOptions& togglePrivateIsNSISupports() {
        privateIsNSISupports_ = !privateIsNSISupports_;
        return *this;
    }

    bool dontReportUncaught() const { return dontReportUncaught_; }
    ContextOptions& setDontReportUncaught(bool flag) {
        dontReportUncaught_ = flag;
        return *this;
    }
    ContextOptions& toggleDontReportUncaught() {
        dontReportUncaught_ = !dontReportUncaught_;
        return *this;
    }

    bool autoJSAPIOwnsErrorReporting() const { return autoJSAPIOwnsErrorReporting_; }
    ContextOptions& setAutoJSAPIOwnsErrorReporting(bool flag) {
        autoJSAPIOwnsErrorReporting_ = flag;
        return *this;
    }
    ContextOptions& toggleAutoJSAPIOwnsErrorReporting() {
        autoJSAPIOwnsErrorReporting_ = !autoJSAPIOwnsErrorReporting_;
        return *this;
    }


  private:
    bool privateIsNSISupports_ : 1;
    bool dontReportUncaught_ : 1;
    // dontReportUncaught isn't respected by all JSAPI codepaths, particularly the
    // JS_ReportError* functions that eventually report the error even when dontReportUncaught is
    // set, if script is not running. We want a way to indicate that the embedder will always
    // handle any exceptions, and that SpiderMonkey should just leave them on the context. This is
    // the way we want to do all future error handling in Gecko - stealing the exception explicitly
    // from the context and handling it as per the situation. This will eventually become the
    // default and these 2 flags should go away.
    bool autoJSAPIOwnsErrorReporting_ : 1;
};

JS_PUBLIC_API(ContextOptions&)
ContextOptionsRef(JSContext* cx);

class JS_PUBLIC_API(AutoSaveContextOptions) {
  public:
    explicit AutoSaveContextOptions(JSContext* cx)
      : cx_(cx),
        oldOptions_(ContextOptionsRef(cx_))
    {
    }

    ~AutoSaveContextOptions()
    {
        ContextOptionsRef(cx_) = oldOptions_;
    }

  private:
    JSContext* cx_;
    JS::ContextOptions oldOptions_;
};

} /* namespace JS */

extern JS_PUBLIC_API(const char*)
JS_GetImplementationVersion(void);

extern JS_PUBLIC_API(void)
JS_SetDestroyCompartmentCallback(JSRuntime* rt, JSDestroyCompartmentCallback callback);

extern JS_PUBLIC_API(void)
JS_SetDestroyZoneCallback(JSRuntime* rt, JSZoneCallback callback);

extern JS_PUBLIC_API(void)
JS_SetSweepZoneCallback(JSRuntime* rt, JSZoneCallback callback);

extern JS_PUBLIC_API(void)
JS_SetCompartmentNameCallback(JSRuntime* rt, JSCompartmentNameCallback callback);

extern JS_PUBLIC_API(void)
JS_SetWrapObjectCallbacks(JSRuntime* rt, const JSWrapObjectCallbacks* callbacks);

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
 */

class JS_PUBLIC_API(JSAutoCompartment)
{
    JSContext* cx_;
    JSCompartment* oldCompartment_;
  public:
    JSAutoCompartment(JSContext* cx, JSObject* target);
    JSAutoCompartment(JSContext* cx, JSScript* target);
    ~JSAutoCompartment();
};

class JS_PUBLIC_API(JSAutoNullableCompartment)
{
    JSContext* cx_;
    JSCompartment* oldCompartment_;
  public:
    explicit JSAutoNullableCompartment(JSContext* cx, JSObject* targetOrNull);
    ~JSAutoNullableCompartment();
};

/* NB: This API is infallible; a nullptr return value does not indicate error. */
extern JS_PUBLIC_API(JSCompartment*)
JS_EnterCompartment(JSContext* cx, JSObject* target);

extern JS_PUBLIC_API(void)
JS_LeaveCompartment(JSContext* cx, JSCompartment* oldCompartment);

typedef void (*JSIterateCompartmentCallback)(JSRuntime* rt, void* data, JSCompartment* compartment);

/*
 * This function calls |compartmentCallback| on every compartment. Beware that
 * there is no guarantee that the compartment will survive after the callback
 * returns. Also, if the callback can GC, there is no guarantee that every
 * compartment will be visited.
 */
extern JS_PUBLIC_API(void)
JS_IterateCompartments(JSRuntime* rt, void* data,
                       JSIterateCompartmentCallback compartmentCallback);

/*
 * Initialize standard JS class constructors, prototypes, and any top-level
 * functions and constants associated with the standard classes (e.g. isNaN
 * for Number).
 *
 * NB: This sets cx's global object to obj if it was null.
 */
extern JS_PUBLIC_API(bool)
JS_InitStandardClasses(JSContext* cx, JS::Handle<JSObject*> obj);

/*
 * Resolve id, which must contain either a string or an int, to a standard
 * class name in obj if possible, defining the class's constructor and/or
 * prototype and storing true in *resolved.  If id does not name a standard
 * class or a top-level property induced by initializing a standard class,
 * store false in *resolved and just return true.  Return false on error,
 * as usual for bool result-typed API entry points.
 *
 * This API can be called directly from a global object class's resolve op,
 * to define standard classes lazily.  The class's enumerate op should call
 * JS_EnumerateStandardClasses(cx, obj), to define eagerly during for..in
 * loops any classes not yet resolved lazily.
 */
extern JS_PUBLIC_API(bool)
JS_ResolveStandardClass(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolved);

extern JS_PUBLIC_API(bool)
JS_EnumerateStandardClasses(JSContext* cx, JS::HandleObject obj);

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

/*
 * Returns the original value of |Function.prototype| from the global object in
 * which |forObj| was created.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetFunctionPrototype(JSContext* cx, JS::HandleObject forObj);

/*
 * Returns the original value of |Object.prototype| from the global object in
 * which |forObj| was created.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetObjectPrototype(JSContext* cx, JS::HandleObject forObj);

/*
 * Returns the original value of |Array.prototype| from the global object in
 * which |forObj| was created.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetArrayPrototype(JSContext* cx, JS::HandleObject forObj);

/*
 * Returns the original value of |Error.prototype| from the global
 * object of the current compartment of cx.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetErrorPrototype(JSContext* cx);

extern JS_PUBLIC_API(JSObject*)
JS_GetGlobalForObject(JSContext* cx, JSObject* obj);

extern JS_PUBLIC_API(bool)
JS_IsGlobalObject(JSObject* obj);

/*
 * May return nullptr, if |c| never had a global (e.g. the atoms compartment),
 * or if |c|'s global has been collected.
 */
extern JS_PUBLIC_API(JSObject*)
JS_GetGlobalForCompartmentOrNull(JSContext* cx, JSCompartment* c);

namespace JS {

extern JS_PUBLIC_API(JSObject*)
CurrentGlobalOrNull(JSContext* cx);

}

/*
 * Initialize the 'Reflect' object on a global object.
 */
extern JS_PUBLIC_API(JSObject*)
JS_InitReflect(JSContext* cx, JS::HandleObject global);

/*
 * Add various profiling-related functions as properties of the given object.
 * Defined in builtin/Profilers.cpp.
 */
extern JS_PUBLIC_API(bool)
JS_DefineProfilingFunctions(JSContext* cx, JS::HandleObject obj);

/* Defined in vm/Debugger.cpp. */
extern JS_PUBLIC_API(bool)
JS_DefineDebuggerObject(JSContext* cx, JS::HandleObject obj);

#ifdef JS_HAS_CTYPES
/*
 * Initialize the 'ctypes' object on a global variable 'obj'. The 'ctypes'
 * object will be sealed.
 */
extern JS_PUBLIC_API(bool)
JS_InitCTypesClass(JSContext* cx, JS::HandleObject global);

/*
 * Convert a unicode string 'source' of length 'slen' to the platform native
 * charset, returning a null-terminated string allocated with JS_malloc. On
 * failure, this function should report an error.
 */
typedef char*
(* JSCTypesUnicodeToNativeFun)(JSContext* cx, const char16_t* source, size_t slen);

/*
 * Set of function pointers that ctypes can use for various internal functions.
 * See JS_SetCTypesCallbacks below. Providing nullptr for a function is safe,
 * and will result in the applicable ctypes functionality not being available.
 */
struct JSCTypesCallbacks {
    JSCTypesUnicodeToNativeFun unicodeToNative;
};

typedef struct JSCTypesCallbacks JSCTypesCallbacks;

/*
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

/*
 * A wrapper for js_free(p) that may delay js_free(p) invocation as a
 * performance optimization.
 * cx may be nullptr.
 */
extern JS_PUBLIC_API(void)
JS_free(JSContext* cx, void* p);

/*
 * A wrapper for js_free(p) that may delay js_free(p) invocation as a
 * performance optimization as specified by the given JSFreeOp instance.
 */
extern JS_PUBLIC_API(void)
JS_freeop(JSFreeOp* fop, void* p);

extern JS_PUBLIC_API(JSFreeOp*)
JS_GetDefaultFreeOp(JSRuntime* rt);

extern JS_PUBLIC_API(void)
JS_updateMallocCounter(JSContext* cx, size_t nbytes);

extern JS_PUBLIC_API(char*)
JS_strdup(JSContext* cx, const char* s);

/* Duplicate a string.  Does not report an error on failure. */
extern JS_PUBLIC_API(char*)
JS_strdup(JSRuntime* rt, const char* s);

/*
 * Register externally maintained GC roots.
 *
 * traceOp: the trace operation. For each root the implementation should call
 *          JS_CallTracer whenever the root contains a traceable thing.
 * data:    the data argument to pass to each invocation of traceOp.
 */
extern JS_PUBLIC_API(bool)
JS_AddExtraGCRootsTracer(JSRuntime* rt, JSTraceDataOp traceOp, void* data);

/* Undo a call to JS_AddExtraGCRootsTracer. */
extern JS_PUBLIC_API(void)
JS_RemoveExtraGCRootsTracer(JSRuntime* rt, JSTraceDataOp traceOp, void* data);

/*
 * Garbage collector API.
 */
extern JS_PUBLIC_API(void)
JS_GC(JSRuntime* rt);

extern JS_PUBLIC_API(void)
JS_MaybeGC(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_SetGCCallback(JSRuntime* rt, JSGCCallback cb, void* data);

extern JS_PUBLIC_API(bool)
JS_AddFinalizeCallback(JSRuntime* rt, JSFinalizeCallback cb, void* data);

extern JS_PUBLIC_API(void)
JS_RemoveFinalizeCallback(JSRuntime* rt, JSFinalizeCallback cb);

extern JS_PUBLIC_API(bool)
JS_IsGCMarkingTracer(JSTracer* trc);

/* For assertions only. */
#ifdef JS_DEBUG
extern JS_PUBLIC_API(bool)
JS_IsMarkingGray(JSTracer* trc);
#endif

/*
 * Weak pointers and garbage collection
 *
 * Weak pointers are by their nature not marked as part of garbage collection,
 * but they may need to be updated in two cases after a GC:
 *
 *  1) Their referent was found not to be live and is about to be finalized
 *  2) Their referent has been moved by a compacting GC
 *
 * To handle this, any part of the system that maintain weak pointers to
 * JavaScript GC things must register a callback with
 * JS_(Add,Remove)WeakPointerCallback().  This callback must then call
 * JS_UpdateWeakPointerAfterGC() on all weak pointers it knows about.
 *
 * The argument to JS_UpdateWeakPointerAfterGC() is an in-out param.  If the
 * referent is about to be finalized the pointer will be set to null.  If the
 * referent has been moved then the pointer will be updated to point to the new
 * location.
 *
 * Callers of this method are responsible for updating any state that is
 * dependent on the object's address. For example, if the object's address is
 * used as a key in a hashtable, then the object must be removed and
 * re-inserted with the correct hash.
 */

extern JS_PUBLIC_API(bool)
JS_AddWeakPointerCallback(JSRuntime* rt, JSWeakPointerCallback cb, void* data);

extern JS_PUBLIC_API(void)
JS_RemoveWeakPointerCallback(JSRuntime* rt, JSWeakPointerCallback cb);

extern JS_PUBLIC_API(void)
JS_UpdateWeakPointerAfterGC(JS::Heap<JSObject*>* objp);

extern JS_PUBLIC_API(void)
JS_UpdateWeakPointerAfterGCUnbarriered(JSObject** objp);

typedef enum JSGCParamKey {
    /* Maximum nominal heap before last ditch GC. */
    JSGC_MAX_BYTES          = 0,

    /* Number of JS_malloc bytes before last ditch GC. */
    JSGC_MAX_MALLOC_BYTES   = 1,

    /* Amount of bytes allocated by the GC. */
    JSGC_BYTES = 3,

    /* Number of times GC has been invoked. Includes both major and minor GC. */
    JSGC_NUMBER = 4,

    /* Max size of the code cache in bytes. */
    JSGC_MAX_CODE_CACHE_BYTES = 5,

    /* Select GC mode. */
    JSGC_MODE = 6,

    /* Number of cached empty GC chunks. */
    JSGC_UNUSED_CHUNKS = 7,

    /* Total number of allocated GC chunks. */
    JSGC_TOTAL_CHUNKS = 8,

    /* Max milliseconds to spend in an incremental GC slice. */
    JSGC_SLICE_TIME_BUDGET = 9,

    /* Maximum size the GC mark stack can grow to. */
    JSGC_MARK_STACK_LIMIT = 10,

    /*
     * GCs less than this far apart in time will be considered 'high-frequency GCs'.
     * See setGCLastBytes in jsgc.cpp.
     */
    JSGC_HIGH_FREQUENCY_TIME_LIMIT = 11,

    /* Start of dynamic heap growth. */
    JSGC_HIGH_FREQUENCY_LOW_LIMIT = 12,

    /* End of dynamic heap growth. */
    JSGC_HIGH_FREQUENCY_HIGH_LIMIT = 13,

    /* Upper bound of heap growth. */
    JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MAX = 14,

    /* Lower bound of heap growth. */
    JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MIN = 15,

    /* Heap growth for low frequency GCs. */
    JSGC_LOW_FREQUENCY_HEAP_GROWTH = 16,

    /*
     * If false, the heap growth factor is fixed at 3. If true, it is determined
     * based on whether GCs are high- or low- frequency.
     */
    JSGC_DYNAMIC_HEAP_GROWTH = 17,

    /* If true, high-frequency GCs will use a longer mark slice. */
    JSGC_DYNAMIC_MARK_SLICE = 18,

    /* Lower limit after which we limit the heap growth. */
    JSGC_ALLOCATION_THRESHOLD = 19,

    /*
     * We decommit memory lazily. If more than this number of megabytes is
     * available to be decommitted, then JS_MaybeGC will trigger a shrinking GC
     * to decommit it.
     */
    JSGC_DECOMMIT_THRESHOLD = 20,

    /*
     * We try to keep at least this many unused chunks in the free chunk pool at
     * all times, even after a shrinking GC.
     */
    JSGC_MIN_EMPTY_CHUNK_COUNT = 21,

    /* We never keep more than this many unused chunks in the free chunk pool. */
    JSGC_MAX_EMPTY_CHUNK_COUNT = 22,

    /* Whether compacting GC is enabled. */
    JSGC_COMPACTING_ENABLED = 23
} JSGCParamKey;

extern JS_PUBLIC_API(void)
JS_SetGCParameter(JSRuntime* rt, JSGCParamKey key, uint32_t value);

extern JS_PUBLIC_API(uint32_t)
JS_GetGCParameter(JSRuntime* rt, JSGCParamKey key);

extern JS_PUBLIC_API(void)
JS_SetGCParameterForThread(JSContext* cx, JSGCParamKey key, uint32_t value);

extern JS_PUBLIC_API(uint32_t)
JS_GetGCParameterForThread(JSContext* cx, JSGCParamKey key);

extern JS_PUBLIC_API(void)
JS_SetGCParametersBasedOnAvailableMemory(JSRuntime* rt, uint32_t availMem);

/*
 * Create a new JSString whose chars member refers to external memory, i.e.,
 * memory requiring application-specific finalization.
 */
extern JS_PUBLIC_API(JSString*)
JS_NewExternalString(JSContext* cx, const char16_t* chars, size_t length,
                     const JSStringFinalizer* fin);

/*
 * Return whether 'str' was created with JS_NewExternalString or
 * JS_NewExternalStringWithClosure.
 */
extern JS_PUBLIC_API(bool)
JS_IsExternalString(JSString* str);

/*
 * Return the 'fin' arg passed to JS_NewExternalString.
 */
extern JS_PUBLIC_API(const JSStringFinalizer*)
JS_GetExternalStringFinalizer(JSString* str);

/*
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
JS_SetNativeStackQuota(JSRuntime* cx, size_t systemCodeStackSize,
                       size_t trustedScriptStackSize = 0,
                       size_t untrustedScriptStackSize = 0);

/************************************************************************/

extern JS_PUBLIC_API(int)
JS_IdArrayLength(JSContext* cx, JSIdArray* ida);

extern JS_PUBLIC_API(jsid)
JS_IdArrayGet(JSContext* cx, JSIdArray* ida, unsigned index);

extern JS_PUBLIC_API(void)
JS_DestroyIdArray(JSContext* cx, JSIdArray* ida);

namespace JS {

class AutoIdArray : private AutoGCRooter
{
  public:
    AutoIdArray(JSContext* cx, JSIdArray* ida
                MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, IDARRAY), context(cx), idArray(ida)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }
    ~AutoIdArray() {
        if (idArray)
            JS_DestroyIdArray(context, idArray);
    }
    bool operator!() const {
        return !idArray;
    }
    jsid operator[](size_t i) const {
        MOZ_ASSERT(idArray);
        return JS_IdArrayGet(context, idArray, unsigned(i));
    }
    size_t length() const {
        return JS_IdArrayLength(context, idArray);
    }

    friend void AutoGCRooter::trace(JSTracer* trc);

    JSIdArray* steal() {
        JSIdArray* copy = idArray;
        idArray = nullptr;
        return copy;
    }

  protected:
    inline void trace(JSTracer* trc);

  private:
    JSContext* context;
    JSIdArray* idArray;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    /* No copy or assignment semantics. */
    AutoIdArray(AutoIdArray& ida) = delete;
    void operator=(AutoIdArray& ida) = delete;
};

} /* namespace JS */

extern JS_PUBLIC_API(bool)
JS_ValueToId(JSContext* cx, JS::HandleValue v, JS::MutableHandleId idp);

extern JS_PUBLIC_API(bool)
JS_StringToId(JSContext* cx, JS::HandleString s, JS::MutableHandleId idp);

extern JS_PUBLIC_API(bool)
JS_IdToValue(JSContext* cx, jsid id, JS::MutableHandle<JS::Value> vp);

/*
 * Invoke the [[DefaultValue]] hook (see ES5 8.6.2) with the provided hint on
 * the specified object, computing a primitive default value for the object.
 * The hint must be JSTYPE_STRING, JSTYPE_NUMBER, or JSTYPE_VOID (no hint).  On
 * success the resulting value is stored in *vp.
 */
extern JS_PUBLIC_API(bool)
JS_DefaultValue(JSContext* cx, JS::Handle<JSObject*> obj, JSType hint,
                JS::MutableHandle<JS::Value> vp);

extern JS_PUBLIC_API(bool)
JS_PropertyStub(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_StrictPropertyStub(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool strict,
                      JS::MutableHandleValue vp);

template<typename T>
struct JSConstScalarSpec {
    const char* name;
    T val;
};

typedef JSConstScalarSpec<double> JSConstDoubleSpec;
typedef JSConstScalarSpec<int32_t> JSConstIntegerSpec;

struct JSJitInfo;

/*
 * Wrapper to relace JSNative for JSPropertySpecs and JSFunctionSpecs. This will
 * allow us to pass one JSJitInfo per function with the property/function spec,
 * without additional field overhead.
 */
typedef struct JSNativeWrapper {
    JSNative        op;
    const JSJitInfo* info;
} JSNativeWrapper;

/*
 * Macro static initializers which make it easy to pass no JSJitInfo as part of a
 * JSPropertySpec or JSFunctionSpec.
 */
#define JSNATIVE_WRAPPER(native) { {native, nullptr} }

/*
 * To define an array element rather than a named property member, cast the
 * element's index to (const char*) and initialize name with it, and set the
 * JSPROP_INDEX bit in flags.
 */
struct JSPropertySpec {
    struct SelfHostedWrapper {
        void*      unused;
        const char* funname;
    };

    const char*                 name;
    uint8_t                     flags;
    union {
        JSNativeWrapper     native;
        SelfHostedWrapper   selfHosted;
    } getter;
    union {
        JSNativeWrapper           native;
        SelfHostedWrapper         selfHosted;
    } setter;

    bool isSelfHosted() const {
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
        MOZ_ASSERT(getter.native.op);
        // We may not have a setter at all.  So all we can assert here, for the
        // native case is that if we have a jitinfo for the setter then we have
        // a setter op too.  This is good enough to make sure we don't have a
        // SelfHostedWrapper for the setter.
        MOZ_ASSERT_IF(setter.native.info, setter.native.op);
    }

    void checkAccessorsAreSelfHosted() const {
        MOZ_ASSERT(!getter.selfHosted.unused);
        MOZ_ASSERT(!setter.selfHosted.unused);
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

/* NEVER DEFINED, DON'T USE.  For use by JS_PROPERTYOP_GETTER only. */
inline int CheckIsPropertyOp(JSPropertyOp op);

/* NEVER DEFINED, DON'T USE.  For use by JS_PROPERTYOP_SETTER only. */
inline int CheckIsStrictPropertyOp(JSStrictPropertyOp op);


} // namespace detail
} // namespace JS

#define JS_CAST_NATIVE_TO(v, To) \
  (static_cast<void>(sizeof(JS::detail::CheckIsNative(v))), \
   reinterpret_cast<To>(v))

#define JS_CAST_STRING_TO(s, To) \
  (static_cast<void>(sizeof(JS::detail::CheckIsCharacterLiteral(s))), \
   reinterpret_cast<To>(s))

#define JS_CHECK_ACCESSOR_FLAGS(flags) \
  (static_cast<mozilla::EnableIf<!((flags) & (JSPROP_READONLY | JSPROP_SHARED | JSPROP_PROPOP_ACCESSORS))>::Type>(0), \
   (flags))

#define JS_PROPERTYOP_GETTER(v) \
  (static_cast<void>(sizeof(JS::detail::CheckIsPropertyOp(v))), \
   reinterpret_cast<JSNative>(v))

#define JS_PROPERTYOP_SETTER(v) \
  (static_cast<void>(sizeof(JS::detail::CheckIsStrictPropertyOp(v))), \
   reinterpret_cast<JSNative>(v))

#define JS_STUBGETTER JS_PROPERTYOP_GETTER(JS_PropertyStub)

#define JS_STUBSETTER JS_PROPERTYOP_SETTER(JS_StrictPropertyStub)

/*
 * JSPropertySpec uses JSNativeWrapper.  These macros encapsulate the definition
 * of JSNative-backed JSPropertySpecs, by defining the JSNativeWrappers for
 * them.
 */
#define JS_PSG(name, getter, flags) \
    {name, \
     uint8_t(JS_CHECK_ACCESSOR_FLAGS(flags) | JSPROP_SHARED), \
     JSNATIVE_WRAPPER(getter), \
     JSNATIVE_WRAPPER(nullptr)}
#define JS_PSGS(name, getter, setter, flags) \
    {name, \
     uint8_t(JS_CHECK_ACCESSOR_FLAGS(flags) | JSPROP_SHARED), \
     JSNATIVE_WRAPPER(getter), \
     JSNATIVE_WRAPPER(setter)}
#define JS_SELF_HOSTED_GET(name, getterName, flags) \
    {name, \
     uint8_t(JS_CHECK_ACCESSOR_FLAGS(flags) | JSPROP_SHARED | JSPROP_GETTER), \
     { nullptr, JS_CAST_STRING_TO(getterName, const JSJitInfo*) }, \
     JSNATIVE_WRAPPER(nullptr) }
#define JS_SELF_HOSTED_GETSET(name, getterName, setterName, flags) \
    {name, \
     uint8_t(JS_CHECK_ACCESSOR_FLAGS(flags) | JSPROP_SHARED | JSPROP_GETTER | JSPROP_SETTER), \
     { nullptr, JS_CAST_STRING_TO(getterName, const JSJitInfo*) },  \
     { nullptr, JS_CAST_STRING_TO(setterName, const JSJitInfo*) } }
#define JS_PS_END { nullptr, 0, JSNATIVE_WRAPPER(nullptr), JSNATIVE_WRAPPER(nullptr) }

/*
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
#define JS_FS_END JS_FS(nullptr,nullptr,0,0)

/*
 * Initializer macros for a JSFunctionSpec array element. JS_FN (whose name pays
 * homage to the old JSNative/JSFastNative split) simply adds the flag
 * JSFUN_STUB_GSOPS. JS_FNINFO allows the simple adding of
 * JSJitInfos. JS_SELF_HOSTED_FN declares a self-hosted function. Finally
 * JS_FNSPEC has slots for all the fields.
 *
 * The _SYM variants allow defining a function with a symbol key rather than a
 * string key. For example, use JS_SYM_FN(iterator, ...) to define an
 * @@iterator method.
 */
#define JS_FS(name,call,nargs,flags)                                          \
    JS_FNSPEC(name, call, nullptr, nargs, flags, nullptr)
#define JS_FN(name,call,nargs,flags)                                          \
    JS_FNSPEC(name, call, nullptr, nargs, (flags) | JSFUN_STUB_GSOPS, nullptr)
#define JS_SYM_FN(name,call,nargs,flags)                                      \
    JS_SYM_FNSPEC(symbol, call, nullptr, nargs, (flags) | JSFUN_STUB_GSOPS, nullptr)
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

/*
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

extern JS_PUBLIC_API(void*)
JS_GetPrivate(JSObject* obj);

extern JS_PUBLIC_API(void)
JS_SetPrivate(JSObject* obj, void* data);

extern JS_PUBLIC_API(void*)
JS_GetInstancePrivate(JSContext* cx, JS::Handle<JSObject*> obj, const JSClass* clasp,
                      JS::CallArgs* args);

extern JS_PUBLIC_API(bool)
JS_GetPrototype(JSContext* cx, JS::HandleObject obj, JS::MutableHandleObject protop);

extern JS_PUBLIC_API(bool)
JS_SetPrototype(JSContext* cx, JS::HandleObject obj, JS::HandleObject proto);

extern JS_PUBLIC_API(JSObject*)
JS_GetParent(JSObject* obj);

extern JS_PUBLIC_API(bool)
JS_SetParent(JSContext* cx, JS::HandleObject obj, JS::HandleObject parent);

extern JS_PUBLIC_API(JSObject*)
JS_GetConstructor(JSContext* cx, JS::Handle<JSObject*> proto);

namespace JS {

enum ZoneSpecifier {
    FreshZone = 0,
    SystemZone = 1
};

class JS_PUBLIC_API(CompartmentOptions)
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

    explicit CompartmentOptions()
      : version_(JSVERSION_UNKNOWN)
      , invisibleToDebugger_(false)
      , mergeable_(false)
      , discardSource_(false)
      , cloneSingletons_(false)
      , traceGlobal_(nullptr)
      , singletonsAsTemplates_(true)
      , addonId_(nullptr)
      , preserveJitCode_(false)
    {
        zone_.spec = JS::FreshZone;
    }

    JSVersion version() const { return version_; }
    CompartmentOptions& setVersion(JSVersion aVersion) {
        MOZ_ASSERT(aVersion != JSVERSION_UNKNOWN);
        version_ = aVersion;
        return *this;
    }

    // Certain scopes (i.e. XBL compilation scopes) are implementation details
    // of the embedding, and references to them should never leak out to script.
    // This flag causes the this compartment to skip firing onNewGlobalObject
    // and makes addDebuggee a no-op for this global.
    bool invisibleToDebugger() const { return invisibleToDebugger_; }
    CompartmentOptions& setInvisibleToDebugger(bool flag) {
        invisibleToDebugger_ = flag;
        return *this;
    }

    // Compartments used for off-thread compilation have their contents merged
    // into a target compartment when the compilation is finished. This is only
    // allowed if this flag is set.  The invisibleToDebugger flag must also be
    // set for such compartments.
    bool mergeable() const { return mergeable_; }
    CompartmentOptions& setMergeable(bool flag) {
        mergeable_ = flag;
        return *this;
    }

    // For certain globals, we know enough about the code that will run in them
    // that we can discard script source entirely.
    bool discardSource() const { return discardSource_; }
    CompartmentOptions& setDiscardSource(bool flag) {
        discardSource_ = flag;
        return *this;
    }

    bool cloneSingletons() const { return cloneSingletons_; }
    CompartmentOptions& setCloneSingletons(bool flag) {
        cloneSingletons_ = flag;
        return *this;
    }

    bool extraWarnings(JSRuntime* rt) const;
    bool extraWarnings(JSContext* cx) const;
    Override& extraWarningsOverride() { return extraWarningsOverride_; }

    void* zonePointer() const {
        MOZ_ASSERT(uintptr_t(zone_.pointer) > uintptr_t(JS::SystemZone));
        return zone_.pointer;
    }
    ZoneSpecifier zoneSpecifier() const { return zone_.spec; }
    CompartmentOptions& setZone(ZoneSpecifier spec);
    CompartmentOptions& setSameZoneAs(JSObject* obj);

    void setSingletonsAsValues() {
        singletonsAsTemplates_ = false;
    }
    bool getSingletonsAsTemplates() const {
        return singletonsAsTemplates_;
    }

    // A null add-on ID means that the compartment is not associated with an
    // add-on.
    JSAddonId* addonIdOrNull() const { return addonId_; }
    CompartmentOptions& setAddonId(JSAddonId* id) {
        addonId_ = id;
        return *this;
    }

    CompartmentOptions& setTrace(JSTraceOp op) {
        traceGlobal_ = op;
        return *this;
    }
    JSTraceOp getTrace() const {
        return traceGlobal_;
    }

    bool preserveJitCode() const { return preserveJitCode_; }
    CompartmentOptions& setPreserveJitCode(bool flag) {
        preserveJitCode_ = flag;
        return *this;
    }

  private:
    JSVersion version_;
    bool invisibleToDebugger_;
    bool mergeable_;
    bool discardSource_;
    bool cloneSingletons_;
    Override extraWarningsOverride_;
    union {
        ZoneSpecifier spec;
        void* pointer; // js::Zone* is not exposed in the API.
    } zone_;
    JSTraceOp traceGlobal_;

    // To XDR singletons, we need to ensure that all singletons are all used as
    // templates, by making JSOP_OBJECT return a clone of the JSScript
    // singleton, instead of returning the value which is baked in the JSScript.
    bool singletonsAsTemplates_;

    JSAddonId* addonId_;
    bool preserveJitCode_;
};

JS_PUBLIC_API(CompartmentOptions&)
CompartmentOptionsRef(JSCompartment* compartment);

JS_PUBLIC_API(CompartmentOptions&)
CompartmentOptionsRef(JSObject* obj);

JS_PUBLIC_API(CompartmentOptions&)
CompartmentOptionsRef(JSContext* cx);

// During global creation, we fire notifications to callbacks registered
// via the Debugger API. These callbacks are arbitrary script, and can touch
// the global in arbitrary ways. When that happens, the global should not be
// in a half-baked state. But this creates a problem for consumers that need
// to set slots on the global to put it in a consistent state.
//
// This API provides a way for consumers to set slots atomically (immediately
// after the global is created), before any debugger hooks are fired. It's
// unfortunately on the clunky side, but that's the way the cookie crumbles.
//
// If callers have no additional state on the global to set up, they may pass
// |FireOnNewGlobalHook| to JS_NewGlobalObject, which causes that function to
// fire the hook as its final act before returning. Otherwise, callers should
// pass |DontFireOnNewGlobalHook|, which means that they are responsible for
// invoking JS_FireOnNewGlobalObject upon successfully creating the global. If
// an error occurs and the operation aborts, callers should skip firing the
// hook. But otherwise, callers must take care to fire the hook exactly once
// before compiling any script in the global's scope (we have assertions in
// place to enforce this). This lets us be sure that debugger clients never miss
// breakpoints.
enum OnNewGlobalHookOption {
    FireOnNewGlobalHook,
    DontFireOnNewGlobalHook
};

} /* namespace JS */

extern JS_PUBLIC_API(JSObject*)
JS_NewGlobalObject(JSContext* cx, const JSClass* clasp, JSPrincipals* principals,
                   JS::OnNewGlobalHookOption hookOption,
                   const JS::CompartmentOptions& options = JS::CompartmentOptions());
/*
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
JS_NewObject(JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> parent = JS::NullPtr());

/* Queries the [[Extensible]] property of the object. */
extern JS_PUBLIC_API(bool)
JS_IsExtensible(JSContext* cx, JS::HandleObject obj, bool* extensible);

extern JS_PUBLIC_API(bool)
JS_IsNative(JSObject* obj);

extern JS_PUBLIC_API(JSRuntime*)
JS_GetObjectRuntime(JSObject* obj);

/*
 * Unlike JS_NewObject, JS_NewObjectWithGivenProto does not compute a default proto.
 * If proto is JS::NullPtr, the JS object will have `null` as [[Prototype]].
 */
extern JS_PUBLIC_API(JSObject*)
JS_NewObjectWithGivenProto(JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> proto,
                           JS::Handle<JSObject*> parent = JS::NullPtr());

// Creates a new plain object, like `new Object()`, with Object.prototype as [[Prototype]].
extern JS_PUBLIC_API(JSObject*)
JS_NewPlainObject(JSContext* cx);

/*
 * Freeze obj, and all objects it refers to, recursively. This will not recurse
 * through non-extensible objects, on the assumption that those are already
 * deep-frozen.
 */
extern JS_PUBLIC_API(bool)
JS_DeepFreezeObject(JSContext* cx, JS::Handle<JSObject*> obj);

/*
 * Freezes an object; see ES5's Object.freeze(obj) method.
 */
extern JS_PUBLIC_API(bool)
JS_FreezeObject(JSContext* cx, JS::Handle<JSObject*> obj);

/*
 * Attempt to make |obj| non-extensible.  If an error occurs while making the
 * attempt, return false (with a pending exception set, depending upon the
 * nature of the error).  If no error occurs, return true with |*succeeded| set
 * to indicate whether the attempt successfully set the [[Extensible]] property
 * to false.
 */
extern JS_PUBLIC_API(bool)
JS_PreventExtensions(JSContext* cx, JS::HandleObject obj, bool* succeeded);

extern JS_PUBLIC_API(JSObject*)
JS_New(JSContext* cx, JS::HandleObject ctor, const JS::HandleValueArray& args);

extern JS_PUBLIC_API(JSObject*)
JS_DefineObject(JSContext* cx, JS::HandleObject obj, const char* name,
                const JSClass* clasp = nullptr, unsigned attrs = 0);

extern JS_PUBLIC_API(bool)
JS_DefineConstDoubles(JSContext* cx, JS::HandleObject obj, const JSConstDoubleSpec* cds);

extern JS_PUBLIC_API(bool)
JS_DefineConstIntegers(JSContext* cx, JS::HandleObject obj, const JSConstIntegerSpec* cis);

extern JS_PUBLIC_API(bool)
JS_DefineProperties(JSContext* cx, JS::HandleObject obj, const JSPropertySpec* ps);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::HandleValue value,
                  unsigned attrs,
                  JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::HandleObject value,
                  unsigned attrs,
                  JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::HandleString value,
                  unsigned attrs,
                  JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, int32_t value,
                  unsigned attrs,
                  JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, uint32_t value,
                  unsigned attrs,
                  JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineProperty(JSContext* cx, JS::HandleObject obj, const char* name, double value,
                  unsigned attrs,
                  JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue value,
                      unsigned attrs,
                      JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleObject value,
                      unsigned attrs,
                      JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleString value,
                      unsigned attrs,
                      JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, int32_t value,
                      unsigned attrs,
                      JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, uint32_t value,
                      unsigned attrs,
                      JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefinePropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, double value,
                      unsigned attrs,
                      JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_AlreadyHasOwnProperty(JSContext* cx, JS::HandleObject obj, const char* name,
                         bool* foundp);

extern JS_PUBLIC_API(bool)
JS_AlreadyHasOwnPropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                             bool* foundp);

extern JS_PUBLIC_API(bool)
JS_HasProperty(JSContext* cx, JS::HandleObject obj, const char* name, bool* foundp);

extern JS_PUBLIC_API(bool)
JS_HasPropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* foundp);

struct JSPropertyDescriptor {
    JSObject*          obj;
    unsigned           attrs;
    JSPropertyOp       getter;
    JSStrictPropertyOp setter;
    JS::Value          value;

    JSPropertyDescriptor()
      : obj(nullptr), attrs(0), getter(nullptr), setter(nullptr), value(JSVAL_VOID)
    {}

    void trace(JSTracer* trc);

    static js::ThingRootKind rootKind() { return js::THING_ROOT_PROPERTY_DESCRIPTOR; }
};

namespace JS {

template <typename Outer>
class PropertyDescriptorOperations
{
    const JSPropertyDescriptor * desc() const { return static_cast<const Outer*>(this)->extract(); }

  public:
    bool isEnumerable() const { return desc()->attrs & JSPROP_ENUMERATE; }
    bool isReadonly() const { return desc()->attrs & JSPROP_READONLY; }
    bool isPermanent() const { return desc()->attrs & JSPROP_PERMANENT; }
    bool hasGetterObject() const { return desc()->attrs & JSPROP_GETTER; }
    bool hasSetterObject() const { return desc()->attrs & JSPROP_SETTER; }
    bool hasGetterOrSetterObject() const { return desc()->attrs & (JSPROP_GETTER | JSPROP_SETTER); }
    bool hasGetterOrSetter() const { return desc()->getter || desc()->setter; }
    bool isShared() const { return desc()->attrs & JSPROP_SHARED; }
    bool isIndex() const { return desc()->attrs & JSPROP_INDEX; }
    bool hasAttributes(unsigned attrs) const { return desc()->attrs & attrs; }

    // Descriptors with JSPropertyOps are considered data descriptors. It's
    // complicated.
    bool isAccessorDescriptor() const { return hasGetterOrSetterObject(); }
    bool isDataDescriptor() const { return !isAccessorDescriptor(); }

    bool isWritable() const { MOZ_ASSERT(isDataDescriptor()); return !isReadonly(); }

    JS::HandleObject object() const {
        return JS::HandleObject::fromMarkedLocation(&desc()->obj);
    }
    unsigned attributes() const { return desc()->attrs; }
    JSPropertyOp getter() const { return desc()->getter; }
    JSStrictPropertyOp setter() const { return desc()->setter; }
    JS::HandleObject getterObject() const {
        MOZ_ASSERT(hasGetterObject());
        return JS::HandleObject::fromMarkedLocation(
                reinterpret_cast<JSObject* const*>(&desc()->getter));
    }
    JS::HandleObject setterObject() const {
        MOZ_ASSERT(hasSetterObject());
        return JS::HandleObject::fromMarkedLocation(
                reinterpret_cast<JSObject* const*>(&desc()->setter));
    }
    JS::HandleValue value() const {
        return JS::HandleValue::fromMarkedLocation(&desc()->value);
    }
};

template <typename Outer>
class MutablePropertyDescriptorOperations : public PropertyDescriptorOperations<Outer>
{
    JSPropertyDescriptor * desc() { return static_cast<Outer*>(this)->extractMutable(); }

  public:

    void clear() {
        object().set(nullptr);
        setAttributes(0);
        setGetter(nullptr);
        setSetter(nullptr);
        value().setUndefined();
    }

    void assign(JSPropertyDescriptor& other) {
        object().set(other.obj);
        setAttributes(other.attrs);
        setGetter(other.getter);
        setSetter(other.setter);
        value().set(other.value);
    }

    JS::MutableHandleObject object() {
        return JS::MutableHandleObject::fromMarkedLocation(&desc()->obj);
    }
    unsigned& attributesRef() { return desc()->attrs; }
    JSPropertyOp& getter() { return desc()->getter; }
    JSStrictPropertyOp& setter() { return desc()->setter; }
    JS::MutableHandleValue value() {
        return JS::MutableHandleValue::fromMarkedLocation(&desc()->value);
    }

    void setEnumerable() { desc()->attrs |= JSPROP_ENUMERATE; }
    void setAttributes(unsigned attrs) { desc()->attrs = attrs; }

    void setGetter(JSPropertyOp op) {
        MOZ_ASSERT(op != JS_PropertyStub);
        desc()->getter = op;
    }
    void setSetter(JSStrictPropertyOp op) {
        MOZ_ASSERT(op != JS_StrictPropertyStub);
        desc()->setter = op;
    }
    void setGetterObject(JSObject* obj) { desc()->getter = reinterpret_cast<JSPropertyOp>(obj); }
    void setSetterObject(JSObject* obj) { desc()->setter = reinterpret_cast<JSStrictPropertyOp>(obj); }

    JS::MutableHandleObject getterObject() {
        MOZ_ASSERT(this->hasGetterObject());
        return JS::MutableHandleObject::fromMarkedLocation(
                reinterpret_cast<JSObject**>(&desc()->getter));
    }
    JS::MutableHandleObject setterObject() {
        MOZ_ASSERT(this->hasSetterObject());
        return JS::MutableHandleObject::fromMarkedLocation(
                reinterpret_cast<JSObject**>(&desc()->setter));
    }
};

} /* namespace JS */

namespace js {

template <>
struct GCMethods<JSPropertyDescriptor> {
    static JSPropertyDescriptor initial() { return JSPropertyDescriptor(); }
    static bool poisoned(const JSPropertyDescriptor& desc) {
        return (desc.obj && JS::IsPoisonedPtr(desc.obj)) ||
               (desc.attrs & JSPROP_GETTER && desc.getter && JS::IsPoisonedPtr(desc.getter)) ||
               (desc.attrs & JSPROP_SETTER && desc.setter && JS::IsPoisonedPtr(desc.setter)) ||
               (desc.value.isGCThing() && JS::IsPoisonedPtr(desc.value.toGCThing()));
    }
};

template <>
class RootedBase<JSPropertyDescriptor>
  : public JS::MutablePropertyDescriptorOperations<JS::Rooted<JSPropertyDescriptor> >
{
    friend class JS::PropertyDescriptorOperations<JS::Rooted<JSPropertyDescriptor> >;
    friend class JS::MutablePropertyDescriptorOperations<JS::Rooted<JSPropertyDescriptor> >;
    const JSPropertyDescriptor* extract() const {
        return static_cast<const JS::Rooted<JSPropertyDescriptor>*>(this)->address();
    }
    JSPropertyDescriptor* extractMutable() {
        return static_cast<JS::Rooted<JSPropertyDescriptor>*>(this)->address();
    }
};

template <>
class HandleBase<JSPropertyDescriptor>
  : public JS::PropertyDescriptorOperations<JS::Handle<JSPropertyDescriptor> >
{
    friend class JS::PropertyDescriptorOperations<JS::Handle<JSPropertyDescriptor> >;
    const JSPropertyDescriptor* extract() const {
        return static_cast<const JS::Handle<JSPropertyDescriptor>*>(this)->address();
    }
};

template <>
class MutableHandleBase<JSPropertyDescriptor>
  : public JS::MutablePropertyDescriptorOperations<JS::MutableHandle<JSPropertyDescriptor> >
{
    friend class JS::PropertyDescriptorOperations<JS::MutableHandle<JSPropertyDescriptor> >;
    friend class JS::MutablePropertyDescriptorOperations<JS::MutableHandle<JSPropertyDescriptor> >;
    const JSPropertyDescriptor* extract() const {
        return static_cast<const JS::MutableHandle<JSPropertyDescriptor>*>(this)->address();
    }
    JSPropertyDescriptor* extractMutable() {
        return static_cast<JS::MutableHandle<JSPropertyDescriptor>*>(this)->address();
    }
};

} /* namespace js */

namespace JS {

extern JS_PUBLIC_API(bool)
ParsePropertyDescriptorObject(JSContext* cx,
                              JS::HandleObject obj,
                              JS::HandleValue descriptor,
                              JS::MutableHandle<JSPropertyDescriptor> desc);

} // namespace JS

extern JS_PUBLIC_API(bool)
JS_GetOwnPropertyDescriptorById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                                JS::MutableHandle<JSPropertyDescriptor> desc);

extern JS_PUBLIC_API(bool)
JS_GetOwnPropertyDescriptor(JSContext* cx, JS::HandleObject obj, const char* name,
                            JS::MutableHandle<JSPropertyDescriptor> desc);

/*
 * Like JS_GetOwnPropertyDescriptorById but will return a property on
 * an object on the prototype chain (returned in desc->obj). If desc->obj is null,
 * then this property was not found on the prototype chain.
 */
extern JS_PUBLIC_API(bool)
JS_GetPropertyDescriptorById(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                             JS::MutableHandle<JSPropertyDescriptor> desc);

extern JS_PUBLIC_API(bool)
JS_GetPropertyDescriptor(JSContext* cx, JS::HandleObject obj, const char* name,
                         JS::MutableHandle<JSPropertyDescriptor> desc);

extern JS_PUBLIC_API(bool)
JS_GetProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_GetPropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_ForwardGetPropertyTo(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleObject onBehalfOf,
                        JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_SetProperty(JSContext* cx, JS::HandleObject obj, const char* name, JS::HandleValue v);

extern JS_PUBLIC_API(bool)
JS_SetPropertyById(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue v);

extern JS_PUBLIC_API(bool)
JS_ForwardSetPropertyTo(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue onBehalfOf,
                        bool strict, JS::HandleValue vp);

extern JS_PUBLIC_API(bool)
JS_DeleteProperty(JSContext* cx, JS::HandleObject obj, const char* name);

extern JS_PUBLIC_API(bool)
JS_DeleteProperty2(JSContext* cx, JS::HandleObject obj, const char* name, bool* succeeded);

extern JS_PUBLIC_API(bool)
JS_DeletePropertyById(JSContext* cx, JS::HandleObject obj, jsid id);

extern JS_PUBLIC_API(bool)
JS_DeletePropertyById2(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* succeeded);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JS::HandleValue value, unsigned attrs,
                    JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JS::HandleObject value, unsigned attrs,
                    JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    JS::HandleString value, unsigned attrs,
                    JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    int32_t value, unsigned attrs,
                    JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    uint32_t value, unsigned attrs,
                    JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                    double value, unsigned attrs,
                    JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_AlreadyHasOwnUCProperty(JSContext* cx, JS::HandleObject obj, const char16_t* name,
                           size_t namelen, bool* foundp);

extern JS_PUBLIC_API(bool)
JS_HasUCProperty(JSContext* cx, JS::HandleObject obj,
                 const char16_t* name, size_t namelen,
                 bool* vp);

extern JS_PUBLIC_API(bool)
JS_GetUCProperty(JSContext* cx, JS::HandleObject obj,
                 const char16_t* name, size_t namelen,
                 JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_SetUCProperty(JSContext* cx, JS::HandleObject obj,
                 const char16_t* name, size_t namelen,
                 JS::HandleValue v);

extern JS_PUBLIC_API(bool)
JS_DeleteUCProperty2(JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
                     bool* succeeded);

extern JS_PUBLIC_API(JSObject*)
JS_NewArrayObject(JSContext* cx, const JS::HandleValueArray& contents);

extern JS_PUBLIC_API(JSObject*)
JS_NewArrayObject(JSContext* cx, size_t length);

extern JS_PUBLIC_API(bool)
JS_IsArrayObject(JSContext* cx, JS::HandleValue value);

extern JS_PUBLIC_API(bool)
JS_IsArrayObject(JSContext* cx, JS::HandleObject obj);

extern JS_PUBLIC_API(bool)
JS_GetArrayLength(JSContext* cx, JS::Handle<JSObject*> obj, uint32_t* lengthp);

extern JS_PUBLIC_API(bool)
JS_SetArrayLength(JSContext* cx, JS::Handle<JSObject*> obj, uint32_t length);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleValue value,
                 unsigned attrs,
                 JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleObject value,
                 unsigned attrs,
                 JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleString value,
                 unsigned attrs,
                 JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, int32_t value,
                 unsigned attrs,
                 JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, uint32_t value,
                 unsigned attrs,
                 JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_DefineElement(JSContext* cx, JS::HandleObject obj, uint32_t index, double value,
                 unsigned attrs,
                 JSNative getter = nullptr, JSNative setter = nullptr);

extern JS_PUBLIC_API(bool)
JS_AlreadyHasOwnElement(JSContext* cx, JS::HandleObject obj, uint32_t index, bool* foundp);

extern JS_PUBLIC_API(bool)
JS_HasElement(JSContext* cx, JS::HandleObject obj, uint32_t index, bool* foundp);

extern JS_PUBLIC_API(bool)
JS_GetElement(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::MutableHandleValue vp);

extern JS_PUBLIC_API(bool)
JS_ForwardGetElementTo(JSContext* cx, JS::HandleObject obj, uint32_t index,
                       JS::HandleObject onBehalfOf, JS::MutableHandleValue vp);

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

extern JS_PUBLIC_API(bool)
JS_DeleteElement(JSContext* cx, JS::HandleObject obj, uint32_t index);

extern JS_PUBLIC_API(bool)
JS_DeleteElement2(JSContext* cx, JS::HandleObject obj, uint32_t index, bool* succeeded);

/*
 * Assign 'undefined' to all of the object's non-reserved slots. Note: this is
 * done for all slots, regardless of the associated property descriptor.
 */
JS_PUBLIC_API(void)
JS_SetAllNonReservedSlotsToUndefined(JSContext* cx, JSObject* objArg);

/*
 * Create a new array buffer with the given contents. It must be legal to pass
 * these contents to free(). On success, the ownership is transferred to the
 * new array buffer.
 */
extern JS_PUBLIC_API(JSObject*)
JS_NewArrayBufferWithContents(JSContext* cx, size_t nbytes, void* contents);

/*
 * Steal the contents of the given array buffer. The array buffer has its
 * length set to 0 and its contents array cleared. The caller takes ownership
 * of the return value and must free it or transfer ownership via
 * JS_NewArrayBufferWithContents when done using it.
 */
extern JS_PUBLIC_API(void*)
JS_StealArrayBufferContents(JSContext* cx, JS::HandleObject obj);

/*
 * Create a new mapped array buffer with the given memory mapped contents. It
 * must be legal to free the contents pointer by unmapping it. On success,
 * ownership is transferred to the new mapped array buffer.
 */
extern JS_PUBLIC_API(JSObject*)
JS_NewMappedArrayBufferWithContents(JSContext* cx, size_t nbytes, void* contents);

/*
 * Create memory mapped array buffer contents.
 * Caller must take care of closing fd after calling this function.
 */
extern JS_PUBLIC_API(void*)
JS_CreateMappedArrayBufferContents(int fd, size_t offset, size_t length);

/*
 * Release the allocated resource of mapped array buffer contents before the
 * object is created.
 * If a new object has been created by JS_NewMappedArrayBufferWithContents()
 * with this content, then JS_NeuterArrayBuffer() should be used instead to
 * release the resource used by the object.
 */
extern JS_PUBLIC_API(void)
JS_ReleaseMappedArrayBufferContents(void* contents, size_t length);

extern JS_PUBLIC_API(JSIdArray*)
JS_Enumerate(JSContext* cx, JS::HandleObject obj);

extern JS_PUBLIC_API(jsval)
JS_GetReservedSlot(JSObject* obj, uint32_t index);

extern JS_PUBLIC_API(void)
JS_SetReservedSlot(JSObject* obj, uint32_t index, jsval v);

/************************************************************************/

/*
 * Functions and scripts.
 */
extern JS_PUBLIC_API(JSFunction*)
JS_NewFunction(JSContext* cx, JSNative call, unsigned nargs, unsigned flags,
               JS::Handle<JSObject*> parent, const char* name);

/*
 * Create the function with the name given by the id. JSID_IS_STRING(id) must
 * be true.
 */
extern JS_PUBLIC_API(JSFunction*)
JS_NewFunctionById(JSContext* cx, JSNative call, unsigned nargs, unsigned flags,
                   JS::Handle<JSObject*> parent, JS::Handle<jsid> id);

namespace JS {

extern JS_PUBLIC_API(JSFunction*)
GetSelfHostedFunction(JSContext* cx, const char* selfHostedName, JS::Handle<jsid> id,
                      unsigned nargs);

} /* namespace JS */

extern JS_PUBLIC_API(JSObject*)
JS_GetFunctionObject(JSFunction* fun);

/*
 * Return the function's identifier as a JSString, or null if fun is unnamed.
 * The returned string lives as long as fun, so you don't need to root a saved
 * reference to it if fun is well-connected or rooted, and provided you bound
 * the use of the saved reference by fun's lifetime.
 */
extern JS_PUBLIC_API(JSString*)
JS_GetFunctionId(JSFunction* fun);

/*
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

/*
 * API for determining callability and constructability. This does the right
 * thing for proxies.
 */
namespace JS {

extern JS_PUBLIC_API(bool)
IsCallable(JSObject* obj);

extern JS_PUBLIC_API(bool)
IsConstructor(JSObject* obj);

} /* namespace JS */

/*
 * Infallible predicate to test whether obj is a function object (faster than
 * comparing obj's class name to "Function", but equivalent unless someone has
 * overwritten the "Function" identifier with a different constructor and then
 * created instances using that constructor that might be passed in as obj).
 */
extern JS_PUBLIC_API(bool)
JS_ObjectIsFunction(JSContext* cx, JSObject* obj);

extern JS_PUBLIC_API(bool)
JS_IsNativeFunction(JSObject* funobj, JSNative call);

/* Return whether the given function is a valid constructor. */
extern JS_PUBLIC_API(bool)
JS_IsConstructor(JSFunction* fun);

/*
 * Bind the given callable to use the given object as "this".
 *
 * If |callable| is not callable, will throw and return nullptr.
 */
extern JS_PUBLIC_API(JSObject*)
JS_BindCallable(JSContext* cx, JS::Handle<JSObject*> callable, JS::Handle<JSObject*> newThis);

// This enum is used to select if properties with JSPROP_DEFINE_LATE flag
// should be defined on the object.
// Normal JSAPI consumers probably always want DefineAllProperties here.
enum PropertyDefinitionBehavior {
    DefineAllProperties,
    OnlyDefineLateProperties,
    DontDefineLateProperties
};

extern JS_PUBLIC_API(bool)
JS_DefineFunctions(JSContext* cx, JS::Handle<JSObject*> obj, const JSFunctionSpec* fs,
                   PropertyDefinitionBehavior behavior = DefineAllProperties);

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

namespace JS {

/*
 * Clone a top-level function into cx's global. This function will dynamically
 * fail if funobj was lexically nested inside some other function.
 */
extern JS_PUBLIC_API(JSObject*)
CloneFunctionObject(JSContext* cx, HandleObject funobj);

/*
 * As above, but providing an explicit scope chain.  scopeChain must not include
 * the global object on it; that's implicit.  It needs to contain the other
 * objects that should end up on the clone's scope chain.
 */
extern JS_PUBLIC_API(JSObject*)
CloneFunctionObject(JSContext* cx, HandleObject funobj, AutoObjectVector& scopeChain);

} // namespace JS

/*
 * Given a buffer, return false if the buffer might become a valid
 * javascript statement with the addition of more lines.  Otherwise return
 * true.  The intent is to support interactive compilation - accumulate
 * lines in a buffer until JS_BufferIsCompilableUnit is true, then pass it to
 * the compiler.
 */
extern JS_PUBLIC_API(bool)
JS_BufferIsCompilableUnit(JSContext* cx, JS::Handle<JSObject*> obj, const char* utf8,
                          size_t length);

/*
 * |script| will always be set. On failure, it will be set to nullptr.
 */
extern JS_PUBLIC_API(bool)
JS_CompileScript(JSContext* cx, JS::HandleObject obj,
                 const char* ascii, size_t length,
                 const JS::CompileOptions& options,
                 JS::MutableHandleScript script);

/*
 * |script| will always be set. On failure, it will be set to nullptr.
 */
extern JS_PUBLIC_API(bool)
JS_CompileUCScript(JSContext* cx, JS::HandleObject obj,
                   const char16_t* chars, size_t length,
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
 * So, we have a class hierarchy that reflects these three use cases:
 *
 * - ReadOnlyCompileOptions is the common base class. It can be used by code
 *   that simply needs to access options set elsewhere, like the compiler.
 *
 * - The usual CompileOptions class must be stack-allocated, and holds
 *   non-owning references to the filename, element, and so on. It's derived
 *   from ReadOnlyCompileOptions, so the compiler can use it.
 *
 * - OwningCompileOptions roots / copies / reference counts of all its values,
 *   and unroots / frees / releases them when it is destructed. It too is
 *   derived from ReadOnlyCompileOptions, so the compiler accepts it.
 */

/*
 * The common base class for the CompileOptions hierarchy.
 *
 * Use this in code that only needs to access compilation options created
 * elsewhere, like the compiler. Don't instantiate this class (the constructor
 * is protected anyway); instead, create instances only of the derived classes:
 * CompileOptions and OwningCompileOptions.
 */
class JS_FRIEND_API(ReadOnlyCompileOptions)
{
    friend class CompileOptions;

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

    // This constructor leaves 'version' set to JSVERSION_UNKNOWN. The structure
    // is unusable until that's set to something more specific; the derived
    // classes' constructors take care of that, in ways appropriate to their
    // purpose.
    ReadOnlyCompileOptions()
      : mutedErrors_(false),
        filename_(nullptr),
        introducerFilename_(nullptr),
        sourceMapURL_(nullptr),
        version(JSVERSION_UNKNOWN),
        versionSet(false),
        utf8(false),
        lineno(1),
        column(0),
        compileAndGo(false),
        forEval(false),
        noScriptRval(false),
        selfHostingMode(false),
        canLazilyParse(true),
        strictOption(false),
        extraWarningsOption(false),
        werrorOption(false),
        asmJSOption(false),
        forceAsync(false),
        installedFile(false),
        sourceIsLazy(false),
        introductionType(nullptr),
        introductionLineno(0),
        introductionOffset(0),
        hasIntroductionInfo(false)
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
    virtual JSObject* element() const = 0;
    virtual JSString* elementAttributeName() const = 0;
    virtual JSScript* introductionScript() const = 0;

    // POD options.
    JSVersion version;
    bool versionSet;
    bool utf8;
    unsigned lineno;
    unsigned column;
    bool compileAndGo;
    bool forEval;
    bool noScriptRval;
    bool selfHostingMode;
    bool canLazilyParse;
    bool strictOption;
    bool extraWarningsOption;
    bool werrorOption;
    bool asmJSOption;
    bool forceAsync;
    bool installedFile;  // 'true' iff pre-compiling js file in packaged app
    bool sourceIsLazy;

    // |introductionType| is a statically allocated C string:
    // one of "eval", "Function", or "GeneratorFunction".
    const char* introductionType;
    unsigned introductionLineno;
    uint32_t introductionOffset;
    bool hasIntroductionInfo;

  private:
    static JSObject * const nullObjectPtr;
    void operator=(const ReadOnlyCompileOptions&) = delete;
};

/*
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
    JSRuntime* runtime;
    PersistentRootedObject elementRoot;
    PersistentRootedString elementAttributeNameRoot;
    PersistentRootedScript introductionScriptRoot;

  public:
    // A minimal constructor, for use with OwningCompileOptions::copy. This
    // leaves |this.version| set to JSVERSION_UNKNOWN; the instance
    // shouldn't be used until we've set that to something real (as |copy|
    // will).
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
    OwningCompileOptions& setVersion(JSVersion v) {
        version = v;
        versionSet = true;
        return *this;
    }
    OwningCompileOptions& setUTF8(bool u) { utf8 = u; return *this; }
    OwningCompileOptions& setColumn(unsigned c) { column = c; return *this; }
    OwningCompileOptions& setCompileAndGo(bool cng) { compileAndGo = cng; return *this; }
    OwningCompileOptions& setForEval(bool eval) { forEval = eval; return *this; }
    OwningCompileOptions& setNoScriptRval(bool nsr) { noScriptRval = nsr; return *this; }
    OwningCompileOptions& setSelfHostingMode(bool shm) { selfHostingMode = shm; return *this; }
    OwningCompileOptions& setCanLazilyParse(bool clp) { canLazilyParse = clp; return *this; }
    OwningCompileOptions& setSourceIsLazy(bool l) { sourceIsLazy = l; return *this; }
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

/*
 * Compilation options stored on the stack. An instance of this type
 * simply holds references to dynamically allocated resources (element;
 * filename; source map URL) that are owned by something else. If you
 * create an instance of this type, it's up to you to guarantee that
 * everything you store in it will outlive it.
 */
class MOZ_STACK_CLASS JS_FRIEND_API(CompileOptions) : public ReadOnlyCompileOptions
{
    RootedObject elementRoot;
    RootedString elementAttributeNameRoot;
    RootedScript introductionScriptRoot;

  public:
    explicit CompileOptions(JSContext* cx, JSVersion version = JSVERSION_UNKNOWN);
    CompileOptions(js::ContextFriendFields* cx, const ReadOnlyCompileOptions& rhs)
      : ReadOnlyCompileOptions(), elementRoot(cx), elementAttributeNameRoot(cx),
        introductionScriptRoot(cx)
    {
        copyPODOptions(rhs);

        mutedErrors_ = rhs.mutedErrors_;
        filename_ = rhs.filename();
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
    CompileOptions& setVersion(JSVersion v) {
        version = v;
        versionSet = true;
        return *this;
    }
    CompileOptions& setUTF8(bool u) { utf8 = u; return *this; }
    CompileOptions& setColumn(unsigned c) { column = c; return *this; }
    CompileOptions& setCompileAndGo(bool cng) { compileAndGo = cng; return *this; }
    CompileOptions& setForEval(bool eval) { forEval = eval; return *this; }
    CompileOptions& setNoScriptRval(bool nsr) { noScriptRval = nsr; return *this; }
    CompileOptions& setSelfHostingMode(bool shm) { selfHostingMode = shm; return *this; }
    CompileOptions& setCanLazilyParse(bool clp) { canLazilyParse = clp; return *this; }
    CompileOptions& setSourceIsLazy(bool l) { sourceIsLazy = l; return *this; }
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

/*
 * |script| will always be set. On failure, it will be set to nullptr.
 */
extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, JS::HandleObject obj, const ReadOnlyCompileOptions& options,
        SourceBufferHolder& srcBuf, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, JS::HandleObject obj, const ReadOnlyCompileOptions& options,
        const char* bytes, size_t length, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, JS::HandleObject obj, const ReadOnlyCompileOptions& options,
        const char16_t* chars, size_t length, JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, JS::HandleObject obj, const ReadOnlyCompileOptions& options, FILE* file,
        JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
Compile(JSContext* cx, JS::HandleObject obj, const ReadOnlyCompileOptions& options, const char* filename,
        JS::MutableHandleScript script);

extern JS_PUBLIC_API(bool)
CanCompileOffThread(JSContext* cx, const ReadOnlyCompileOptions& options, size_t length);

/*
 * Off thread compilation control flow.
 *
 * After successfully triggering an off thread compile of a script, the
 * callback will eventually be invoked with the specified data and a token
 * for the compilation. The callback will be invoked while off the main thread,
 * so must ensure that its operations are thread safe. Afterwards,
 * FinishOffThreadScript must be invoked on the main thread to get the result
 * script or nullptr. If maybecx is not specified, the resources will be freed,
 * but no script will be returned.
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
FinishOffThreadScript(JSContext* maybecx, JSRuntime* rt, void* token);

/**
 * Compile a function with scopeChain plus the global as its scope chain.
 * scopeChain must contain objects in the current compartment of cx.  The actual
 * scope chain used for the function will consist of With wrappers for those
 * objects, followed by the current global of the compartment cx is in.  This
 * global must not be explicitly included in the scope chain.
 */
extern JS_PUBLIC_API(bool)
CompileFunction(JSContext* cx, AutoObjectVector& scopeChain,
                const ReadOnlyCompileOptions& options,
                const char* name, unsigned nargs, const char* const* argnames,
                const char16_t* chars, size_t length, JS::MutableHandleFunction fun);

/**
 * Same as above, but taking a SourceBufferHolder for the function body.
 */
extern JS_PUBLIC_API(bool)
CompileFunction(JSContext* cx, AutoObjectVector& scopeChain,
                const ReadOnlyCompileOptions& options,
                const char* name, unsigned nargs, const char* const* argnames,
                SourceBufferHolder& srcBuf, JS::MutableHandleFunction fun);

/**
 * Same as above, but taking a const char * for the function body.
 */
extern JS_PUBLIC_API(bool)
CompileFunction(JSContext* cx, AutoObjectVector& scopeChain,
                const ReadOnlyCompileOptions& options,
                const char* name, unsigned nargs, const char* const* argnames,
                const char* bytes, size_t length, JS::MutableHandleFunction fun);

} /* namespace JS */

extern JS_PUBLIC_API(JSString*)
JS_DecompileScript(JSContext* cx, JS::Handle<JSScript*> script, const char* name, unsigned indent);

/*
 * API extension: OR this into indent to avoid pretty-printing the decompiled
 * source resulting from JS_DecompileFunction{,Body}.
 */
#define JS_DONT_PRETTY_PRINT    ((unsigned)0x8000)

extern JS_PUBLIC_API(JSString*)
JS_DecompileFunction(JSContext* cx, JS::Handle<JSFunction*> fun, unsigned indent);

extern JS_PUBLIC_API(JSString*)
JS_DecompileFunctionBody(JSContext* cx, JS::Handle<JSFunction*> fun, unsigned indent);

/*
 * NB: JS_ExecuteScript and the JS::Evaluate APIs use the obj
 * parameter as the initial scope chain header, the 'this' keyword value, and
 * the variables object (ECMA parlance for where 'var' and 'function' bind
 * names) of the execution context for script.
 *
 * Using obj as the variables object is problematic if obj's parent (which is
 * the scope chain link; see JS_SetParent and JS_NewObject) is not null: in
 * this case, variables created by 'var x = 0', e.g., go in obj, but variables
 * created by assignment to an unbound id, 'x = 0', go in the last object on
 * the scope chain linked by parent.
 *
 * ECMA calls that last scoping object the "global object", but note that many
 * embeddings have several such objects.  ECMA requires that "global code" be
 * executed with the variables object equal to this global object.  But these
 * JS API entry points provide freedom to execute code against a "sub-global",
 * i.e., a parented or scoped object, in which case the variables object will
 * differ from the last object on the scope chain, resulting in confusing and
 * non-ECMA explicit vs. implicit variable creation.
 *
 * Caveat embedders: unless you already depend on this buggy variables object
 * binding behavior, you should call RuntimeOptionsRef(rt).setVarObjFix(true)
 * for each context in the application, if you pass parented objects as the obj
 * parameter, or may ever pass such objects in the future.
 *
 * Why a runtime option?  The alternative is to add APIs duplicating those below
 * for the other value of varobjfix, and that doesn't seem worth the code bloat
 * cost.  Such new entry points would probably have less obvious names, too, so
 * would not tend to be used.  The RuntimeOptionsRef adjustment, OTOH, can be
 * more easily hacked into existing code that does not depend on the bug; such
 * code can continue to use the familiar JS::Evaluate, etc., entry points.
 */
extern JS_PUBLIC_API(bool)
JS_ExecuteScript(JSContext* cx, JS::HandleObject obj, JS::HandleScript script, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
JS_ExecuteScript(JSContext* cx, JS::HandleObject obj, JS::HandleScript script);

/*
 * As above, but providing an explicit scope chain.  scopeChain must not include
 * the global object on it; that's implicit.  It needs to contain the other
 * objects that should end up on the scripts's scope chain.
 */
extern JS_PUBLIC_API(bool)
JS_ExecuteScript(JSContext* cx, JS::AutoObjectVector& scopeChain,
                 JS::HandleScript script, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
JS_ExecuteScript(JSContext* cx, JS::AutoObjectVector& scopeChain, JS::HandleScript script);

namespace JS {

/*
 * Like the above, but handles a cross-compartment script. If the script is
 * cross-compartment, it is cloned into the current compartment before executing.
 */
extern JS_PUBLIC_API(bool)
CloneAndExecuteScript(JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<JSScript*> script);

} /* namespace JS */

namespace JS {

extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, JS::HandleObject obj, const ReadOnlyCompileOptions& options,
         SourceBufferHolder& srcBuf, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, AutoObjectVector& scopeChain, const ReadOnlyCompileOptions& options,
         SourceBufferHolder& srcBuf, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, JS::HandleObject obj, const ReadOnlyCompileOptions& options,
         const char16_t* chars, size_t length, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, JS::HandleObject obj, const ReadOnlyCompileOptions& options,
         const char* bytes, size_t length, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
Evaluate(JSContext* cx, JS::HandleObject obj, const ReadOnlyCompileOptions& options,
         const char* filename, JS::MutableHandleValue rval);

} /* namespace JS */

extern JS_PUBLIC_API(bool)
JS_CallFunction(JSContext* cx, JS::HandleObject obj, JS::HandleFunction fun,
                const JS::HandleValueArray& args, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
JS_CallFunctionName(JSContext* cx, JS::HandleObject obj, const char* name,
                    const JS::HandleValueArray& args, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
JS_CallFunctionValue(JSContext* cx, JS::HandleObject obj, JS::HandleValue fval,
                     const JS::HandleValueArray& args, JS::MutableHandleValue rval);

namespace JS {

static inline bool
Call(JSContext* cx, JS::HandleObject thisObj, JS::HandleFunction fun,
     const JS::HandleValueArray& args, MutableHandleValue rval)
{
    return !!JS_CallFunction(cx, thisObj, fun, args, rval);
}

static inline bool
Call(JSContext* cx, JS::HandleObject thisObj, const char* name, const JS::HandleValueArray& args,
     MutableHandleValue rval)
{
    return !!JS_CallFunctionName(cx, thisObj, name, args, rval);
}

static inline bool
Call(JSContext* cx, JS::HandleObject thisObj, JS::HandleValue fun, const JS::HandleValueArray& args,
     MutableHandleValue rval)
{
    return !!JS_CallFunctionValue(cx, thisObj, fun, args, rval);
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

extern JS_PUBLIC_API(bool)
Construct(JSContext* cx, JS::HandleValue fun,
          const JS::HandleValueArray& args,
          MutableHandleValue rval);

} /* namespace JS */

/*
 * These functions allow setting an interrupt callback that will be called
 * from the JS thread some time after any thread triggered the callback using
 * JS_RequestInterruptCallback(rt).
 *
 * To schedule the GC and for other activities the engine internally triggers
 * interrupt callbacks. The embedding should thus not rely on callbacks being
 * triggered through the external API only.
 *
 * Important note: Additional callbacks can occur inside the callback handler
 * if it re-enters the JS engine. The embedding must ensure that the callback
 * is disconnected before attempting such re-entry.
 */
extern JS_PUBLIC_API(JSInterruptCallback)
JS_SetInterruptCallback(JSRuntime* rt, JSInterruptCallback callback);

extern JS_PUBLIC_API(JSInterruptCallback)
JS_GetInterruptCallback(JSRuntime* rt);

extern JS_PUBLIC_API(void)
JS_RequestInterruptCallback(JSRuntime* rt);

extern JS_PUBLIC_API(bool)
JS_IsRunning(JSContext* cx);

/*
 * Saving and restoring frame chains.
 *
 * These two functions are used to set aside cx's call stack while that stack
 * is inactive. After a call to JS_SaveFrameChain, it looks as if there is no
 * code running on cx. Before calling JS_RestoreFrameChain, cx's call stack
 * must be balanced and all nested calls to JS_SaveFrameChain must have had
 * matching JS_RestoreFrameChain calls.
 *
 * JS_SaveFrameChain deals with cx not having any code running on it.
 */
extern JS_PUBLIC_API(bool)
JS_SaveFrameChain(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_RestoreFrameChain(JSContext* cx);

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
JS_InternJSString(JSContext* cx, JS::HandleString str);

extern JS_PUBLIC_API(JSString*)
JS_InternStringN(JSContext* cx, const char* s, size_t length);

extern JS_PUBLIC_API(JSString*)
JS_InternString(JSContext* cx, const char* s);

extern JS_PUBLIC_API(JSString*)
JS_NewUCString(JSContext* cx, char16_t* chars, size_t length);

extern JS_PUBLIC_API(JSString*)
JS_NewUCStringCopyN(JSContext* cx, const char16_t* s, size_t n);

extern JS_PUBLIC_API(JSString*)
JS_NewUCStringCopyZ(JSContext* cx, const char16_t* s);

extern JS_PUBLIC_API(JSString*)
JS_InternUCStringN(JSContext* cx, const char16_t* s, size_t length);

extern JS_PUBLIC_API(JSString*)
JS_InternUCString(JSContext* cx, const char16_t* s);

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
 * The first case is interned strings, i.e., strings from JS_InternString or
 * JSID_TO_STRING(id), using JS_GetLatin1InternedStringChars or
 * JS_GetTwoByteInternedStringChars.
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

/* Returns true iff the string's characters are stored as Latin1. */
extern JS_PUBLIC_API(bool)
JS_StringHasLatin1Chars(JSString* str);

extern JS_PUBLIC_API(const JS::Latin1Char*)
JS_GetLatin1StringCharsAndLength(JSContext* cx, const JS::AutoCheckCannotGC& nogc, JSString* str,
                                 size_t* length);

extern JS_PUBLIC_API(const char16_t*)
JS_GetTwoByteStringCharsAndLength(JSContext* cx, const JS::AutoCheckCannotGC& nogc, JSString* str,
                                  size_t* length);

extern JS_PUBLIC_API(bool)
JS_GetStringCharAt(JSContext* cx, JSString* str, size_t index, char16_t* res);

extern JS_PUBLIC_API(char16_t)
JS_GetFlatStringCharAt(JSFlatString* str, size_t index);

extern JS_PUBLIC_API(const char16_t*)
JS_GetTwoByteExternalStringChars(JSString* str);

extern JS_PUBLIC_API(bool)
JS_CopyStringChars(JSContext* cx, mozilla::Range<char16_t> dest, JSString* str);

extern JS_PUBLIC_API(const JS::Latin1Char*)
JS_GetLatin1InternedStringChars(const JS::AutoCheckCannotGC& nogc, JSString* str);

extern JS_PUBLIC_API(const char16_t*)
JS_GetTwoByteInternedStringChars(const JS::AutoCheckCannotGC& nogc, JSString* str);

extern JS_PUBLIC_API(JSFlatString*)
JS_FlattenString(JSContext* cx, JSString* str);

extern JS_PUBLIC_API(const JS::Latin1Char*)
JS_GetLatin1FlatStringChars(const JS::AutoCheckCannotGC& nogc, JSFlatString* str);

extern JS_PUBLIC_API(const char16_t*)
JS_GetTwoByteFlatStringChars(const JS::AutoCheckCannotGC& nogc, JSFlatString* str);

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

/*
 * Create a dependent string, i.e., a string that owns no character storage,
 * but that refers to a slice of another string's chars.  Dependent strings
 * are mutable by definition, so the thread safety comments above apply.
 */
extern JS_PUBLIC_API(JSString*)
JS_NewDependentString(JSContext* cx, JS::HandleString str, size_t start,
                      size_t length);

/*
 * Concatenate two strings, possibly resulting in a rope.
 * See above for thread safety comments.
 */
extern JS_PUBLIC_API(JSString*)
JS_ConcatStrings(JSContext* cx, JS::HandleString left, JS::HandleString right);

/*
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

/*
 * A variation on JS_EncodeCharacters where a null terminated string is
 * returned that you are expected to call JS_free on when done.
 */
JS_PUBLIC_API(char*)
JS_EncodeString(JSContext* cx, JSString* str);

/*
 * Same behavior as JS_EncodeString(), but encode into UTF-8 string
 */
JS_PUBLIC_API(char*)
JS_EncodeStringToUTF8(JSContext* cx, JS::HandleString str);

/*
 * Get number of bytes in the string encoding (without accounting for a
 * terminating zero bytes. The function returns (size_t) -1 if the string
 * can not be encoded into bytes and reports an error using cx accordingly.
 */
JS_PUBLIC_API(size_t)
JS_GetStringEncodingLength(JSContext* cx, JSString* str);

/*
 * Encode string into a buffer. The function does not stores an additional
 * zero byte. The function returns (size_t) -1 if the string can not be
 * encoded into bytes with no error reported. Otherwise it returns the number
 * of bytes that are necessary to encode the string. If that exceeds the
 * length parameter, the string will be cut and only length bytes will be
 * written into the buffer.
 */
JS_PUBLIC_API(size_t)
JS_EncodeStringToBuffer(JSContext* cx, JSString* str, char* buffer, size_t length);

class JSAutoByteString
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
        js_free(mBytes);
    }

    /* Take ownership of the given byte array. */
    void initBytes(char* bytes) {
        MOZ_ASSERT(!mBytes);
        mBytes = bytes;
    }

    char* encodeLatin1(JSContext* cx, JSString* str) {
        MOZ_ASSERT(!mBytes);
        MOZ_ASSERT(cx);
        mBytes = JS_EncodeString(cx, str);
        return mBytes;
    }

    char* encodeLatin1(js::ExclusiveContext* cx, JSString* str);

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
    char*       mBytes;
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

/*
 * Create a new Symbol with the given description. This function never returns
 * a Symbol that is in the Runtime-wide symbol registry.
 *
 * If description is null, the new Symbol's [[Description]] attribute is
 * undefined.
 */
JS_PUBLIC_API(Symbol*)
NewSymbol(JSContext* cx, HandleString description);

/*
 * Symbol.for as specified in ES6.
 *
 * Get a Symbol with the description 'key' from the Runtime-wide symbol registry.
 * If there is not already a Symbol with that description in the registry, a new
 * Symbol is created and registered. 'key' must not be null.
 */
JS_PUBLIC_API(Symbol*)
GetSymbolFor(JSContext* cx, HandleString key);

/*
 * Get the [[Description]] attribute of the given symbol.
 *
 * This function is infallible. If it returns null, that means the symbol's
 * [[Description]] is undefined.
 */
JS_PUBLIC_API(JSString*)
GetSymbolDescription(HandleSymbol symbol);

/* Well-known symbols. */
enum class SymbolCode : uint32_t {
    iterator,                       // well-known Symbol.iterator
    InSymbolRegistry = 0xfffffffe,  // created by Symbol.for() or JS::GetSymbolFor()
    UniqueSymbol = 0xffffffff       // created by Symbol() or JS::NewSymbol()
};

/* For use in loops that iterate over the well-known symbols. */
const size_t WellKnownSymbolLimit = 1;

/*
 * Return the SymbolCode telling what sort of symbol `symbol` is.
 *
 * A symbol's SymbolCode never changes once it is created.
 */
JS_PUBLIC_API(SymbolCode)
GetSymbolCode(Handle<Symbol*> symbol);

/*
 * Get one of the well-known symbols defined by ES6. A single set of well-known
 * symbols is shared by all compartments in a JSRuntime.
 *
 * `which` must be in the range [0, WellKnownSymbolLimit).
 */
JS_PUBLIC_API(Symbol*)
GetWellKnownSymbol(JSContext* cx, SymbolCode which);

/*
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

/*
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

/*
 * JSON.stringify as specified by ES5.
 */
JS_PUBLIC_API(bool)
JS_Stringify(JSContext* cx, JS::MutableHandleValue value, JS::HandleObject replacer,
             JS::HandleValue space, JSONWriteCallback callback, void* data);

/*
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

/*
 * The default locale for the ECMAScript Internationalization API
 * (Intl.Collator, Intl.NumberFormat, Intl.DateTimeFormat).
 * Note that the Internationalization API encourages clients to
 * specify their own locales.
 * The locale string remains owned by the caller.
 */
extern JS_PUBLIC_API(bool)
JS_SetDefaultLocale(JSRuntime* rt, const char* locale);

/*
 * Reset the default locale to OS defaults.
 */
extern JS_PUBLIC_API(void)
JS_ResetDefaultLocale(JSRuntime* rt);

/*
 * Locale specific string conversion and error message callbacks.
 */
struct JSLocaleCallbacks {
    JSLocaleToUpperCase     localeToUpperCase;
    JSLocaleToLowerCase     localeToLowerCase;
    JSLocaleCompare         localeCompare; // not used #if EXPOSE_INTL_API
    JSLocaleToUnicode       localeToUnicode;
};

/*
 * Establish locale callbacks. The pointer must persist as long as the
 * JSRuntime.  Passing nullptr restores the default behaviour.
 */
extern JS_PUBLIC_API(void)
JS_SetLocaleCallbacks(JSRuntime* rt, const JSLocaleCallbacks* callbacks);

/*
 * Return the address of the current locale callbacks struct, which may
 * be nullptr.
 */
extern JS_PUBLIC_API(const JSLocaleCallbacks*)
JS_GetLocaleCallbacks(JSRuntime* rt);

/************************************************************************/

/*
 * Error reporting.
 */

/*
 * Report an exception represented by the sprintf-like conversion of format
 * and its arguments.  This exception message string is passed to a pre-set
 * JSErrorReporter function (set by JS_SetErrorReporter).
 */
extern JS_PUBLIC_API(void)
JS_ReportError(JSContext* cx, const char* format, ...);

/*
 * Use an errorNumber to retrieve the format string, args are char*
 */
extern JS_PUBLIC_API(void)
JS_ReportErrorNumber(JSContext* cx, JSErrorCallback errorCallback,
                     void* userRef, const unsigned errorNumber, ...);

#ifdef va_start
extern JS_PUBLIC_API(void)
JS_ReportErrorNumberVA(JSContext* cx, JSErrorCallback errorCallback,
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

/*
 * As above, but report a warning instead (JSREPORT_IS_WARNING(report.flags)).
 * Return true if there was no error trying to issue the warning, and if the
 * warning was not converted into an error due to the JSOPTION_WERROR option
 * being set, false otherwise.
 */
extern JS_PUBLIC_API(bool)
JS_ReportWarning(JSContext* cx, const char* format, ...);

extern JS_PUBLIC_API(bool)
JS_ReportErrorFlagsAndNumber(JSContext* cx, unsigned flags,
                             JSErrorCallback errorCallback, void* userRef,
                             const unsigned errorNumber, ...);

extern JS_PUBLIC_API(bool)
JS_ReportErrorFlagsAndNumberUC(JSContext* cx, unsigned flags,
                               JSErrorCallback errorCallback, void* userRef,
                               const unsigned errorNumber, ...);

/*
 * Complain when out of memory.
 */
extern JS_PUBLIC_API(void)
JS_ReportOutOfMemory(JSContext* cx);

/*
 * Complain when an allocation size overflows the maximum supported limit.
 */
extern JS_PUBLIC_API(void)
JS_ReportAllocationOverflow(JSContext* cx);

class JSErrorReport
{
  public:
    JSErrorReport()
      : filename(nullptr), lineno(0), column(0), isMuted(false), linebuf(nullptr),
        tokenptr(nullptr), uclinebuf(nullptr), uctokenptr(nullptr), flags(0), errorNumber(0),
        ucmessage(nullptr), messageArgs(nullptr), exnType(0)
    {}

    const char*     filename;      /* source file name, URL, etc., or null */
    unsigned        lineno;         /* source line number */
    unsigned        column;         /* zero-based column index in line */
    bool            isMuted;        /* See the comment in ReadOnlyCompileOptions. */
    const char*     linebuf;       /* offending source line without final \n */
    const char*     tokenptr;      /* pointer to error token in linebuf */
    const char16_t* uclinebuf;     /* unicode (original) line buffer */
    const char16_t* uctokenptr;    /* unicode (original) token pointer */
    unsigned        flags;          /* error/warning, etc. */
    unsigned        errorNumber;    /* the error number, e.g. see js.msg */
    const char16_t* ucmessage;     /* the (default) error message */
    const char16_t** messageArgs;  /* arguments for the error message */
    int16_t         exnType;        /* One of the JSExnType constants */
};

/*
 * JSErrorReport flag values.  These may be freely composed.
 */
#define JSREPORT_ERROR      0x0     /* pseudo-flag for default case */
#define JSREPORT_WARNING    0x1     /* reported via JS_ReportWarning */
#define JSREPORT_EXCEPTION  0x2     /* exception was thrown */
#define JSREPORT_STRICT     0x4     /* error or warning due to strict option */

/*
 * This condition is an error in strict mode code, a warning if
 * JS_HAS_STRICT_OPTION(cx), and otherwise should not be reported at
 * all.  We check the strictness of the context's top frame's script;
 * where that isn't appropriate, the caller should do the right checks
 * itself instead of using this flag.
 */
#define JSREPORT_STRICT_MODE_ERROR 0x8

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
#define JSREPORT_IS_STRICT_MODE_ERROR(flags) (((flags) &                      \
                                              JSREPORT_STRICT_MODE_ERROR) != 0)
extern JS_PUBLIC_API(JSErrorReporter)
JS_GetErrorReporter(JSRuntime* rt);

extern JS_PUBLIC_API(JSErrorReporter)
JS_SetErrorReporter(JSRuntime* rt, JSErrorReporter er);

namespace JS {

extern JS_PUBLIC_API(bool)
CreateError(JSContext* cx, JSExnType type, HandleString stack,
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
MapClear(JSContext* cx, HandleObject obj);

extern JS_PUBLIC_API(bool)
MapKeys(JSContext* cx, HandleObject obj, MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
MapValues(JSContext* cx, HandleObject obj, MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
MapEntries(JSContext* cx, HandleObject obj, MutableHandleValue rval);

} /* namespace JS */

/*
 * Dates.
 */

extern JS_PUBLIC_API(JSObject*)
JS_NewDateObject(JSContext* cx, int year, int mon, int mday, int hour, int min, int sec);

extern JS_PUBLIC_API(JSObject*)
JS_NewDateObjectMsec(JSContext* cx, double msec);

/*
 * Infallible predicate to test whether obj is a date object.
 */
extern JS_PUBLIC_API(bool)
JS_ObjectIsDate(JSContext* cx, JS::HandleObject obj);

/*
 * Clears the cache of calculated local time from each Date object.
 * Call to propagate a system timezone change.
 */
extern JS_PUBLIC_API(void)
JS_ClearDateCaches(JSContext* cx);

/************************************************************************/

/*
 * Regular Expressions.
 */
#define JSREG_FOLD      0x01u   /* fold uppercase to lowercase */
#define JSREG_GLOB      0x02u   /* global exec, creates array of matches */
#define JSREG_MULTILINE 0x04u   /* treat ^ and $ as begin and end of line */
#define JSREG_STICKY    0x08u   /* only match starting at lastIndex */

extern JS_PUBLIC_API(JSObject*)
JS_NewRegExpObject(JSContext* cx, JS::HandleObject obj, const char* bytes, size_t length,
                   unsigned flags);

extern JS_PUBLIC_API(JSObject*)
JS_NewUCRegExpObject(JSContext* cx, JS::HandleObject obj, const char16_t* chars, size_t length,
                     unsigned flags);

extern JS_PUBLIC_API(bool)
JS_SetRegExpInput(JSContext* cx, JS::HandleObject obj, JS::HandleString input,
                  bool multiline);

extern JS_PUBLIC_API(bool)
JS_ClearRegExpStatics(JSContext* cx, JS::HandleObject obj);

extern JS_PUBLIC_API(bool)
JS_ExecuteRegExp(JSContext* cx, JS::HandleObject obj, JS::HandleObject reobj,
                 char16_t* chars, size_t length, size_t* indexp, bool test,
                 JS::MutableHandleValue rval);

/* RegExp interface for clients without a global object. */

extern JS_PUBLIC_API(JSObject*)
JS_NewRegExpObjectNoStatics(JSContext* cx, char* bytes, size_t length, unsigned flags);

extern JS_PUBLIC_API(JSObject*)
JS_NewUCRegExpObjectNoStatics(JSContext* cx, char16_t* chars, size_t length, unsigned flags);

extern JS_PUBLIC_API(bool)
JS_ExecuteRegExpNoStatics(JSContext* cx, JS::HandleObject reobj, char16_t* chars, size_t length,
                          size_t* indexp, bool test, JS::MutableHandleValue rval);

extern JS_PUBLIC_API(bool)
JS_ObjectIsRegExp(JSContext* cx, JS::HandleObject obj);

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

extern JS_PUBLIC_API(bool)
JS_ReportPendingException(JSContext* cx);

namespace JS {

/*
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

/*
 * If the given object is an exception object, the exception will have (or be
 * able to lazily create) an error report struct, and this function will return
 * the address of that struct.  Otherwise, it returns nullptr. The lifetime
 * of the error report struct that might be returned is the same as the
 * lifetime of the exception object.
 */
extern JS_PUBLIC_API(JSErrorReport*)
JS_ErrorFromException(JSContext* cx, JS::HandleObject obj);

/*
 * Throws a StopIteration exception on cx.
 */
extern JS_PUBLIC_API(bool)
JS_ThrowStopIteration(JSContext* cx);

extern JS_PUBLIC_API(bool)
JS_IsStopIteration(jsval v);

extern JS_PUBLIC_API(intptr_t)
JS_GetCurrentThread();

/*
 * A JS runtime always has an "owner thread". The owner thread is set when the
 * runtime is created (to the current thread) and practically all entry points
 * into the JS engine check that a runtime (or anything contained in the
 * runtime: context, compartment, object, etc) is only touched by its owner
 * thread. Embeddings may check this invariant outside the JS engine by calling
 * JS_AbortIfWrongThread (which will abort if not on the owner thread, even for
 * non-debug builds).
 */

extern JS_PUBLIC_API(void)
JS_AbortIfWrongThread(JSRuntime* rt);

/************************************************************************/

/*
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
JS_GetGCZeal(JSContext* cx, uint8_t* zeal, uint32_t* frequency, uint32_t* nextScheduled);

extern JS_PUBLIC_API(void)
JS_SetGCZeal(JSContext* cx, uint8_t zeal, uint32_t frequency);

extern JS_PUBLIC_API(void)
JS_ScheduleGC(JSContext* cx, uint32_t count);
#endif

extern JS_PUBLIC_API(void)
JS_SetParallelParsingEnabled(JSRuntime* rt, bool enabled);

extern JS_PUBLIC_API(void)
JS_SetOffthreadIonCompilationEnabled(JSRuntime* rt, bool enabled);

#define JIT_COMPILER_OPTIONS(Register)                                     \
    Register(BASELINE_WARMUP_TRIGGER, "baseline.warmup.trigger")           \
    Register(ION_WARMUP_TRIGGER, "ion.warmup.trigger")                     \
    Register(ION_GVN_ENABLE, "ion.gvn.enable")                             \
    Register(ION_ENABLE, "ion.enable")                                     \
    Register(BASELINE_ENABLE, "baseline.enable")                           \
    Register(OFFTHREAD_COMPILATION_ENABLE, "offthread-compilation.enable") \
    Register(SIGNALS_ENABLE, "signals.enable")

typedef enum JSJitCompilerOption {
#define JIT_COMPILER_DECLARE(key, str) \
    JSJITCOMPILER_ ## key,

    JIT_COMPILER_OPTIONS(JIT_COMPILER_DECLARE)
#undef JIT_COMPILER_DECLARE

    JSJITCOMPILER_NOT_AN_OPTION
} JSJitCompilerOption;

extern JS_PUBLIC_API(void)
JS_SetGlobalJitCompilerOption(JSRuntime* rt, JSJitCompilerOption opt, uint32_t value);
extern JS_PUBLIC_API(int)
JS_GetGlobalJitCompilerOption(JSRuntime* rt, JSJitCompilerOption opt);

/*
 * Convert a uint32_t index into a jsid.
 */
extern JS_PUBLIC_API(bool)
JS_IndexToId(JSContext* cx, uint32_t index, JS::MutableHandleId);

/*
 * Convert chars into a jsid.
 *
 * |chars| may not be an index.
 */
extern JS_PUBLIC_API(bool)
JS_CharsToId(JSContext* cx, JS::TwoByteChars chars, JS::MutableHandleId);

/*
 *  Test if the given string is a valid ECMAScript identifier
 */
extern JS_PUBLIC_API(bool)
JS_IsIdentifier(JSContext* cx, JS::HandleString str, bool* isIdentifier);

/*
 * Test whether the given chars + length are a valid ECMAScript identifier.
 * This version is infallible, so just returns whether the chars are an
 * identifier.
 */
extern JS_PUBLIC_API(bool)
JS_IsIdentifier(const char16_t* chars, size_t length);

namespace JS {

/*
 * AutoFilename encapsulates a pointer to a C-string and keeps the C-string
 * alive for as long as the associated AutoFilename object is alive.
 */
class MOZ_STACK_CLASS JS_PUBLIC_API(AutoFilename)
{
    void* scriptSource_;

    AutoFilename(const AutoFilename&) = delete;
    void operator=(const AutoFilename&) = delete;

  public:
    AutoFilename() : scriptSource_(nullptr) {}
    ~AutoFilename() { reset(nullptr); }

    const char* get() const;

    void reset(void* newScriptSource);
};

/*
 * Return the current filename and line number of the most currently running
 * frame. Returns true if a scripted frame was found, false otherwise.
 *
 * If a the embedding has hidden the scripted caller for the topmost activation
 * record, this will also return false.
 */
extern JS_PUBLIC_API(bool)
DescribeScriptedCaller(JSContext* cx, AutoFilename* filename = nullptr,
                       unsigned* lineno = nullptr);

extern JS_PUBLIC_API(JSObject*)
GetScriptedCallerGlobal(JSContext* cx);

/*
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

class AutoHideScriptedCaller
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

} /* namespace JS */

/*
 * Encode/Decode interpreted scripts and functions to/from memory.
 */

extern JS_PUBLIC_API(void*)
JS_EncodeScript(JSContext* cx, JS::HandleScript script, uint32_t* lengthp);

extern JS_PUBLIC_API(void*)
JS_EncodeInterpretedFunction(JSContext* cx, JS::HandleObject funobj, uint32_t* lengthp);

extern JS_PUBLIC_API(JSScript*)
JS_DecodeScript(JSContext* cx, const void* data, uint32_t length);

extern JS_PUBLIC_API(JSObject*)
JS_DecodeInterpretedFunction(JSContext* cx, const void* data, uint32_t length);

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

/* The list of reasons why an asm.js module may not be stored in the cache. */
enum AsmJSCacheResult
{
    AsmJSCache_MIN,
    AsmJSCache_Success = AsmJSCache_MIN,
    AsmJSCache_ModuleTooSmall,
    AsmJSCache_SynchronousScript,
    AsmJSCache_QuotaExceeded,
    AsmJSCache_Disabled_Internal,
    AsmJSCache_Disabled_ShellFlags,
    AsmJSCache_Disabled_JitInspector,
    AsmJSCache_InternalError,
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
 *
 * If 'installed' is true, then the cache entry is associated with a permanently
 * installed JS file (e.g., in a packaged webapp). This information allows the
 * embedding to store the cache entry in a installed location associated with
 * the principal of 'global' where it will not be evicted until the associated
 * installed JS file is removed.
 */
typedef AsmJSCacheResult
(* OpenAsmJSCacheEntryForWriteOp)(HandleObject global, bool installed,
                                  const char16_t* begin, const char16_t* end,
                                  size_t size, uint8_t** memory, intptr_t* handle);
typedef void
(* CloseAsmJSCacheEntryForWriteOp)(size_t size, uint8_t* memory, intptr_t handle);

typedef js::Vector<char, 0, js::SystemAllocPolicy> BuildIdCharVector;

// Return the buildId (represented as a sequence of characters) associated with
// the currently-executing build. If the JS engine is embedded such that a
// single cache entry can be observed by different compiled versions of the JS
// engine, it is critical that the buildId shall change for each new build of
// the JS engine.

typedef bool
(* BuildIdOp)(BuildIdCharVector* buildId);

struct AsmJSCacheOps
{
    OpenAsmJSCacheEntryForReadOp openEntryForRead;
    CloseAsmJSCacheEntryForReadOp closeEntryForRead;
    OpenAsmJSCacheEntryForWriteOp openEntryForWrite;
    CloseAsmJSCacheEntryForWriteOp closeEntryForWrite;
    BuildIdOp buildId;
};

extern JS_PUBLIC_API(void)
SetAsmJSCacheOps(JSRuntime* rt, const AsmJSCacheOps* callbacks);

/*
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
     *      index - fixed to NOT_ARRAY (== UINT32_MAX)
     *
     *  Case 2: Optimized Array Iteration
     *      iterator - pointer to the array object.
     *      index - current position in array.
     *
     * The cases are distinguished by whether or not |index| is equal to NOT_ARRAY.
     */
    JS::RootedObject iterator;
    uint32_t index;

    static const uint32_t NOT_ARRAY = UINT32_MAX;

    ForOfIterator(const ForOfIterator&) = delete;
    ForOfIterator& operator=(const ForOfIterator&) = delete;

  public:
    explicit ForOfIterator(JSContext* cx) : cx_(cx), iterator(cx_), index(NOT_ARRAY) { }

    enum NonIterableBehavior {
        ThrowOnNonIterable,
        AllowNonIterable
    };

    /*
     * Initialize the iterator.  If AllowNonIterable is passed then if getting
     * the @@iterator property from iterable returns undefined init() will just
     * return true instead of throwing.  Callers must then check
     * valueIsIterable() before continuing with the iteration.
     */
    bool init(JS::HandleValue iterable,
              NonIterableBehavior nonIterableBehavior = ThrowOnNonIterable);

    /*
     * This method assumes that |iterator| is already an iterator.  It will not
     * check for, and call @@iterator.  Callers should make sure that the passed
     * in value is in fact an iterator.
     */
    bool initWithIterator(JS::HandleValue aIterator);

    /*
     * Get the next value from the iterator.  If false *done is true
     * after this call, do not examine val.
     */
    bool next(JS::MutableHandleValue val, bool* done);

    /*
     * If initialized with throwOnNonCallable = false, check whether
     * the value is iterable.
     */
    bool valueIsIterable() const {
        return iterator;
    }

  private:
    inline bool nextFromOptimizedArray(MutableHandleValue val, bool* done);
    bool materializeArrayIterator();
};


/*
 * If a large allocation fails when calling pod_{calloc,realloc}CanGC, the JS
 * engine may call the large-allocation- failure callback, if set, to allow the
 * embedding to flush caches, possibly perform shrinking GCs, etc. to make some
 * room. The allocation will then be retried (and may still fail.)
 */

typedef void
(* LargeAllocationFailureCallback)(void* data);

extern JS_PUBLIC_API(void)
SetLargeAllocationFailureCallback(JSRuntime* rt, LargeAllocationFailureCallback afc, void* data);

/*
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
SetOutOfMemoryCallback(JSRuntime* rt, OutOfMemoryCallback cb, void* data);


/*
 * Capture the current call stack as a chain of SavedFrame objects, and set
 * |stackp| to the SavedFrame for the newest stack frame. If |maxFrameCount| is
 * non-zero, capture at most the youngest |maxFrameCount| frames.
 */
extern JS_PUBLIC_API(bool)
CaptureCurrentStack(JSContext* cx, MutableHandleObject stackp, unsigned maxFrameCount = 0);

} /* namespace JS */

#endif /* jsapi_h */
