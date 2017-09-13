/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jspropertytree_h
#define jspropertytree_h

#include "jsalloc.h"
#include "jspubtd.h"

#include "js/HashTable.h"

namespace js {

class Shape;
struct StackShape;

struct ShapeHasher : public DefaultHasher<Shape*> {
    typedef Shape* Key;
    typedef StackShape Lookup;

    static inline HashNumber hash(const Lookup& l);
    static inline bool match(Key k, const Lookup& l);
};

typedef HashSet<Shape*, ShapeHasher, SystemAllocPolicy> KidsHash;

class KidsPointer {
  private:
    enum {
        SHAPE = 0,
        HASH  = 1,
        TAG   = 1
    };

    uintptr_t w;

  public:
    bool isNull() const { return !w; }
    void setNull() { w = 0; }

    bool isShape() const { return (w & TAG) == SHAPE && !isNull(); }
    Shape* toShape() const {
        MOZ_ASSERT(isShape());
        return reinterpret_cast<Shape*>(w & ~uintptr_t(TAG));
    }
    void setShape(Shape* shape) {
        MOZ_ASSERT(shape);
        MOZ_ASSERT((reinterpret_cast<uintptr_t>(static_cast<Shape*>(shape)) & TAG) == 0);
        w = reinterpret_cast<uintptr_t>(static_cast<Shape*>(shape)) | SHAPE;
    }

    bool isHash() const { return (w & TAG) == HASH; }
    KidsHash* toHash() const {
        MOZ_ASSERT(isHash());
        return reinterpret_cast<KidsHash*>(w & ~uintptr_t(TAG));
    }
    void setHash(KidsHash* hash) {
        MOZ_ASSERT(hash);
        MOZ_ASSERT((reinterpret_cast<uintptr_t>(hash) & TAG) == 0);
        w = reinterpret_cast<uintptr_t>(hash) | HASH;
    }

#ifdef DEBUG
    void checkConsistency(Shape* aKid) const;
#endif
};

class PropertyTree
{
    friend class ::JSFunction;

    JSCompartment* compartment_;

    bool insertChild(ExclusiveContext* cx, Shape* parent, Shape* child);

    PropertyTree();

  public:
    /*
     * Use a lower limit for objects that are accessed using SETELEM (o[x] = y).
     * These objects are likely used as hashmaps and dictionary mode is more
     * efficient in this case.
     */
    enum {
        MAX_HEIGHT = 512,
        MAX_HEIGHT_WITH_ELEMENTS_ACCESS = 128
    };

    explicit PropertyTree(JSCompartment* comp)
        : compartment_(comp)
    {
    }

    JSCompartment* compartment() { return compartment_; }

    Shape* getChild(ExclusiveContext* cx, Shape* parent, JS::Handle<StackShape> child);
};

} /* namespace js */

#endif /* jspropertytree_h */
