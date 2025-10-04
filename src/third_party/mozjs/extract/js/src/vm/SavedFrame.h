/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SavedFrame_h
#define vm_SavedFrame_h

#include "mozilla/Attributes.h"

#include "gc/Policy.h"
#include "js/ColumnNumber.h"  // JS::TaggedColumnNumberOneOrigin
#include "js/GCHashTable.h"
#include "js/Principals.h"
#include "js/UbiNode.h"
#include "vm/NativeObject.h"

namespace js {

class SavedFrame : public NativeObject {
  friend class SavedStacks;
  friend struct ::JSStructuredCloneReader;

  static const ClassSpec classSpec_;

 public:
  static const JSClass class_;
  static const JSClass protoClass_;
  static const JSPropertySpec protoAccessors[];
  static const JSFunctionSpec protoFunctions[];
  static const JSFunctionSpec staticFunctions[];

  // Prototype methods and properties to be exposed to JS.
  static bool construct(JSContext* cx, unsigned argc, Value* vp);
  static bool sourceProperty(JSContext* cx, unsigned argc, Value* vp);
  static bool sourceIdProperty(JSContext* cx, unsigned argc, Value* vp);
  static bool lineProperty(JSContext* cx, unsigned argc, Value* vp);
  static bool columnProperty(JSContext* cx, unsigned argc, Value* vp);
  static bool functionDisplayNameProperty(JSContext* cx, unsigned argc,
                                          Value* vp);
  static bool asyncCauseProperty(JSContext* cx, unsigned argc, Value* vp);
  static bool asyncParentProperty(JSContext* cx, unsigned argc, Value* vp);
  static bool parentProperty(JSContext* cx, unsigned argc, Value* vp);
  static bool toStringMethod(JSContext* cx, unsigned argc, Value* vp);

  static void finalize(JS::GCContext* gcx, JSObject* obj);

  // Convenient getters for SavedFrame's reserved slots for use from C++.
  JSAtom* getSource();
  uint32_t getSourceId();
  // Line number (1-origin).
  uint32_t getLine();
  // Column number in UTF-16 code units.
  JS::TaggedColumnNumberOneOrigin getColumn();
  JSAtom* getFunctionDisplayName();
  JSAtom* getAsyncCause();
  SavedFrame* getParent() const;
  JSPrincipals* getPrincipals();
  bool getMutedErrors();
  bool isSelfHosted(JSContext* cx);
  bool isWasm();

  // When isWasm():
  uint32_t wasmFuncIndex();
  uint32_t wasmBytecodeOffset();

  // Iterator for use with C++11 range based for loops, eg:
  //
  //     Rooted<SavedFrame*> stack(cx, getSomeSavedFrameStack());
  //     for (Handle<SavedFrame*> frame : SavedFrame::RootedRange(cx, stack)) {
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
    explicit RootedIterator() : range_(nullptr) {}

   public:
    explicit RootedIterator(RootedRange& range) : range_(&range) {}
    Handle<SavedFrame*> operator*() {
      MOZ_ASSERT(range_);
      return range_->frame_;
    }
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
    Rooted<SavedFrame*> frame_;

   public:
    RootedRange(JSContext* cx, Handle<SavedFrame*> frame) : frame_(cx, frame) {}
    RootedIterator begin() { return RootedIterator(*this); }
    RootedIterator end() { return RootedIterator(); }
  };

  struct Lookup;
  struct HashPolicy;

  typedef JS::GCHashSet<WeakHeapPtr<SavedFrame*>, HashPolicy, SystemAllocPolicy>
      Set;

 private:
  static SavedFrame* create(JSContext* cx);
  [[nodiscard]] static bool finishSavedFrameInit(JSContext* cx,
                                                 HandleObject ctor,
                                                 HandleObject proto);
  void initFromLookup(JSContext* cx, Handle<Lookup> lookup);
  void initSource(JSAtom* source);
  void initSourceId(uint32_t id);
  void initLine(uint32_t line);
  void initColumn(JS::TaggedColumnNumberOneOrigin column);
  void initFunctionDisplayName(JSAtom* maybeName);
  void initAsyncCause(JSAtom* maybeCause);
  void initParent(SavedFrame* maybeParent);
  void initPrincipalsAlreadyHeldAndMutedErrors(JSPrincipals* principals,
                                               bool mutedErrors);
  void initPrincipalsAndMutedErrors(JSPrincipals* principals, bool mutedErrors);

  enum {
    // The reserved slots in the SavedFrame class.
    JSSLOT_SOURCE,
    JSSLOT_SOURCEID,
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

struct SavedFrame::HashPolicy {
  using Lookup = SavedFrame::Lookup;
  using SavedFramePtrHasher = StableCellHasher<SavedFrame*>;
  using JSPrincipalsPtrHasher = PointerHasher<JSPrincipals*>;

  static bool maybeGetHash(const Lookup& l, HashNumber* hashOut);
  static bool ensureHash(const Lookup& l, HashNumber* hashOut);
  static HashNumber hash(const Lookup& lookup);
  static bool match(SavedFrame* existing, const Lookup& lookup);

  using Key = WeakHeapPtr<SavedFrame*>;
  static void rekey(Key& key, const Key& newKey);

 private:
  static HashNumber calculateHash(const Lookup& lookup, HashNumber parentHash);
};

}  // namespace js

namespace mozilla {

template <>
struct FallibleHashMethods<js::SavedFrame::HashPolicy> {
  template <typename Lookup>
  static bool maybeGetHash(Lookup&& l, HashNumber* hashOut) {
    return js::SavedFrame::HashPolicy::maybeGetHash(std::forward<Lookup>(l),
                                                    hashOut);
  }
  template <typename Lookup>
  static bool ensureHash(Lookup&& l, HashNumber* hashOut) {
    return js::SavedFrame::HashPolicy::ensureHash(std::forward<Lookup>(l),
                                                  hashOut);
  }
};

}  // namespace mozilla

namespace js {

// Assert that if the given object is not null, that it must be either a
// SavedFrame object or wrapper (Xray or CCW) around a SavedFrame object.
inline void AssertObjectIsSavedFrameOrWrapper(JSContext* cx,
                                              HandleObject stack);

// When we reconstruct a SavedFrame stack from a JS::ubi::StackFrame, we may not
// have access to the principals that the original stack was captured
// with. Instead, we use these two singleton principals based on whether
// JS::ubi::StackFrame::isSystem or not. These singletons should never be passed
// to the subsumes callback, and should be special cased with a shortcut before
// that.
struct ReconstructedSavedFramePrincipals : public JSPrincipals {
  explicit ReconstructedSavedFramePrincipals() {
    MOZ_ASSERT(is(this));
    this->refcount = 1;
  }

  [[nodiscard]] bool write(JSContext* cx,
                           JSStructuredCloneWriter* writer) override {
    MOZ_ASSERT(false,
               "ReconstructedSavedFramePrincipals should never be exposed to "
               "embedders");
    return false;
  }

  bool isSystemOrAddonPrincipal() override {
    MOZ_ASSERT(false,
               "ReconstructedSavedFramePrincipals should never be exposed to "
               "embedders");
    return false;
  }

  static ReconstructedSavedFramePrincipals IsSystem;
  static ReconstructedSavedFramePrincipals IsNotSystem;

  // Return true if the given JSPrincipals* points to one of the
  // ReconstructedSavedFramePrincipals singletons, false otherwise.
  static bool is(JSPrincipals* p) {
    return p == &IsSystem || p == &IsNotSystem;
  }

  // Get the appropriate ReconstructedSavedFramePrincipals singleton for the
  // given JS::ubi::StackFrame that is being reconstructed as a SavedFrame
  // stack.
  static JSPrincipals* getSingleton(JS::ubi::StackFrame& f) {
    return f.isSystem() ? &IsSystem : &IsNotSystem;
  }
};

inline void SavedFrame::RootedIterator::operator++() {
  MOZ_ASSERT(range_);
  range_->frame_ = range_->frame_->getParent();
}

}  // namespace js

namespace JS {
namespace ubi {

using js::SavedFrame;

// A concrete JS::ubi::StackFrame that is backed by a live SavedFrame object.
template <>
class ConcreteStackFrame<SavedFrame> : public BaseStackFrame {
  explicit ConcreteStackFrame(SavedFrame* ptr) : BaseStackFrame(ptr) {}
  SavedFrame& get() const { return *static_cast<SavedFrame*>(ptr); }

 public:
  static void construct(void* storage, SavedFrame* ptr) {
    new (storage) ConcreteStackFrame(ptr);
  }

  StackFrame parent() const override { return get().getParent(); }
  uint32_t line() const override { return get().getLine(); }
  JS::TaggedColumnNumberOneOrigin column() const override {
    return get().getColumn();
  }

  AtomOrTwoByteChars source() const override {
    auto source = get().getSource();
    return AtomOrTwoByteChars(source);
  }

  uint32_t sourceId() const override { return get().getSourceId(); }

  AtomOrTwoByteChars functionDisplayName() const override {
    auto name = get().getFunctionDisplayName();
    return AtomOrTwoByteChars(name);
  }

  void trace(JSTracer* trc) override {
    JSObject* prev = &get();
    JSObject* next = prev;
    js::TraceRoot(trc, &next, "ConcreteStackFrame<SavedFrame>::ptr");
    if (next != prev) {
      ptr = next;
    }
  }

  bool isSelfHosted(JSContext* cx) const override {
    return get().isSelfHosted(cx);
  }

  bool isSystem() const override;

  [[nodiscard]] bool constructSavedFrameStack(
      JSContext* cx, MutableHandleObject outSavedFrameStack) const override;
};

}  // namespace ubi
}  // namespace JS

#endif  // vm_SavedFrame_h
