/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_TaggedProto_h
#define vm_TaggedProto_h

#include "gc/Tracer.h"

namespace js {

// Information about an object prototype, which can be either a particular
// object, null, or a lazily generated object. The latter is only used by
// certain kinds of proxies.
class TaggedProto : public JS::Traceable
{
  public:
    static JSObject * const LazyProto;

    TaggedProto() : proto(nullptr) {}
    explicit TaggedProto(JSObject* proto) : proto(proto) {}

    uintptr_t toWord() const { return uintptr_t(proto); }

    bool isLazy() const {
        return proto == LazyProto;
    }
    bool isObject() const {
        /* Skip nullptr and LazyProto. */
        return uintptr_t(proto) > uintptr_t(TaggedProto::LazyProto);
    }
    JSObject* toObject() const {
        MOZ_ASSERT(isObject());
        return proto;
    }
    JSObject* toObjectOrNull() const {
        MOZ_ASSERT(!proto || isObject());
        return proto;
    }
    JSObject* raw() const { return proto; }

    bool operator ==(const TaggedProto& other) const { return proto == other.proto; }
    bool operator !=(const TaggedProto& other) const { return proto != other.proto; }

    static void trace(TaggedProto* protop, JSTracer* trc) {
        TraceManuallyBarrieredEdge(trc, protop, "TaggedProto");
    }

  private:
    JSObject* proto;
};

template <> struct GCMethods<TaggedProto>
{
    static TaggedProto initial() { return TaggedProto(); }
};

template <> struct InternalGCMethods<TaggedProto>
{
    static void preBarrier(TaggedProto& proto);

    static void postBarrier(TaggedProto* vp, TaggedProto prev, TaggedProto next);

    static bool isMarkableTaggedPointer(TaggedProto proto) {
        return proto.isObject();
    }

    static bool isMarkable(TaggedProto proto) {
        return proto.isObject();
    }
};

template<class Outer>
class TaggedProtoOperations
{
    const TaggedProto& value() const {
        return static_cast<const Outer*>(this)->get();
    }

  public:
    uintptr_t toWord() const { return value().toWord(); }
    inline bool isLazy() const { return value().isLazy(); }
    inline bool isObject() const { return value().isObject(); }
    inline JSObject* toObject() const { return value().toObject(); }
    inline JSObject* toObjectOrNull() const { return value().toObjectOrNull(); }
    JSObject* raw() const { return value().raw(); }
};

template <>
class HandleBase<TaggedProto> : public TaggedProtoOperations<Handle<TaggedProto>>
{};

template <>
class RootedBase<TaggedProto> : public TaggedProtoOperations<Rooted<TaggedProto>>
{};

template <>
class BarrieredBaseMixins<TaggedProto> : public TaggedProtoOperations<HeapPtr<TaggedProto>>
{};

// If the TaggedProto is a JSObject pointer, convert to that type and call |f|
// with the pointer. If the TaggedProto is lazy, calls F::defaultValue.
template <typename F, typename... Args>
auto
DispatchTyped(F f, TaggedProto& proto, Args&&... args)
  -> decltype(f(static_cast<JSObject*>(nullptr), mozilla::Forward<Args>(args)...))
{
    if (proto.isObject())
        return f(proto.toObject(), mozilla::Forward<Args>(args)...);
    return F::defaultValue(proto);
}

// Since JSObject pointers are either nullptr or a valid object and since the
// object layout of TaggedProto is identical to a bare object pointer, we can
// safely treat a pointer to an already-rooted object (e.g. HandleObject) as a
// pointer to a TaggedProto.
inline Handle<TaggedProto>
AsTaggedProto(HandleObject obj)
{
    static_assert(sizeof(JSObject*) == sizeof(TaggedProto),
                  "TaggedProto must be binary compatible with JSObject");
    return Handle<TaggedProto>::fromMarkedLocation(
            reinterpret_cast<TaggedProto const*>(obj.address()));
}

} // namespace js

#endif // vm_TaggedProto_h
