/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SavedFrame_h
#define vm_SavedFrame_h

#include "mozilla/Attributes.h"

#include "js/GCHashTable.h"
#include "js/UbiNode.h"
#include "js/Wrapper.h"

namespace js {

class SavedFrame : public NativeObject {
    friend class SavedStacks;
    friend struct ::JSStructuredCloneReader;

    static const ClassSpec      classSpec_;

  public:
    static const Class          class_;
    static const JSPropertySpec protoAccessors[];
    static const JSFunctionSpec protoFunctions[];
    static const JSFunctionSpec staticFunctions[];

    // Prototype methods and properties to be exposed to JS.
    static bool construct(JSContext* cx, unsigned argc, Value* vp);
    static bool sourceProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool lineProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool columnProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool functionDisplayNameProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool asyncCauseProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool asyncParentProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool parentProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool toStringMethod(JSContext* cx, unsigned argc, Value* vp);

    static void finalize(FreeOp* fop, JSObject* obj);

    // Convenient getters for SavedFrame's reserved slots for use from C++.
    JSAtom*       getSource();
    uint32_t      getLine();
    uint32_t      getColumn();
    JSAtom*       getFunctionDisplayName();
    JSAtom*       getAsyncCause();
    SavedFrame*   getParent() const;
    JSPrincipals* getPrincipals();
    bool          isSelfHosted(JSContext* cx);

    // Iterator for use with C++11 range based for loops, eg:
    //
    //     RootedSavedFrame stack(cx, getSomeSavedFrameStack());
    //     for (HandleSavedFrame frame : SavedFrame::RootedRange(cx, stack)) {
    //         ...
    //     }
    //
    // Each frame yielded by `SavedFrame::RootedRange` is only a valid handle to
    // a rooted `SavedFrame` within the loop's block for a single loop
    // iteration. When the next iteration begins, the value is invalidated.

    class RootedRange;

    class MOZ_STACK_CLASS RootedIterator {
        friend class RootedRange;
        RootedRange* range_;
        // For use by RootedRange::end() only.
        explicit RootedIterator() : range_(nullptr) { }

      public:
        explicit RootedIterator(RootedRange& range) : range_(&range) { }
        HandleSavedFrame operator*() { MOZ_ASSERT(range_); return range_->frame_; }
        bool operator!=(const RootedIterator& rhs) const {
            // We should only ever compare to the null range, aka we are just
            // testing if this range is done.
            MOZ_ASSERT(rhs.range_ == nullptr);
            return range_->frame_ != nullptr;
        }
        inline void operator++();
    };

    class MOZ_STACK_CLASS RootedRange {
        friend class RootedIterator;
        RootedSavedFrame frame_;

      public:
        RootedRange(JSContext* cx, HandleSavedFrame frame) : frame_(cx, frame) { }
        RootedIterator begin() { return RootedIterator(*this); }
        RootedIterator end() { return RootedIterator(); }
    };

    static bool isSavedFrameAndNotProto(JSObject& obj) {
        return obj.is<SavedFrame>() &&
               !obj.as<SavedFrame>().getReservedSlot(JSSLOT_SOURCE).isNull();
    }

    static bool isSavedFrameOrWrapperAndNotProto(JSObject& obj) {
        auto unwrapped = CheckedUnwrap(&obj);
        if (!unwrapped)
            return false;
        return isSavedFrameAndNotProto(*unwrapped);
    }

    struct Lookup;
    struct HashPolicy;

    typedef JS::GCHashSet<ReadBarriered<SavedFrame*>,
                          HashPolicy,
                          SystemAllocPolicy> Set;

    class AutoLookupVector;

    class MOZ_STACK_CLASS HandleLookup {
        friend class AutoLookupVector;

        Lookup& lookup;

        explicit HandleLookup(Lookup& lookup) : lookup(lookup) { }

      public:
        inline Lookup& get() { return lookup; }
        inline Lookup* operator->() { return &lookup; }
    };

  private:
    static SavedFrame* create(JSContext* cx);
    static MOZ_MUST_USE bool finishSavedFrameInit(JSContext* cx, HandleObject ctor, HandleObject proto);
    void initFromLookup(JSContext* cx, HandleLookup lookup);
    void initSource(JSAtom* source);
    void initLine(uint32_t line);
    void initColumn(uint32_t column);
    void initFunctionDisplayName(JSAtom* maybeName);
    void initAsyncCause(JSAtom* maybeCause);
    void initParent(SavedFrame* maybeParent);
    void initPrincipalsAlreadyHeld(JSPrincipals* principals);
    void initPrincipals(JSPrincipals* principals);

    enum {
        // The reserved slots in the SavedFrame class.
        JSSLOT_SOURCE,
        JSSLOT_LINE,
        JSSLOT_COLUMN,
        JSSLOT_FUNCTIONDISPLAYNAME,
        JSSLOT_ASYNCCAUSE,
        JSSLOT_PARENT,
        JSSLOT_PRINCIPALS,

        // The total number of reserved slots in the SavedFrame class.
        JSSLOT_COUNT
    };
};

struct SavedFrame::HashPolicy
{
    typedef SavedFrame::Lookup              Lookup;
    typedef MovableCellHasher<SavedFrame*>  SavedFramePtrHasher;
    typedef PointerHasher<JSPrincipals*> JSPrincipalsPtrHasher;

    static bool       hasHash(const Lookup& l);
    static bool       ensureHash(const Lookup& l);
    static HashNumber hash(const Lookup& lookup);
    static bool       match(SavedFrame* existing, const Lookup& lookup);

    typedef ReadBarriered<SavedFrame*> Key;
    static void rekey(Key& key, const Key& newKey);
};

template <>
struct FallibleHashMethods<SavedFrame::HashPolicy>
{
    template <typename Lookup> static bool hasHash(Lookup&& l) {
        return SavedFrame::HashPolicy::hasHash(mozilla::Forward<Lookup>(l));
    }
    template <typename Lookup> static bool ensureHash(Lookup&& l) {
        return SavedFrame::HashPolicy::ensureHash(mozilla::Forward<Lookup>(l));
    }
};

// Assert that if the given object is not null, that it must be either a
// SavedFrame object or wrapper (Xray or CCW) around a SavedFrame object.
inline void AssertObjectIsSavedFrameOrWrapper(JSContext* cx, HandleObject stack);

// When we reconstruct a SavedFrame stack from a JS::ubi::StackFrame, we may not
// have access to the principals that the original stack was captured
// with. Instead, we use these two singleton principals based on whether
// JS::ubi::StackFrame::isSystem or not. These singletons should never be passed
// to the subsumes callback, and should be special cased with a shortcut before
// that.
struct ReconstructedSavedFramePrincipals : public JSPrincipals
{
    explicit ReconstructedSavedFramePrincipals()
        : JSPrincipals()
    {
        MOZ_ASSERT(is(this));
        this->refcount = 1;
    }

    MOZ_MUST_USE bool write(JSContext* cx, JSStructuredCloneWriter* writer) override {
        MOZ_ASSERT(false, "ReconstructedSavedFramePrincipals should never be exposed to embedders");
        return false;
    }

    static ReconstructedSavedFramePrincipals IsSystem;
    static ReconstructedSavedFramePrincipals IsNotSystem;

    // Return true if the given JSPrincipals* points to one of the
    // ReconstructedSavedFramePrincipals singletons, false otherwise.
    static bool is(JSPrincipals* p) { return p == &IsSystem || p == &IsNotSystem; }

    // Get the appropriate ReconstructedSavedFramePrincipals singleton for the
    // given JS::ubi::StackFrame that is being reconstructed as a SavedFrame
    // stack.
    static JSPrincipals* getSingleton(JS::ubi::StackFrame& f) {
        return f.isSystem() ? &IsSystem : &IsNotSystem;
    }
};

inline void
SavedFrame::RootedIterator::operator++()
{
    MOZ_ASSERT(range_);
    range_->frame_ = range_->frame_->getParent();
}

} // namespace js

namespace JS {
namespace ubi {

using js::SavedFrame;

// A concrete JS::ubi::StackFrame that is backed by a live SavedFrame object.
template<>
class ConcreteStackFrame<SavedFrame> : public BaseStackFrame {
    explicit ConcreteStackFrame(SavedFrame* ptr) : BaseStackFrame(ptr) { }
    SavedFrame& get() const { return *static_cast<SavedFrame*>(ptr); }

  public:
    static void construct(void* storage, SavedFrame* ptr) { new (storage) ConcreteStackFrame(ptr); }

    StackFrame parent() const override { return get().getParent(); }
    uint32_t line() const override { return get().getLine(); }
    uint32_t column() const override { return get().getColumn(); }

    AtomOrTwoByteChars source() const override {
        auto source = get().getSource();
        return AtomOrTwoByteChars(source);
    }

    AtomOrTwoByteChars functionDisplayName() const override {
        auto name = get().getFunctionDisplayName();
        return AtomOrTwoByteChars(name);
    }

    void trace(JSTracer* trc) override {
        JSObject* prev = &get();
        JSObject* next = prev;
        js::TraceRoot(trc, &next, "ConcreteStackFrame<SavedFrame>::ptr");
        if (next != prev)
            ptr = next;
    }

    bool isSelfHosted(JSContext* cx) const override {
        return get().isSelfHosted(cx);
    }

    bool isSystem() const override;

    MOZ_MUST_USE bool constructSavedFrameStack(JSContext* cx,
                                               MutableHandleObject outSavedFrameStack)
        const override;
};

} // namespace ubi
} // namespace JS

#endif // vm_SavedFrame_h
