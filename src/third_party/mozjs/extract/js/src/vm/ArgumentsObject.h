/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArgumentsObject_h
#define vm_ArgumentsObject_h

#include "mozilla/MemoryReporting.h"

#include "gc/Barrier.h"
#include "vm/NativeObject.h"

namespace js {

class AbstractFramePtr;
class ScriptFrameIter;

namespace jit {
class JitFrameLayout;
} // namespace jit

// RareArgumentsData stores the deleted-elements bits for an arguments object.
// Because |delete arguments[i]| is uncommon, we allocate this data the first
// time an element is deleted.
class RareArgumentsData
{
    // Pointer to an array of bits indicating, for every argument in
    // [0, initialLength) whether the element has been deleted. See
    // ArgumentsObject::isElementDeleted comment.
    size_t deletedBits_[1];

    RareArgumentsData() = default;
    RareArgumentsData(const RareArgumentsData&) = delete;
    void operator=(const RareArgumentsData&) = delete;

  public:
    static RareArgumentsData* create(JSContext* cx, ArgumentsObject* obj);
    static size_t bytesRequired(size_t numActuals);

    bool isAnyElementDeleted(size_t len) const {
        return IsAnyBitArrayElementSet(deletedBits_, len);
    }
    bool isElementDeleted(size_t len, size_t i) const {
        MOZ_ASSERT(i < len);
        return IsBitArrayElementSet(deletedBits_, len, i);
    }
    void markElementDeleted(size_t len, size_t i) {
        MOZ_ASSERT(i < len);
        SetBitArrayElement(deletedBits_, len, i);
    }
};

// ArgumentsData stores the initial indexed arguments provided to a function
// call. It is used to store arguments[i] -- up until the corresponding
// property is modified, when the relevant value is flagged to memorialize the
// modification.
struct ArgumentsData
{
    /*
     * numArgs = Max(numFormalArgs, numActualArgs)
     * The array 'args' has numArgs elements.
     */
    uint32_t    numArgs;

    RareArgumentsData* rareData;

    /*
     * This array holds either the current argument value or the magic
     * forwarding value. The latter means that the function has both a
     * CallObject and an ArgumentsObject AND the particular formal variable is
     * aliased by the CallObject. In such cases, the CallObject holds the
     * canonical value so any element access to the arguments object should load
     * the value out of the CallObject (which is pointed to by MAYBE_CALL_SLOT).
     */
    GCPtrValue args[1];

    /* For jit use: */
    static ptrdiff_t offsetOfArgs() { return offsetof(ArgumentsData, args); }

    /* Iterate args. */
    GCPtrValue* begin() { return args; }
    const GCPtrValue* begin() const { return args; }
    GCPtrValue* end() { return args + numArgs; }
    const GCPtrValue* end() const { return args + numArgs; }

    static size_t bytesRequired(size_t numArgs) {
        return offsetof(ArgumentsData, args) + numArgs * sizeof(Value);
    }
};

// Maximum supported value of arguments.length. This bounds the maximum
// number of arguments that can be supplied to Function.prototype.apply.
// This value also bounds the number of elements parsed in an array
// initializer.
// NB: keep this in sync with the copy in builtin/SelfHostingDefines.h.
static const unsigned ARGS_LENGTH_MAX = 500 * 1000;

/*
 * ArgumentsObject instances represent |arguments| objects created to store
 * function arguments when a function is called.  It's expensive to create such
 * objects if they're never used, so they're only created when they are
 * potentially used.
 *
 * Arguments objects are complicated because, for non-strict mode code, they
 * must alias any named arguments which were provided to the function.  Gnarly
 * example:
 *
 *   function f(a, b, c, d)
 *   {
 *     arguments[0] = "seta";
 *     assertEq(a, "seta");
 *     b = "setb";
 *     assertEq(arguments[1], "setb");
 *     c = "setc";
 *     assertEq(arguments[2], undefined);
 *     arguments[3] = "setd";
 *     assertEq(d, undefined);
 *   }
 *   f("arga", "argb");
 *
 * ES5's strict mode behaves more sanely, and named arguments don't alias
 * elements of an arguments object.
 *
 * ArgumentsObject instances use the following reserved slots:
 *
 *   INITIAL_LENGTH_SLOT
 *     Stores the initial value of arguments.length, plus a bit indicating
 *     whether arguments.length and/or arguments[@@iterator] have been
 *     modified.  Use initialLength(), hasOverriddenLength(), and
 *     hasOverriddenIterator() to access these values.  If arguments.length has
 *     been modified, then the current value of arguments.length is stored in
 *     another slot associated with a new property.
 *   DATA_SLOT
 *     Stores an ArgumentsData*, described above.
 *   MAYBE_CALL_SLOT
 *     Stores the CallObject, if the callee has aliased bindings. See
 *     the ArgumentsData::args comment.
 *   CALLEE_SLOT
 *     Stores the initial arguments.callee. This value can be overridden on
 *     mapped arguments objects, see hasOverriddenCallee.
 */
class ArgumentsObject : public NativeObject
{
  protected:
    static const uint32_t INITIAL_LENGTH_SLOT = 0;
    static const uint32_t DATA_SLOT = 1;
    static const uint32_t MAYBE_CALL_SLOT = 2;
    static const uint32_t CALLEE_SLOT = 3;

  public:
    static const uint32_t LENGTH_OVERRIDDEN_BIT = 0x1;
    static const uint32_t ITERATOR_OVERRIDDEN_BIT = 0x2;
    static const uint32_t ELEMENT_OVERRIDDEN_BIT = 0x4;
    static const uint32_t CALLEE_OVERRIDDEN_BIT = 0x8;
    static const uint32_t PACKED_BITS_COUNT = 4;

    static_assert(ARGS_LENGTH_MAX <= (UINT32_MAX >> PACKED_BITS_COUNT),
                  "Max arguments length must fit in available bits");

  protected:
    template <typename CopyArgs>
    static ArgumentsObject* create(JSContext* cx, HandleFunction callee, unsigned numActuals,
                                   CopyArgs& copy);

    ArgumentsData* data() const {
        return reinterpret_cast<ArgumentsData*>(getFixedSlot(DATA_SLOT).toPrivate());
    }

    RareArgumentsData* maybeRareData() const {
        return data()->rareData;
    }

    MOZ_MUST_USE bool createRareData(JSContext* cx);

    RareArgumentsData* getOrCreateRareData(JSContext* cx) {
        if (!data()->rareData && !createRareData(cx))
            return nullptr;
        return data()->rareData;
    }

    static bool obj_delProperty(JSContext* cx, HandleObject obj, HandleId id,
                                ObjectOpResult& result);

    static bool obj_mayResolve(const JSAtomState& names, jsid id, JSObject*);

  public:
    static const uint32_t RESERVED_SLOTS = 4;
    static const gc::AllocKind FINALIZE_KIND = gc::AllocKind::OBJECT4_BACKGROUND;

    /* Create an arguments object for a frame that is expecting them. */
    static ArgumentsObject* createExpected(JSContext* cx, AbstractFramePtr frame);

    /*
     * Purposefully disconnect the returned arguments object from the frame
     * by always creating a new copy that does not alias formal parameters.
     * This allows function-local analysis to determine that formals are
     * not aliased and generally simplifies arguments objects.
     */
    static ArgumentsObject* createUnexpected(JSContext* cx, ScriptFrameIter& iter);
    static ArgumentsObject* createUnexpected(JSContext* cx, AbstractFramePtr frame);
    static ArgumentsObject* createForIon(JSContext* cx, jit::JitFrameLayout* frame,
                                         HandleObject scopeChain);

    /*
     * Allocate ArgumentsData and fill reserved slots after allocating an
     * ArgumentsObject in Ion code.
     */
    static ArgumentsObject* finishForIon(JSContext* cx, jit::JitFrameLayout* frame,
                                         JSObject* scopeChain, ArgumentsObject* obj);

    static ArgumentsObject* createTemplateObject(JSContext* cx, bool mapped);

    /*
     * Return the initial length of the arguments.  This may differ from the
     * current value of arguments.length!
     */
    uint32_t initialLength() const {
        uint32_t argc = uint32_t(getFixedSlot(INITIAL_LENGTH_SLOT).toInt32()) >> PACKED_BITS_COUNT;
        MOZ_ASSERT(argc <= ARGS_LENGTH_MAX);
        return argc;
    }

    /* True iff arguments.length has been assigned or its attributes changed. */
    bool hasOverriddenLength() const {
        const Value& v = getFixedSlot(INITIAL_LENGTH_SLOT);
        return v.toInt32() & LENGTH_OVERRIDDEN_BIT;
    }

    void markLengthOverridden() {
        uint32_t v = getFixedSlot(INITIAL_LENGTH_SLOT).toInt32() | LENGTH_OVERRIDDEN_BIT;
        setFixedSlot(INITIAL_LENGTH_SLOT, Int32Value(v));
    }

    /*
     * Create the default "length" property and set LENGTH_OVERRIDDEN_BIT.
     */
    static bool reifyLength(JSContext* cx, Handle<ArgumentsObject*> obj);

    /* True iff arguments[@@iterator] has been assigned or its attributes
     * changed. */
    bool hasOverriddenIterator() const {
        const Value& v = getFixedSlot(INITIAL_LENGTH_SLOT);
        return v.toInt32() & ITERATOR_OVERRIDDEN_BIT;
    }

    void markIteratorOverridden() {
        uint32_t v = getFixedSlot(INITIAL_LENGTH_SLOT).toInt32() | ITERATOR_OVERRIDDEN_BIT;
        setFixedSlot(INITIAL_LENGTH_SLOT, Int32Value(v));
    }

    /*
     * Create the default @@iterator property and set ITERATOR_OVERRIDDEN_BIT.
     */
    static bool reifyIterator(JSContext* cx, Handle<ArgumentsObject*> obj);

    /* True iff any element has been assigned or its attributes
     * changed. */
    bool hasOverriddenElement() const {
        const Value& v = getFixedSlot(INITIAL_LENGTH_SLOT);
        return v.toInt32() & ELEMENT_OVERRIDDEN_BIT;
    }

    void markElementOverridden() {
        uint32_t v = getFixedSlot(INITIAL_LENGTH_SLOT).toInt32() | ELEMENT_OVERRIDDEN_BIT;
        setFixedSlot(INITIAL_LENGTH_SLOT, Int32Value(v));
    }

    /*
     * Because the arguments object is a real object, its elements may be
     * deleted. This is implemented by setting a 'deleted' flag for the arg
     * which is read by argument object resolve and getter/setter hooks.
     *
     * NB: an element, once deleted, stays deleted. Thus:
     *
     *   function f(x) { delete arguments[0]; arguments[0] = 42; return x }
     *   assertEq(f(1), 1);
     *
     * This works because, once a property is deleted from an arguments object,
     * it gets regular properties with regular getters/setters that don't alias
     * ArgumentsData::slots.
     */
    bool isElementDeleted(uint32_t i) const {
        MOZ_ASSERT(i < data()->numArgs);
        if (i >= initialLength())
            return false;
        return maybeRareData() && maybeRareData()->isElementDeleted(initialLength(), i);
    }

    bool isAnyElementDeleted() const {
        return maybeRareData() && maybeRareData()->isAnyElementDeleted(initialLength());
    }

    bool markElementDeleted(JSContext* cx, uint32_t i);

    /*
     * An ArgumentsObject serves two roles:
     *  - a real object, accessed through regular object operations, e.g..,
     *    GetElement corresponding to 'arguments[i]';
     *  - a VM-internal data structure, storing the value of arguments (formal
     *    and actual) that are accessed directly by the VM when a reading the
     *    value of a formal parameter.
     * There are two ways to access the ArgumentsData::args corresponding to
     * these two use cases:
     *  - object access should use elements(i) which will take care of
     *    forwarding when the value is the magic forwarding value;
     *  - VM argument access should use arg(i) which will assert that the
     *    value is not the magic forwarding value (since, if such forwarding was
     *    needed, the frontend should have emitted JSOP_GETALIASEDVAR).
     */
    const Value& element(uint32_t i) const;

    inline void setElement(JSContext* cx, uint32_t i, const Value& v);

    const Value& arg(unsigned i) const {
        MOZ_ASSERT(i < data()->numArgs);
        const Value& v = data()->args[i];
        MOZ_ASSERT(!v.isMagic());
        return v;
    }

    void setArg(unsigned i, const Value& v) {
        MOZ_ASSERT(i < data()->numArgs);
        GCPtrValue& lhs = data()->args[i];
        MOZ_ASSERT(!lhs.isMagic());
        lhs = v;
    }

    /*
     * Attempt to speedily and efficiently access the i-th element of this
     * arguments object.  Return true if the element was speedily returned.
     * Return false if the element must be looked up more slowly using
     * getProperty or some similar method. The second overload copies the
     * elements [start, start + count) into the locations starting at 'vp'.
     *
     * NB: Returning false does not indicate error!
     */
    bool maybeGetElement(uint32_t i, MutableHandleValue vp) {
        if (i >= initialLength() || isElementDeleted(i))
            return false;
        vp.set(element(i));
        return true;
    }

    inline bool maybeGetElements(uint32_t start, uint32_t count, js::Value* vp);

    /*
     * Measures things hanging off this ArgumentsObject that are counted by the
     * |miscSize| argument in JSObject::sizeOfExcludingThis().
     */
    size_t sizeOfMisc(mozilla::MallocSizeOf mallocSizeOf) const {
        if (!data()) // Template arguments objects have no data.
            return 0;
        return mallocSizeOf(data()) + mallocSizeOf(maybeRareData());
    }
    size_t sizeOfData() const {
        return ArgumentsData::bytesRequired(data()->numArgs) +
               (maybeRareData() ? RareArgumentsData::bytesRequired(initialLength()) : 0);
    }

    static void finalize(FreeOp* fop, JSObject* obj);
    static void trace(JSTracer* trc, JSObject* obj);
    static size_t objectMoved(JSObject* dst, JSObject* src);

    /* For jit use: */
    static size_t getDataSlotOffset() {
        return getFixedSlotOffset(DATA_SLOT);
    }
    static size_t getInitialLengthSlotOffset() {
        return getFixedSlotOffset(INITIAL_LENGTH_SLOT);
    }

    static Value MagicEnvSlotValue(uint32_t slot) {
        // When forwarding slots to a backing CallObject, the slot numbers are
        // stored as uint32 magic values. This raises an ambiguity if we have
        // also copied JS_OPTIMIZED_OUT magic from a JIT frame or
        // JS_UNINITIALIZED_LEXICAL magic on the CallObject. To distinguish
        // normal magic values (those with a JSWhyMagic) and uint32 magic
        // values, we add the maximum JSWhyMagic value to the slot
        // number. This is safe as ARGS_LENGTH_MAX is well below UINT32_MAX.
        JS_STATIC_ASSERT(UINT32_MAX - JS_WHY_MAGIC_COUNT > ARGS_LENGTH_MAX);
        return JS::MagicValueUint32(slot + JS_WHY_MAGIC_COUNT);
    }
    static uint32_t SlotFromMagicScopeSlotValue(const Value& v) {
        JS_STATIC_ASSERT(UINT32_MAX - JS_WHY_MAGIC_COUNT > ARGS_LENGTH_MAX);
        return v.magicUint32() - JS_WHY_MAGIC_COUNT;
    }
    static bool IsMagicScopeSlotValue(const Value& v) {
        return v.isMagic() && v.magicUint32() > JS_WHY_MAGIC_COUNT;
    }

    static void MaybeForwardToCallObject(AbstractFramePtr frame, ArgumentsObject* obj,
                                         ArgumentsData* data);
    static void MaybeForwardToCallObject(jit::JitFrameLayout* frame, HandleObject callObj,
                                         ArgumentsObject* obj, ArgumentsData* data);
};

class MappedArgumentsObject : public ArgumentsObject
{
    static const ClassOps classOps_;
    static const ClassExtension classExt_;
    static const ObjectOps objectOps_;

  public:
    static const Class class_;

    JSFunction& callee() const {
        return getFixedSlot(CALLEE_SLOT).toObject().as<JSFunction>();
    }

    bool hasOverriddenCallee() const {
        const Value& v = getFixedSlot(INITIAL_LENGTH_SLOT);
        return v.toInt32() & CALLEE_OVERRIDDEN_BIT;
    }

    void markCalleeOverridden() {
        uint32_t v = getFixedSlot(INITIAL_LENGTH_SLOT).toInt32() | CALLEE_OVERRIDDEN_BIT;
        setFixedSlot(INITIAL_LENGTH_SLOT, Int32Value(v));
    }

  private:
    static bool obj_enumerate(JSContext* cx, HandleObject obj);
    static bool obj_resolve(JSContext* cx, HandleObject obj, HandleId id, bool* resolvedp);
    static bool obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   Handle<JS::PropertyDescriptor> desc, ObjectOpResult& result);
};

class UnmappedArgumentsObject : public ArgumentsObject
{
    static const ClassOps classOps_;
    static const ClassExtension classExt_;

  public:
    static const Class class_;

  private:
    static bool obj_enumerate(JSContext* cx, HandleObject obj);
    static bool obj_resolve(JSContext* cx, HandleObject obj, HandleId id, bool* resolvedp);
};

} // namespace js

template<>
inline bool
JSObject::is<js::ArgumentsObject>() const
{
    return is<js::MappedArgumentsObject>() || is<js::UnmappedArgumentsObject>();
}

#endif /* vm_ArgumentsObject_h */
