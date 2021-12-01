/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ReceiverGuard_h
#define vm_ReceiverGuard_h

#include "vm/Shape.h"

namespace js {

// A ReceiverGuard encapsulates the information about an object that needs to
// be tested to determine if it has the same 'structure' as another object.
// The guard includes the shape and/or group of the object, and which of these
// is tested, as well as the meaning here of 'structure', depends on the kind
// of object being tested:
//
// NativeObject: The structure of a native object is determined by its shape.
//   Two objects with the same shape have the same class, prototype, flags,
//   and all properties except those stored in dense elements.
//
// ProxyObject: The structure of a proxy object is determined by its shape.
//   Proxies with the same shape have the same class and prototype, but no
//   other commonality is guaranteed.
//
// TypedObject: The structure of a typed object is determined by its group.
//   All typed objects with the same group have the same class, prototype, and
//   own properties.
//
// UnboxedPlainObject: The structure of an unboxed plain object is determined
//   by its group and its expando object's shape, if there is one. All unboxed
//   plain objects with the same group and expando shape have the same
//   properties except those stored in the expando's dense elements.

class HeapReceiverGuard;

class ReceiverGuard
{
  public:
    ObjectGroup* group;
    Shape* shape;

    ReceiverGuard()
      : group(nullptr), shape(nullptr)
    {}

    inline MOZ_IMPLICIT ReceiverGuard(const HeapReceiverGuard& guard);

    explicit MOZ_ALWAYS_INLINE ReceiverGuard(JSObject* obj);
    MOZ_ALWAYS_INLINE ReceiverGuard(ObjectGroup* group, Shape* shape);

    bool operator ==(const ReceiverGuard& other) const {
        return group == other.group && shape == other.shape;
    }

    bool operator !=(const ReceiverGuard& other) const {
        return !(*this == other);
    }

    uintptr_t hash() const {
        return (uintptr_t(group) >> 3) ^ (uintptr_t(shape) >> 3);
    }
};

class HeapReceiverGuard
{
    GCPtrObjectGroup group_;
    GCPtrShape shape_;

  public:
    explicit HeapReceiverGuard(const ReceiverGuard& guard)
      : group_(guard.group), shape_(guard.shape)
    {}

    void init(const ReceiverGuard& other) {
        group_.init(other.group);
        shape_.init(other.shape);
    }

    void trace(JSTracer* trc);

    Shape* shape() const {
        return shape_;
    }
    ObjectGroup* group() const {
        return group_;
    }
};

inline
ReceiverGuard::ReceiverGuard(const HeapReceiverGuard& guard)
  : group(guard.group()), shape(guard.shape())
{}

} // namespace js

#endif /* vm_ReceiverGuard_h */
