/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/UnboxedObject.h"

#include "jsobjinlines.h"

#include "vm/Shape-inl.h"

using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::PodCopy;
using mozilla::UniquePtr;

using namespace js;

/////////////////////////////////////////////////////////////////////
// UnboxedLayout
/////////////////////////////////////////////////////////////////////

void
UnboxedLayout::trace(JSTracer* trc)
{
    for (size_t i = 0; i < properties_.length(); i++)
        MarkStringUnbarriered(trc, &properties_[i].name, "unboxed_layout_name");

    if (newScript())
        newScript()->trace(trc);

    if (nativeGroup_)
        MarkObjectGroup(trc, &nativeGroup_, "unboxed_layout_nativeGroup");

    if (nativeShape_)
        MarkShape(trc, &nativeShape_, "unboxed_layout_nativeShape");
}

size_t
UnboxedLayout::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    return mallocSizeOf(this)
         + properties_.sizeOfExcludingThis(mallocSizeOf)
         + (newScript() ? newScript()->sizeOfIncludingThis(mallocSizeOf) : 0)
         + mallocSizeOf(traceList());
}

void
UnboxedLayout::setNewScript(TypeNewScript* newScript, bool writeBarrier /* = true */)
{
    if (newScript_ && writeBarrier)
        TypeNewScript::writeBarrierPre(newScript_);
    newScript_ = newScript;
}

/////////////////////////////////////////////////////////////////////
// UnboxedPlainObject
/////////////////////////////////////////////////////////////////////

bool
UnboxedPlainObject::setValue(JSContext* cx, const UnboxedLayout::Property& property, const Value& v)
{
    uint8_t* p = &data_[property.offset];

    switch (property.type) {
      case JSVAL_TYPE_BOOLEAN:
        if (v.isBoolean()) {
            *p = v.toBoolean();
            return true;
        }
        return false;

      case JSVAL_TYPE_INT32:
        if (v.isInt32()) {
            *reinterpret_cast<int32_t*>(p) = v.toInt32();
            return true;
        }
        return false;

      case JSVAL_TYPE_DOUBLE:
        if (v.isNumber()) {
            *reinterpret_cast<double*>(p) = v.toNumber();
            return true;
        }
        return false;

      case JSVAL_TYPE_STRING:
        if (v.isString()) {
            *reinterpret_cast<HeapPtrString*>(p) = v.toString();
            return true;
        }
        return false;

      case JSVAL_TYPE_OBJECT:
        if (v.isObjectOrNull()) {
            // Update property types when writing object properties. Types for
            // other properties were captured when the unboxed layout was
            // created.
            AddTypePropertyId(cx, this, NameToId(property.name), v);

            *reinterpret_cast<HeapPtrObject*>(p) = v.toObjectOrNull();
            return true;
        }
        return false;

      default:
        MOZ_CRASH("Invalid type for unboxed value");
    }
}

Value
UnboxedPlainObject::getValue(const UnboxedLayout::Property& property)
{
    uint8_t* p = &data_[property.offset];

    switch (property.type) {
      case JSVAL_TYPE_BOOLEAN:
        return BooleanValue(*p != 0);

      case JSVAL_TYPE_INT32:
        return Int32Value(*reinterpret_cast<int32_t*>(p));

      case JSVAL_TYPE_DOUBLE:
        return DoubleValue(*reinterpret_cast<double*>(p));

      case JSVAL_TYPE_STRING:
        return StringValue(*reinterpret_cast<JSString**>(p));

      case JSVAL_TYPE_OBJECT:
        return ObjectOrNullValue(*reinterpret_cast<JSObject**>(p));

      default:
        MOZ_CRASH("Invalid type for unboxed value");
    }
}

void
UnboxedPlainObject::trace(JSTracer* trc, JSObject* obj)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();
    const int32_t* list = layout.traceList();
    if (!list)
        return;

    uint8_t* data = obj->as<UnboxedPlainObject>().data();
    while (*list != -1) {
        HeapPtrString* heap = reinterpret_cast<HeapPtrString*>(data + *list);
        MarkString(trc, heap, "unboxed_string");
        list++;
    }
    list++;
    while (*list != -1) {
        HeapPtrObject* heap = reinterpret_cast<HeapPtrObject*>(data + *list);
        if (*heap)
            MarkObject(trc, heap, "unboxed_object");
        list++;
    }

    // Unboxed objects don't have Values to trace.
    MOZ_ASSERT(*(list + 1) == -1);
}

/* static */ bool
UnboxedLayout::makeNativeGroup(JSContext* cx, ObjectGroup* group)
{
    UnboxedLayout& layout = group->unboxedLayout();

    MOZ_ASSERT(!layout.nativeGroup());

    // Immediately clear any new script on the group, as
    // rollbackPartiallyInitializedObjects() will be confused by the type
    // changes we make later on.
    group->clearNewScript(cx);

    AutoEnterAnalysis enter(cx);

    Rooted<TaggedProto> proto(cx, group->proto());

    size_t nfixed = gc::GetGCKindSlots(layout.getAllocKind());
    RootedShape shape(cx, EmptyShape::getInitialShape(cx, &PlainObject::class_, proto,
                                                      cx->global(), nullptr, nfixed, 0));
    if (!shape)
        return false;

    for (size_t i = 0; i < layout.properties().length(); i++) {
        const UnboxedLayout::Property& property = layout.properties()[i];

        StackShape unrootedChild(shape->base()->unowned(), NameToId(property.name), i,
                                 JSPROP_ENUMERATE, 0);
        RootedGeneric<StackShape*> child(cx, &unrootedChild);
        shape = cx->compartment()->propertyTree.getChild(cx, shape, *child);
        if (!shape)
            return false;
    }

    ObjectGroup* nativeGroup =
        ObjectGroupCompartment::makeGroup(cx, &PlainObject::class_, proto,
                                          group->flags() & OBJECT_FLAG_DYNAMIC_MASK);
    if (!nativeGroup)
        return false;

    // Propagate all property types from the old group to the new group.
    for (size_t i = 0; i < group->getPropertyCount(); i++) {
        if (ObjectGroup::Property* property = group->getProperty(i)) {
            TypeSet::TypeList types;
            if (!property->types.enumerateTypes(&types))
                return false;
            for (size_t i = 0; i < types.length(); i++)
                AddTypePropertyId(cx, nativeGroup, property->id, types[i]);
            HeapTypeSet* nativeProperty = nativeGroup->maybeGetProperty(property->id);
            if (nativeProperty->canSetDefinite(i))
                nativeProperty->setDefinite(i);
        }
    }

    layout.nativeGroup_ = nativeGroup;
    layout.nativeShape_ = shape;

    nativeGroup->setOriginalUnboxedGroup(group);

    return true;
}

/* static */ bool
UnboxedPlainObject::convertToNative(JSContext* cx, JSObject* obj)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();

    if (!layout.nativeGroup()) {
        if (!UnboxedLayout::makeNativeGroup(cx, obj->group()))
            return false;

        // makeNativeGroup can reentrantly invoke this method.
        if (obj->is<PlainObject>())
            return true;
    }

    AutoValueVector values(cx);
    for (size_t i = 0; i < layout.properties().length(); i++) {
        if (!values.append(obj->as<UnboxedPlainObject>().getValue(layout.properties()[i])))
            return false;
    }

    uint32_t objectFlags = obj->lastProperty()->getObjectFlags();
    RootedObject metadata(cx, obj->getMetadata());

    obj->setGroup(layout.nativeGroup());
    obj->as<PlainObject>().setLastPropertyMakeNative(cx, layout.nativeShape());

    for (size_t i = 0; i < values.length(); i++)
        obj->as<PlainObject>().initSlotUnchecked(i, values[i]);

    if (objectFlags) {
        RootedObject objRoot(cx, obj);
        if (!obj->setFlags(cx, objectFlags))
            return false;
        obj = objRoot;
    }

    if (metadata) {
        RootedObject objRoot(cx, obj);
        RootedObject metadataRoot(cx, metadata);
        if (!setMetadata(cx, objRoot, metadataRoot))
            return false;
    }

    return true;
}

/* static */
UnboxedPlainObject*
UnboxedPlainObject::create(JSContext* cx, HandleObjectGroup group, NewObjectKind newKind)
{
    MOZ_ASSERT(group->clasp() == &class_);
    gc::AllocKind allocKind = group->unboxedLayout().getAllocKind();

    UnboxedPlainObject* res = NewObjectWithGroup<UnboxedPlainObject>(cx, group, cx->global(),
                                                                     allocKind, newKind);
    if (!res)
        return nullptr;

    // Initialize reference fields of the object. All fields in the object will
    // be overwritten shortly, but references need to be safe for the GC.
    const int32_t* list = res->layout().traceList();
    if (list) {
        uint8_t* data = res->data();
        while (*list != -1) {
            HeapPtrString* heap = reinterpret_cast<HeapPtrString*>(data + *list);
            heap->init(cx->names().empty);
            list++;
        }
        list++;
        while (*list != -1) {
            HeapPtrObject* heap = reinterpret_cast<HeapPtrObject*>(data + *list);
            heap->init(nullptr);
            list++;
        }
        // Unboxed objects don't have Values to initialize.
        MOZ_ASSERT(*(list + 1) == -1);
    }

    return res;
}

/* static */ bool
UnboxedPlainObject::obj_lookupProperty(JSContext* cx, HandleObject obj,
                                       HandleId id, MutableHandleObject objp,
                                       MutableHandleShape propp)
{
    if (obj->as<UnboxedPlainObject>().layout().lookup(id)) {
        MarkNonNativePropertyFound<CanGC>(propp);
        objp.set(obj);
        return true;
    }

    RootedObject proto(cx, obj->getProto());
    if (!proto) {
        objp.set(nullptr);
        propp.set(nullptr);
        return true;
    }

    return LookupProperty(cx, proto, id, objp, propp);
}

/* static */ bool
UnboxedPlainObject::obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                                       PropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    if (!convertToNative(cx, obj))
        return false;

    return DefineProperty(cx, obj, id, v, getter, setter, attrs);
}

/* static */ bool
UnboxedPlainObject::obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp)
{
    if (obj->as<UnboxedPlainObject>().layout().lookup(id)) {
        *foundp = true;
        return true;
    }

    RootedObject proto(cx, obj->getProto());
    if (!proto) {
        *foundp = false;
        return true;
    }

    return HasProperty(cx, proto, id, foundp);
}

/* static */ bool
UnboxedPlainObject::obj_getProperty(JSContext* cx, HandleObject obj, HandleObject receiver,
                                    HandleId id, MutableHandleValue vp)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();

    if (const UnboxedLayout::Property* property = layout.lookup(id)) {
        vp.set(obj->as<UnboxedPlainObject>().getValue(*property));
        return true;
    }

    RootedObject proto(cx, obj->getProto());
    if (!proto) {
        vp.setUndefined();
        return true;
    }

    return GetProperty(cx, proto, receiver, id, vp);
}

/* static */ bool
UnboxedPlainObject::obj_setProperty(JSContext* cx, HandleObject obj, HandleObject receiver,
                                    HandleId id, MutableHandleValue vp, bool strict)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();

    if (const UnboxedLayout::Property* property = layout.lookup(id)) {
        if (obj == receiver) {
            if (obj->as<UnboxedPlainObject>().setValue(cx, *property, vp))
                return true;

            if (!convertToNative(cx, obj))
                return false;
            return SetProperty(cx, obj, receiver, id, vp, strict);
        }

        return SetPropertyByDefining(cx, obj, receiver, id, vp, strict, false);
    }

    return SetPropertyOnProto(cx, obj, receiver, id, vp, strict);
}

/* static */ bool
UnboxedPlainObject::obj_getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                                 MutableHandle<JSPropertyDescriptor> desc)
{
    const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();

    if (const UnboxedLayout::Property* property = layout.lookup(id)) {
        desc.value().set(obj->as<UnboxedPlainObject>().getValue(*property));
        desc.setAttributes(JSPROP_ENUMERATE);
        desc.object().set(obj);
        return true;
    }

    desc.object().set(nullptr);
    return true;
}

/* static */ bool
UnboxedPlainObject::obj_deleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                                       bool* succeeded)
{
    if (!convertToNative(cx, obj))
        return false;
    return DeleteProperty(cx, obj, id, succeeded);
}

/* static */ bool
UnboxedPlainObject::obj_watch(JSContext* cx, HandleObject obj, HandleId id, HandleObject callable)
{
    if (!convertToNative(cx, obj))
        return false;
    return WatchProperty(cx, obj, id, callable);
}

/* static */ bool
UnboxedPlainObject::obj_enumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties)
{
    const UnboxedLayout::PropertyVector& unboxed = obj->as<UnboxedPlainObject>().layout().properties();
    for (size_t i = 0; i < unboxed.length(); i++) {
        if (!properties.append(NameToId(unboxed[i].name)))
            return false;
    }
    return true;
}

const Class UnboxedPlainObject::class_ = {
    "Object",
    Class::NON_NATIVE | JSCLASS_IMPLEMENTS_BARRIERS,
    nullptr,        /* addProperty */
    nullptr,        /* delProperty */
    nullptr,        /* getProperty */
    nullptr,        /* setProperty */
    nullptr,        /* enumerate   */
    nullptr,        /* resolve     */
    nullptr,        /* convert     */
    nullptr,        /* finalize    */
    nullptr,        /* call        */
    nullptr,        /* hasInstance */
    nullptr,        /* construct   */
    UnboxedPlainObject::trace,
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    {
        UnboxedPlainObject::obj_lookupProperty,
        UnboxedPlainObject::obj_defineProperty,
        UnboxedPlainObject::obj_hasProperty,
        UnboxedPlainObject::obj_getProperty,
        UnboxedPlainObject::obj_setProperty,
        UnboxedPlainObject::obj_getOwnPropertyDescriptor,
        UnboxedPlainObject::obj_deleteProperty,
        UnboxedPlainObject::obj_watch,
        nullptr,   /* No unwatch needed, as watch() converts the object to native */
        nullptr,   /* getElements */
        UnboxedPlainObject::obj_enumerate,
        nullptr, /* thisObject */
    }
};

/////////////////////////////////////////////////////////////////////
// API
/////////////////////////////////////////////////////////////////////

static bool
UnboxedTypeIncludes(JSValueType supertype, JSValueType subtype)
{
    if (supertype == JSVAL_TYPE_DOUBLE && subtype == JSVAL_TYPE_INT32)
        return true;
    if (supertype == JSVAL_TYPE_OBJECT && subtype == JSVAL_TYPE_NULL)
        return true;
    return false;
}

// Return whether the property names and types in layout are a subset of the
// specified vector.
static bool
PropertiesAreSuperset(const UnboxedLayout::PropertyVector& properties, UnboxedLayout* layout)
{
    for (size_t i = 0; i < layout->properties().length(); i++) {
        const UnboxedLayout::Property& layoutProperty = layout->properties()[i];
        bool found = false;
        for (size_t j = 0; j < properties.length(); j++) {
            if (layoutProperty.name == properties[j].name) {
                found = (layoutProperty.type == properties[j].type);
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

bool
js::TryConvertToUnboxedLayout(JSContext* cx, Shape* templateShape,
                              ObjectGroup* group, PreliminaryObjectArray* objects)
{
    if (!cx->runtime()->options().unboxedObjects())
        return true;

    if (templateShape->slotSpan() == 0)
        return true;

    UnboxedLayout::PropertyVector properties;
    if (!properties.appendN(UnboxedLayout::Property(), templateShape->slotSpan()))
        return false;

    size_t objectCount = 0;
    for (size_t i = 0; i < PreliminaryObjectArray::COUNT; i++) {
        JSObject* obj = objects->get(i);
        if (!obj)
            continue;

        objectCount++;

        // All preliminary objects must have been created with the largest
        // allocation kind possible, which will allow their unboxed data to be
        // filled in inline.
        MOZ_ASSERT(gc::GetGCKindSlots(obj->asTenured().getAllocKind()) ==
                   NativeObject::MAX_FIXED_SLOTS);

        if (obj->as<PlainObject>().lastProperty() != templateShape ||
            obj->as<PlainObject>().hasDynamicElements())
        {
            // Only use an unboxed representation if all created objects match
            // the template shape exactly.
            return true;
        }

        for (size_t i = 0; i < templateShape->slotSpan(); i++) {
            Value val = obj->as<PlainObject>().getSlot(i);

            JSValueType& existing = properties[i].type;
            JSValueType type = val.isDouble() ? JSVAL_TYPE_DOUBLE : val.extractNonDoubleType();

            if (existing == JSVAL_TYPE_MAGIC || existing == type || UnboxedTypeIncludes(type, existing))
                existing = type;
            else if (!UnboxedTypeIncludes(existing, type))
                return true;
        }
    }

    if (objectCount <= 1) {
        // If only one of the objects has been created, it is more likely to
        // have new properties added later.
        return true;
    }

    for (size_t i = 0; i < templateShape->slotSpan(); i++) {
        // We can't use an unboxed representation if e.g. all the objects have
        // a null value for one of the properties, as we can't decide what type
        // it is supposed to have.
        if (UnboxedTypeSize(properties[i].type) == 0)
            return true;
    }

    // Fill in the names for all the object's properties.
    for (Shape::Range<NoGC> r(templateShape); !r.empty(); r.popFront()) {
        size_t slot = r.front().slot();
        MOZ_ASSERT(!properties[slot].name);
        properties[slot].name = JSID_TO_ATOM(r.front().propid())->asPropertyName();
    }

    // Fill in all the unboxed object's property offsets.
    uint32_t offset = 0;

    // Search for an existing unboxed layout which is a subset of this one.
    // If there are multiple such layouts, use the largest one. If we're able
    // to find such a layout, use the same property offsets for the shared
    // properties, which will allow us to generate better code if the objects
    // have a subtype/supertype relation and are accessed at common sites.
    UnboxedLayout* bestExisting = nullptr;
    for (UnboxedLayout* existing = cx->compartment()->unboxedLayouts.getFirst();
         existing;
         existing = existing->getNext())
    {
        if (PropertiesAreSuperset(properties, existing)) {
            if (!bestExisting ||
                existing->properties().length() > bestExisting->properties().length())
            {
                bestExisting = existing;
            }
        }
    }
    if (bestExisting) {
        for (size_t i = 0; i < bestExisting->properties().length(); i++) {
            const UnboxedLayout::Property& existingProperty = bestExisting->properties()[i];
            for (size_t j = 0; j < templateShape->slotSpan(); j++) {
                if (existingProperty.name == properties[j].name) {
                    MOZ_ASSERT(existingProperty.type == properties[j].type);
                    properties[j].offset = existingProperty.offset;
                }
            }
        }
        offset = bestExisting->size();
    }

    // Order remaining properties from the largest down for the best space
    // utilization.
    static const size_t typeSizes[] = { 8, 4, 1 };

    for (size_t i = 0; i < ArrayLength(typeSizes); i++) {
        size_t size = typeSizes[i];
        for (size_t j = 0; j < templateShape->slotSpan(); j++) {
            if (properties[j].offset != UINT32_MAX)
                continue;
            JSValueType type = properties[j].type;
            if (UnboxedTypeSize(type) == size) {
                offset = JS_ROUNDUP(offset, size);
                properties[j].offset = offset;
                offset += size;
            }
        }
    }

    // The entire object must be allocatable inline.
    if (sizeof(JSObject) + offset > JSObject::MAX_BYTE_SIZE)
        return true;

    UniquePtr<UnboxedLayout, JS::DeletePolicy<UnboxedLayout> > layout;
    layout.reset(group->zone()->new_<UnboxedLayout>(properties, offset));
    if (!layout)
        return false;

    cx->compartment()->unboxedLayouts.insertFront(layout.get());

    // Figure out the offsets of any objects or string properties.
    Vector<int32_t, 8, SystemAllocPolicy> objectOffsets, stringOffsets;
    for (size_t i = 0; i < templateShape->slotSpan(); i++) {
        MOZ_ASSERT(properties[i].offset != UINT32_MAX);
        JSValueType type = properties[i].type;
        if (type == JSVAL_TYPE_OBJECT) {
            if (!objectOffsets.append(properties[i].offset))
                return false;
        } else if (type == JSVAL_TYPE_STRING) {
            if (!stringOffsets.append(properties[i].offset))
                return false;
        }
    }

    // Construct the layout's trace list.
    if (!objectOffsets.empty() || !stringOffsets.empty()) {
        Vector<int32_t, 8, SystemAllocPolicy> entries;
        if (!entries.appendAll(stringOffsets) ||
            !entries.append(-1) ||
            !entries.appendAll(objectOffsets) ||
            !entries.append(-1) ||
            !entries.append(-1))
        {
            return false;
        }
        int32_t* traceList = group->zone()->pod_malloc<int32_t>(entries.length());
        if (!traceList)
            return false;
        PodCopy(traceList, entries.begin(), entries.length());
        layout->setTraceList(traceList);
    }

    // We've determined that all the preliminary objects can use the new layout
    // just constructed, so convert the existing group to be an
    // UnboxedPlainObject rather than a PlainObject, and update the preliminary
    // objects to use the new layout. Do the fallible stuff first before
    // modifying any objects.

    // Get an empty shape which we can use for the preliminary objects.
    Shape* newShape = EmptyShape::getInitialShape(cx, &UnboxedPlainObject::class_,
                                                  group->proto(),
                                                  templateShape->getObjectParent(),
                                                  templateShape->getObjectMetadata(),
                                                  templateShape->getObjectFlags());
    if (!newShape) {
        cx->clearPendingException();
        return false;
    }

    // Accumulate a list of all the properties in each preliminary object, and
    // update their shapes.
    Vector<Value, 0, SystemAllocPolicy> values;
    if (!values.reserve(objectCount * templateShape->slotSpan()))
        return false;
    for (size_t i = 0; i < PreliminaryObjectArray::COUNT; i++) {
        if (!objects->get(i))
            continue;

        RootedNativeObject obj(cx, &objects->get(i)->as<NativeObject>());
        for (size_t j = 0; j < templateShape->slotSpan(); j++)
            values.infallibleAppend(obj->getSlot(j));

        // Clear the object to remove any dynamically allocated information.
        NativeObject::clear(cx, obj);

        obj->setLastPropertyMakeNonNative(newShape);
    }

    if (TypeNewScript* newScript = group->newScript())
        layout->setNewScript(newScript);

    group->setClasp(&UnboxedPlainObject::class_);
    group->setUnboxedLayout(layout.get());

    size_t valueCursor = 0;
    for (size_t i = 0; i < PreliminaryObjectArray::COUNT; i++) {
        if (!objects->get(i))
            continue;
        UnboxedPlainObject* obj = &objects->get(i)->as<UnboxedPlainObject>();
        memset(obj->data(), 0, layout->size());
        for (size_t j = 0; j < templateShape->slotSpan(); j++) {
            Value v = values[valueCursor++];
            JS_ALWAYS_TRUE(obj->setValue(cx, properties[j], v));
        }
    }

    MOZ_ASSERT(valueCursor == values.length());
    layout.release();
    return true;
}
