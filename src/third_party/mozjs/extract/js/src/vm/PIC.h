/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PIC_h
#define vm_PIC_h

#include "vm/GlobalObject.h"
#include "vm/NativeObject.h"

namespace js {

class Shape;

template <typename Category>
class PICChain;

/*
 * The basic PICStub just has a pointer to the next stub.
 */
template <typename Category>
class PICStub {
  friend class PICChain<Category>;

 private:
  using CatStub = typename Category::Stub;
  using CatChain = typename Category::Chain;

 protected:
  CatStub* next_;

  PICStub() : next_(nullptr) {}
  explicit PICStub(const CatStub* next) : next_(next) { MOZ_ASSERT(next_); }
  explicit PICStub(const CatStub& other) : next_(other.next_) {}

 public:
  CatStub* next() const { return next_; }

 protected:
  void append(CatStub* stub) {
    MOZ_ASSERT(!next_);
    MOZ_ASSERT(!stub->next_);
    next_ = stub;
  }
};

/*
 * The basic PIC just has a pointer to the list of stubs.
 */
template <typename Category>
class PICChain {
 private:
  using CatStub = typename Category::Stub;
  using CatChain = typename Category::Chain;

 protected:
  CatStub* stubs_;

  PICChain() : stubs_(nullptr) {}
  // PICs should never be copy constructed.
  PICChain(const PICChain<Category>& other) = delete;

 public:
  CatStub* stubs() const { return stubs_; }

  void addStub(JSObject* obj, CatStub* stub);

  unsigned numStubs() const {
    unsigned count = 0;
    for (CatStub* stub = stubs_; stub; stub = stub->next()) {
      count++;
    }
    return count;
  }
};

// Class for object that holds ForOfPIC chain.
class ForOfPICObject : public NativeObject {
 public:
  enum { ChainSlot, SlotCount };

  static const JSClass class_;
};

/*
 *  ForOfPIC defines a PIC category for optimizing for-of operations.
 */
struct ForOfPIC {
  /* Forward declarations so template-substitution works. */
  class Stub;
  class Chain;

  ForOfPIC() = delete;
  ForOfPIC(const ForOfPIC& other) = delete;

  using BaseStub = PICStub<ForOfPIC>;
  using BaseChain = PICChain<ForOfPIC>;

  /*
   * A ForOfPIC has only one kind of stub for now: one that holds the shape
   * of an array object that does not override its @@iterator property.
   */
  class Stub : public BaseStub {
   private:
    // Shape of matching array object.
    const HeapPtr<Shape*> shape_;

   public:
    explicit Stub(Shape* shape) : shape_(shape) { MOZ_ASSERT(shape_); }

    Shape* shape() { return shape_; }

    void trace(JSTracer* trc);
  };

  /*
   * A ForOfPIC chain holds the following:
   *
   *  Array.prototype (arrayProto_)
   *      To ensure that the incoming array has the standard proto.
   *
   *  Array.prototype's shape (arrayProtoShape_)
   *      To ensure that Array.prototype has not been modified.
   *
   *  ArrayIterator.prototype
   *  ArrayIterator.prototype's shape
   *      (arrayIteratorProto_, arrayIteratorProtoShape_)
   *      To ensure that an ArrayIterator.prototype has not been modified.
   *
   *  Array.prototype's slot number for @@iterator
   *  Array.prototype's canonical value for @@iterator
   *      (arrayProtoIteratorSlot_, canonicalIteratorFunc_)
   *      To quickly retrieve and ensure that the iterator constructor
   *      stored in the slot has not changed.
   *
   *  ArrayIterator.prototype's slot number for 'next'
   *  ArrayIterator.prototype's canonical value for 'next'
   *      (arrayIteratorProtoNextSlot_, canonicalNextFunc_)
   *      To quickly retrieve and ensure that the 'next' method for
   *      ArrayIterator objects has not changed.
   */
  class Chain : public BaseChain {
   private:
    // Pointer to owning JSObject for memory accounting purposes.
    const GCPtr<JSObject*> picObject_;

    // Pointer to canonical Array.prototype, ArrayIterator.prototype,
    // Iterator.prototype, and Object.prototype
    GCPtr<NativeObject*> arrayProto_;
    GCPtr<NativeObject*> arrayIteratorProto_;
    GCPtr<NativeObject*> iteratorProto_;
    GCPtr<NativeObject*> objectProto_;

    // Shape of matching Array.prototype object, and slot containing
    // the @@iterator for it, and the canonical value.
    GCPtr<Shape*> arrayProtoShape_;
    uint32_t arrayProtoIteratorSlot_;
    GCPtr<Value> canonicalIteratorFunc_;

    // Shape of matching ArrayIteratorProto, and slot containing
    // the 'next' property, and the canonical value.
    GCPtr<Shape*> arrayIteratorProtoShape_;
    uint32_t arrayIteratorProtoNextSlot_;
    GCPtr<Value> canonicalNextFunc_;

    // Shape of matching Iterator.prototype object.
    GCPtr<Shape*> iteratorProtoShape_;
    // Shape of matching Object.prototype object.
    GCPtr<Shape*> objectProtoShape_;

    // Initialization flag marking lazy initialization of above fields.
    bool initialized_;

    // Disabled flag is set when we don't want to try optimizing anymore
    // because core objects were changed.
    bool disabled_;

    static const unsigned MAX_STUBS = 10;

   public:
    explicit Chain(JSObject* picObject)
        : picObject_(picObject),
          arrayProto_(nullptr),
          arrayIteratorProto_(nullptr),
          arrayProtoShape_(nullptr),
          arrayProtoIteratorSlot_(-1),
          canonicalIteratorFunc_(UndefinedValue()),
          arrayIteratorProtoShape_(nullptr),
          arrayIteratorProtoNextSlot_(-1),
          initialized_(false),
          disabled_(false) {}

    // Initialize the canonical iterator function.
    bool initialize(JSContext* cx);

    // Try to optimize this chain for a newly allocated array.
    bool tryOptimizeArray(JSContext* cx, bool* optimized);

    // Try to optimize this chain for an object.
    bool tryOptimizeArray(JSContext* cx, Handle<ArrayObject*> array,
                          bool* optimized);

    // Check if %ArrayIteratorPrototype% still uses the default "next" method.
    bool tryOptimizeArrayIteratorNext(JSContext* cx, bool* optimized);

    void trace(JSTracer* trc);
    void finalize(JS::GCContext* gcx, JSObject* obj);

    void freeAllStubs(JS::GCContext* gcx);

   private:
    // Check if the global array-related objects have not been messed with
    // in a way that would disable this PIC.
    bool isArrayStateStillSane();

    // Check if ArrayIterator.next and ArrayIterator.return are still
    // optimizable.
    inline bool isArrayIteratorStateStillSane() {
      // Ensure the prototype chain is intact, which will ensure that "return"
      // has not been defined.
      if (arrayIteratorProto_->shape() != arrayIteratorProtoShape_) {
        return false;
      }

      if (iteratorProto_->shape() != iteratorProtoShape_) {
        return false;
      }

      if (objectProto_->shape() != objectProtoShape_) {
        return false;
      }

      return arrayIteratorProto_->getSlot(arrayIteratorProtoNextSlot_) ==
             canonicalNextFunc_;
    }

    // Check if a matching optimized stub for the given object exists.
    bool hasMatchingStub(ArrayObject* obj);

    // Reset the PIC and all info associated with it.
    void reset(JSContext* cx);

    // Erase the stub chain.
    void eraseChain(JSContext* cx);
  };

  static NativeObject* createForOfPICObject(JSContext* cx,
                                            Handle<GlobalObject*> global);

  static inline Chain* fromJSObject(NativeObject* obj) {
    MOZ_ASSERT(obj->is<ForOfPICObject>());
    return obj->maybePtrFromReservedSlot<Chain>(ForOfPICObject::ChainSlot);
  }
  static inline Chain* getOrCreate(JSContext* cx) {
    NativeObject* obj = cx->global()->getForOfPICObject();
    if (obj) {
      return fromJSObject(obj);
    }
    return create(cx);
  }
  static Chain* create(JSContext* cx);
};

}  // namespace js

#endif /* vm_PIC_h */
