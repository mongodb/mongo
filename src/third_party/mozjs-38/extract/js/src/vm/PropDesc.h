/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PropDesc_h
#define vm_PropDesc_h

#include "jsapi.h"
#include "NamespaceImports.h"

namespace js {

static inline JSPropertyOp
CastAsPropertyOp(JSObject* object)
{
    return JS_DATA_TO_FUNC_PTR(JSPropertyOp, object);
}

static inline JSStrictPropertyOp
CastAsStrictPropertyOp(JSObject* object)
{
    return JS_DATA_TO_FUNC_PTR(JSStrictPropertyOp, object);
}

/*
 * A representation of ECMA-262 ed. 5's internal Property Descriptor data
 * structure.
 */
struct PropDesc {
  private:
    Value value_, get_, set_;

    /* Property descriptor boolean fields. */
    uint8_t attrs;

    /* Bits indicating which values are set. */
    bool hasGet_ : 1;
    bool hasSet_ : 1;
    bool hasValue_ : 1;
    bool hasWritable_ : 1;
    bool hasEnumerable_ : 1;
    bool hasConfigurable_ : 1;

    /* Or maybe this represents a property's absence, and it's undefined. */
    bool isUndefined_ : 1;

    explicit PropDesc(const Value& v)
      : value_(v),
        get_(UndefinedValue()), set_(UndefinedValue()),
        attrs(0),
        hasGet_(false), hasSet_(false),
        hasValue_(true), hasWritable_(false), hasEnumerable_(false), hasConfigurable_(false),
        isUndefined_(false)
    {
    }

  public:
    friend struct GCMethods<PropDesc>;

    void trace(JSTracer* trc);
    static ThingRootKind rootKind() { return THING_ROOT_PROP_DESC; }

    enum Enumerability { Enumerable = true, NonEnumerable = false };
    enum Configurability { Configurable = true, NonConfigurable = false };
    enum Writability { Writable = true, NonWritable = false };

    PropDesc();

    static PropDesc undefined() { return PropDesc(); }
    static PropDesc valueOnly(const Value& v) { return PropDesc(v); }

    PropDesc(const Value& v, Writability writable,
             Enumerability enumerable, Configurability configurable)
      : value_(v),
        get_(UndefinedValue()), set_(UndefinedValue()),
        attrs((writable ? 0 : JSPROP_READONLY) |
              (enumerable ? JSPROP_ENUMERATE : 0) |
              (configurable ? 0 : JSPROP_PERMANENT)),
        hasGet_(false), hasSet_(false),
        hasValue_(true), hasWritable_(true), hasEnumerable_(true), hasConfigurable_(true),
        isUndefined_(false)
    {}

    inline PropDesc(const Value& getter, const Value& setter,
                    Enumerability enumerable, Configurability configurable);

    /*
     * 8.10.5 ToPropertyDescriptor(Obj)
     *
     * If checkAccessors is false, skip steps 7.b and 8.b, which throw a
     * TypeError if .get or .set is neither a callable object nor undefined.
     *
     * (DebuggerObject_defineProperty uses this: the .get and .set properties
     * are expected to be Debugger.Object wrappers of functions, which are not
     * themselves callable.)
     */
    bool initialize(JSContext* cx, const Value& v, bool checkAccessors = true);

    /*
     * If IsGenericDescriptor(desc) or IsDataDescriptor(desc) is true, then if
     * the value of an attribute field of desc, considered as a data
     * descriptor, is absent, set it to its default value. Else if the value of
     * an attribute field of desc, considered as an attribute descriptor, is
     * absent, set it to its default value.
     */
    void complete();

    /*
     * 8.10.4 FromPropertyDescriptor(Desc)
     *
     * initFromPropertyDescriptor sets pd to undefined and populates all the
     * other fields of this PropDesc from desc.
     *
     * makeObject populates pd based on the other fields of *this, creating a
     * new property descriptor JSObject and defining properties on it.
     */
    void initFromPropertyDescriptor(Handle<JSPropertyDescriptor> desc);
    void populatePropertyDescriptor(HandleObject obj, MutableHandle<JSPropertyDescriptor> desc) const;
    bool makeObject(JSContext* cx, MutableHandleObject objp);

    /* Reset the descriptor entirely. */
    void setUndefined();
    bool isUndefined() const { return isUndefined_; }

    bool hasGet() const { MOZ_ASSERT(!isUndefined()); return hasGet_; }
    bool hasSet() const { MOZ_ASSERT(!isUndefined()); return hasSet_; }
    bool hasValue() const { MOZ_ASSERT(!isUndefined()); return hasValue_; }
    bool hasWritable() const { MOZ_ASSERT(!isUndefined()); return hasWritable_; }
    bool hasEnumerable() const { MOZ_ASSERT(!isUndefined()); return hasEnumerable_; }
    bool hasConfigurable() const { MOZ_ASSERT(!isUndefined()); return hasConfigurable_; }

    uint8_t attributes() const { MOZ_ASSERT(!isUndefined()); return attrs; }

    /* 8.10.1 IsAccessorDescriptor(desc) */
    bool isAccessorDescriptor() const {
        return !isUndefined() && (hasGet() || hasSet());
    }

    /* 8.10.2 IsDataDescriptor(desc) */
    bool isDataDescriptor() const {
        return !isUndefined() && (hasValue() || hasWritable());
    }

    /* 8.10.3 IsGenericDescriptor(desc) */
    bool isGenericDescriptor() const {
        return !isUndefined() && !isAccessorDescriptor() && !isDataDescriptor();
    }

    bool configurable() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasConfigurable());
        return (attrs & JSPROP_PERMANENT) == 0;
    }

    bool enumerable() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasEnumerable());
        return (attrs & JSPROP_ENUMERATE) != 0;
    }

    bool writable() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasWritable());
        return (attrs & JSPROP_READONLY) == 0;
    }

    HandleValue value() const {
        MOZ_ASSERT(hasValue());
        return HandleValue::fromMarkedLocation(&value_);
    }
    void setValue(const Value& value) {
        MOZ_ASSERT(!isUndefined());
        value_ = value;
        hasValue_ = true;
    }

    JSObject * getterObject() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasGet());
        return get_.isUndefined() ? nullptr : &get_.toObject();
    }
    JSObject * setterObject() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasSet());
        return set_.isUndefined() ? nullptr : &set_.toObject();
    }

    HandleValue getterValue() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasGet());
        return HandleValue::fromMarkedLocation(&get_);
    }
    HandleValue setterValue() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasSet());
        return HandleValue::fromMarkedLocation(&set_);
    }

    void setGetter(const Value& getter) {
        MOZ_ASSERT(!isUndefined());
        get_ = getter;
        hasGet_ = true;
    }
    void setSetter(const Value& setter) {
        MOZ_ASSERT(!isUndefined());
        set_ = setter;
        hasSet_ = true;
    }

    /*
     * Unfortunately the values produced by these methods are used such that
     * we can't assert anything here.  :-(
     */
    JSPropertyOp getter() const {
        return CastAsPropertyOp(get_.isUndefined() ? nullptr : &get_.toObject());
    }
    JSStrictPropertyOp setter() const {
        return CastAsStrictPropertyOp(set_.isUndefined() ? nullptr : &set_.toObject());
    }

    /*
     * Throw a TypeError if a getter/setter is present and is neither callable
     * nor undefined. These methods do exactly the type checks that are skipped
     * by passing false as the checkAccessors parameter of initialize.
     */
    bool checkGetter(JSContext* cx);
    bool checkSetter(JSContext* cx);
};

} /* namespace js */

namespace JS {

template <typename Outer>
class PropDescOperations
{
    const js::PropDesc * desc() const { return static_cast<const Outer*>(this)->extract(); }

  public:
    bool isUndefined() const { return desc()->isUndefined(); }

    bool hasGet() const { return desc()->hasGet(); }
    bool hasSet() const { return desc()->hasSet(); }
    bool hasValue() const { return desc()->hasValue(); }
    bool hasWritable() const { return desc()->hasWritable(); }
    bool hasEnumerable() const { return desc()->hasEnumerable(); }
    bool hasConfigurable() const { return desc()->hasConfigurable(); }

    uint8_t attributes() const { return desc()->attributes(); }

    bool isAccessorDescriptor() const { return desc()->isAccessorDescriptor(); }
    bool isDataDescriptor() const { return desc()->isDataDescriptor(); }
    bool isGenericDescriptor() const { return desc()->isGenericDescriptor(); }
    bool configurable() const { return desc()->configurable(); }
    bool enumerable() const { return desc()->enumerable(); }
    bool writable() const { return desc()->writable(); }

    HandleValue value() const { return desc()->value(); }
    JSObject* getterObject() const { return desc()->getterObject(); }
    JSObject* setterObject() const { return desc()->setterObject(); }
    HandleValue getterValue() const { return desc()->getterValue(); }
    HandleValue setterValue() const { return desc()->setterValue(); }

    JSPropertyOp getter() const { return desc()->getter(); }
    JSStrictPropertyOp setter() const { return desc()->setter(); }

    void populatePropertyDescriptor(HandleObject obj,
                                    MutableHandle<JSPropertyDescriptor> descriptor) const {
        desc()->populatePropertyDescriptor(obj, descriptor);
    }
};

template <typename Outer>
class MutablePropDescOperations : public PropDescOperations<Outer>
{
    js::PropDesc * desc() { return static_cast<Outer*>(this)->extractMutable(); }

  public:

    bool initialize(JSContext* cx, const Value& v, bool checkAccessors = true) {
        return desc()->initialize(cx, v, checkAccessors);
    }
    void complete() {
        desc()->complete();
    }

    bool checkGetter(JSContext* cx) { return desc()->checkGetter(cx); }
    bool checkSetter(JSContext* cx) { return desc()->checkSetter(cx); }

    void initFromPropertyDescriptor(Handle<JSPropertyDescriptor> descriptor) {
        desc()->initFromPropertyDescriptor(descriptor);
    }
    bool makeObject(JSContext* cx, MutableHandleObject objp) {
        return desc()->makeObject(cx, objp);
    }

    void setValue(const Value& value) {
        desc()->setValue(value);
    }
    void setGetter(const Value& getter) {
        desc()->setGetter(getter);
    }
    void setSetter(const Value& setter) {
        desc()->setSetter(setter);
    }

    void setUndefined() { desc()->setUndefined(); }
};

} /* namespace JS */

namespace js {

template <>
struct GCMethods<PropDesc> {
    static PropDesc initial() { return PropDesc(); }
    static bool poisoned(const PropDesc& desc) {
        return JS::IsPoisonedValue(desc.value_) ||
               JS::IsPoisonedValue(desc.get_) ||
               JS::IsPoisonedValue(desc.set_);
    }
};

template <>
class RootedBase<PropDesc>
  : public JS::MutablePropDescOperations<JS::Rooted<PropDesc> >
{
    friend class JS::PropDescOperations<JS::Rooted<PropDesc> >;
    friend class JS::MutablePropDescOperations<JS::Rooted<PropDesc> >;
    const PropDesc* extract() const {
        return static_cast<const JS::Rooted<PropDesc>*>(this)->address();
    }
    PropDesc* extractMutable() {
        return static_cast<JS::Rooted<PropDesc>*>(this)->address();
    }
};

template <>
class HandleBase<PropDesc>
  : public JS::PropDescOperations<JS::Handle<PropDesc> >
{
    friend class JS::PropDescOperations<JS::Handle<PropDesc> >;
    const PropDesc* extract() const {
        return static_cast<const JS::Handle<PropDesc>*>(this)->address();
    }
};

template <>
class MutableHandleBase<PropDesc>
  : public JS::MutablePropDescOperations<JS::MutableHandle<PropDesc> >
{
    friend class JS::PropDescOperations<JS::MutableHandle<PropDesc> >;
    friend class JS::MutablePropDescOperations<JS::MutableHandle<PropDesc> >;
    const PropDesc* extract() const {
        return static_cast<const JS::MutableHandle<PropDesc>*>(this)->address();
    }
    PropDesc* extractMutable() {
        return static_cast<JS::MutableHandle<PropDesc>*>(this)->address();
    }
};

} /* namespace js */

#endif /* vm_PropDesc_h */
