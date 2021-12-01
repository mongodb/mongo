/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PIC_h
#define vm_PIC_h

#include "vm/GlobalObject.h"

namespace js {

class Shape;

template <typename Category> class PICChain;

/*
 * The basic PICStub just has a pointer to the next stub.
 */
template <typename Category>
class PICStub
{
  friend class PICChain<Category>;
  private:
    typedef typename Category::Stub CatStub;
    typedef typename Category::Chain CatChain;

  protected:
    CatStub* next_;

    PICStub() : next_(nullptr) {}
    explicit PICStub(const CatStub* next) : next_(next) {
        MOZ_ASSERT(next_);
    }
    explicit PICStub(const CatStub& other) : next_(other.next_) {}

  public:
    CatStub* next() const {
        return next_;
    }

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
class PICChain
{
  private:
    typedef typename Category::Stub CatStub;
    typedef typename Category::Chain CatChain;

  protected:
    CatStub* stubs_;

    PICChain() : stubs_(nullptr) {}
    // PICs should never be copy constructed.
    PICChain(const PICChain<Category>& other) = delete;

  public:
    CatStub* stubs() const {
        return stubs_;
    }

    void addStub(CatStub* stub) {
        MOZ_ASSERT(stub);
        MOZ_ASSERT(!stub->next());
        if (!stubs_) {
            stubs_ = stub;
            return;
        }

        CatStub* cur = stubs_;
        while (cur->next())
            cur = cur->next();
        cur->append(stub);
    }

    unsigned numStubs() const {
        unsigned count = 0;
        for (CatStub* stub = stubs_; stub; stub = stub->next())
            count++;
        return count;
    }

    void removeStub(CatStub* stub, CatStub* previous) {
        if (previous) {
            MOZ_ASSERT(previous->next() == stub);
            previous->next_ = stub->next();
        } else {
            MOZ_ASSERT(stub == stubs_);
            stubs_ = stub->next();
        }
        js_delete(stub);
    }
};

/*
 *  ForOfPIC defines a PIC category for optimizing for-of operations.
 */
struct ForOfPIC
{
    /* Forward declarations so template-substitution works. */
    class Stub;
    class Chain;

    ForOfPIC() = delete;
    ForOfPIC(const ForOfPIC& other) = delete;

    typedef PICStub<ForOfPIC> BaseStub;
    typedef PICChain<ForOfPIC> BaseChain;

    /*
     * A ForOfPIC has only one kind of stub for now: one that holds the shape
     * of an array object that does not override its @@iterator property.
     */
    class Stub : public BaseStub
    {
      private:
        // Shape of matching array object.
        Shape* shape_;

      public:
        explicit Stub(Shape* shape)
          : BaseStub(),
            shape_(shape)
        {
            MOZ_ASSERT(shape_);
        }

        Shape* shape() {
            return shape_;
        }
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
     *  ArrayIterator.prototype (arrayIteratorProto_)
     *  ArrayIterator.prototype's shape (arrayIteratorProtoShape_)
     *      To ensure that an ArrayIterator.prototype has not been modified.
     *
     *  Array.prototype's slot number for @@iterator (arrayProtoIteratorSlot_)
     *  Array.prototype's canonical value for @@iterator (canonicalIteratorFunc_)
     *      To quickly retrieve and ensure that the iterator constructor
     *      stored in the slot has not changed.
     *
     *  ArrayIterator.prototype's slot number for 'next' (arrayIteratorProtoNextSlot_)
     *  ArrayIterator.prototype's canonical value for 'next' (canonicalNextFunc_)
     *      To quickly retrieve and ensure that the 'next' method for ArrayIterator
     *      objects has not changed.
     */
    class Chain : public BaseChain
    {
      private:
        // Pointer to canonical Array.prototype and ArrayIterator.prototype
        GCPtrNativeObject arrayProto_;
        GCPtrNativeObject arrayIteratorProto_;

        // Shape of matching Array.prototype object, and slot containing
        // the @@iterator for it, and the canonical value.
        GCPtrShape arrayProtoShape_;
        uint32_t arrayProtoIteratorSlot_;
        GCPtrValue canonicalIteratorFunc_;

        // Shape of matching ArrayIteratorProto, and slot containing
        // the 'next' property, and the canonical value.
        GCPtrShape arrayIteratorProtoShape_;
        uint32_t arrayIteratorProtoNextSlot_;
        GCPtrValue canonicalNextFunc_;

        // Initialization flag marking lazy initialization of above fields.
        bool initialized_;

        // Disabled flag is set when we don't want to try optimizing anymore
        // because core objects were changed.
        bool disabled_;

        static const unsigned MAX_STUBS = 10;

      public:
        Chain()
          : BaseChain(),
            arrayProto_(nullptr),
            arrayIteratorProto_(nullptr),
            arrayProtoShape_(nullptr),
            arrayProtoIteratorSlot_(-1),
            canonicalIteratorFunc_(UndefinedValue()),
            arrayIteratorProtoShape_(nullptr),
            arrayIteratorProtoNextSlot_(-1),
            initialized_(false),
            disabled_(false)
        {}

        // Initialize the canonical iterator function.
        bool initialize(JSContext* cx);

        // Try to optimize this chain for an object.
        bool tryOptimizeArray(JSContext* cx, HandleArrayObject array, bool* optimized);

        // Check if the global array-related objects have not been messed with
        // in a way that would disable this PIC.
        bool isArrayStateStillSane();

        // Check if ArrayIterator.next is still optimizable.
        inline bool isArrayNextStillSane() {
            return (arrayIteratorProto_->lastProperty() == arrayIteratorProtoShape_) &&
                (arrayIteratorProto_->getSlot(arrayIteratorProtoNextSlot_) == canonicalNextFunc_);
        }

        void trace(JSTracer* trc);
        void sweep(FreeOp* fop);

      private:
        // Check if a matching optimized stub for the given object exists.
        bool hasMatchingStub(ArrayObject* obj);

        // Reset the PIC and all info associated with it.
        void reset();

        // Erase the stub chain.
        void eraseChain();
    };

    // Class for object that holds ForOfPIC chain.
    static const Class class_;

    static NativeObject* createForOfPICObject(JSContext* cx, Handle<GlobalObject*> global);

    static inline Chain* fromJSObject(NativeObject* obj) {
        MOZ_ASSERT(js::GetObjectClass(obj) == &ForOfPIC::class_);
        return (ForOfPIC::Chain*) obj->getPrivate();
    }
    static inline Chain* getOrCreate(JSContext* cx) {
        NativeObject* obj = cx->global()->getForOfPICObject();
        if (obj)
            return fromJSObject(obj);
        return create(cx);
    }
    static Chain* create(JSContext* cx);
};


} // namespace js

#endif /* vm_PIC_h */
