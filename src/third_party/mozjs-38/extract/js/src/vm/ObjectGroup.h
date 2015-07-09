/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ObjectGroup_h
#define vm_ObjectGroup_h

#include "jsbytecode.h"
#include "jsfriendapi.h"

#include "ds/IdValuePair.h"
#include "gc/Barrier.h"
#include "vm/TypeInference.h"

namespace js {

class TypeDescr;
class UnboxedLayout;

class TypeNewScript;
class HeapTypeSet;
class AutoClearTypeInferenceStateOnOOM;
class CompilerConstraintList;

// Information about an object prototype, which can be either a particular
// object, null, or a lazily generated object. The latter is only used by
// certain kinds of proxies.
class TaggedProto
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

    bool operator ==(const TaggedProto& other) { return proto == other.proto; }
    bool operator !=(const TaggedProto& other) { return proto != other.proto; }

  private:
    JSObject* proto;
};

template <>
struct RootKind<TaggedProto>
{
    static ThingRootKind rootKind() { return THING_ROOT_OBJECT; }
};

template <> struct GCMethods<const TaggedProto>
{
    static TaggedProto initial() { return TaggedProto(); }
    static bool poisoned(const TaggedProto& v) { return IsPoisonedPtr(v.raw()); }
};

template <> struct GCMethods<TaggedProto>
{
    static TaggedProto initial() { return TaggedProto(); }
    static bool poisoned(const TaggedProto& v) { return IsPoisonedPtr(v.raw()); }
};

template<class Outer>
class TaggedProtoOperations
{
    const TaggedProto* value() const {
        return static_cast<const Outer*>(this)->extract();
    }

  public:
    uintptr_t toWord() const { return value()->toWord(); }
    inline bool isLazy() const { return value()->isLazy(); }
    inline bool isObject() const { return value()->isObject(); }
    inline JSObject* toObject() const { return value()->toObject(); }
    inline JSObject* toObjectOrNull() const { return value()->toObjectOrNull(); }
    JSObject* raw() const { return value()->raw(); }
};

template <>
class HandleBase<TaggedProto> : public TaggedProtoOperations<Handle<TaggedProto> >
{
    friend class TaggedProtoOperations<Handle<TaggedProto> >;
    const TaggedProto * extract() const {
        return static_cast<const Handle<TaggedProto>*>(this)->address();
    }
};

template <>
class RootedBase<TaggedProto> : public TaggedProtoOperations<Rooted<TaggedProto> >
{
    friend class TaggedProtoOperations<Rooted<TaggedProto> >;
    const TaggedProto* extract() const {
        return static_cast<const Rooted<TaggedProto>*>(this)->address();
    }
};

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

/*
 * Lazy object groups overview.
 *
 * Object groups which represent at most one JS object are constructed lazily.
 * These include groups for native functions, standard classes, scripted
 * functions defined at the top level of global/eval scripts, and in some
 * other cases. Typical web workloads often create many windows (and many
 * copies of standard natives) and many scripts, with comparatively few
 * non-singleton groups.
 *
 * We can recover the type information for the object from examining it,
 * so don't normally track the possible types of its properties as it is
 * updated. Property type sets for the object are only constructed when an
 * analyzed script attaches constraints to it: the script is querying that
 * property off the object or another which delegates to it, and the analysis
 * information is sensitive to changes in the property's type. Future changes
 * to the property (whether those uncovered by analysis or those occurring
 * in the VM) will treat these properties like those of any other object group.
 */

/* Type information about an object accessed by a script. */
class ObjectGroup : public gc::TenuredCell
{
    /* Class shared by objects in this group. */
    const Class* clasp_;

    /* Prototype shared by objects in this group. */
    HeapPtrObject proto_;

    /*
     * Whether there is a singleton JS object with this group. That JS object
     * must appear in type sets instead of this; we include the back reference
     * here to allow reverting the JS object to a lazy group.
     */
    HeapPtrObject singleton_;

  public:

    const Class* clasp() const {
        return clasp_;
    }

    void setClasp(const Class* clasp) {
        clasp_ = clasp;
    }

    TaggedProto proto() const {
        return TaggedProto(proto_);
    }

    JSObject* singleton() const {
        return singleton_;
    }

    // For use during marking, don't call otherwise.
    HeapPtrObject& protoRaw() { return proto_; }
    HeapPtrObject& singletonRaw() { return singleton_; }

    void setProto(TaggedProto proto);
    void setProtoUnchecked(TaggedProto proto);

    void initSingleton(JSObject* singleton) {
        singleton_ = singleton;
    }

    /*
     * Value held by singleton if this is a standin group for a singleton JS
     * object whose group has not been constructed yet.
     */
    static const size_t LAZY_SINGLETON = 1;
    bool lazy() const { return singleton() == (JSObject*) LAZY_SINGLETON; }

  private:
    /* Flags for this group. */
    ObjectGroupFlags flags_;

    // Kinds of addendums which can be attached to ObjectGroups.
    enum AddendumKind {
        Addendum_None,

        // When used by interpreted function, the addendum stores the
        // canonical JSFunction object.
        Addendum_InterpretedFunction,

        // When used by the 'new' group when constructing an interpreted
        // function, the addendum stores a TypeNewScript.
        Addendum_NewScript,

        // When objects in this group have an unboxed representation, the
        // addendum stores an UnboxedLayout (which might have a TypeNewScript
        // as well, if the group is also constructed using 'new').
        Addendum_UnboxedLayout,

        // If this group is used by objects that have been converted from an
        // unboxed representation, the addendum points to the original unboxed
        // group.
        Addendum_OriginalUnboxedGroup,

        // When used by typed objects, the addendum stores a TypeDescr.
        Addendum_TypeDescr
    };

    // If non-null, holds additional information about this object, whose
    // format is indicated by the object's addendum kind.
    void* addendum_;

    void setAddendum(AddendumKind kind, void* addendum, bool writeBarrier = true);

    AddendumKind addendumKind() const {
        return (AddendumKind)
            ((flags_ & OBJECT_FLAG_ADDENDUM_MASK) >> OBJECT_FLAG_ADDENDUM_SHIFT);
    }

    TypeNewScript* newScriptDontCheckGeneration() const {
        if (addendumKind() == Addendum_NewScript)
            return reinterpret_cast<TypeNewScript*>(addendum_);
        return nullptr;
    }

    UnboxedLayout* maybeUnboxedLayoutDontCheckGeneration() const {
        if (addendumKind() == Addendum_UnboxedLayout)
            return reinterpret_cast<UnboxedLayout*>(addendum_);
        return nullptr;
    }

    TypeNewScript* anyNewScript();
    void detachNewScript(bool writeBarrier);

    ObjectGroupFlags flagsDontCheckGeneration() {
        return flags_;
    }

  public:

    ObjectGroupFlags flags() {
        maybeSweep(nullptr);
        return flagsDontCheckGeneration();
    }

    void addFlags(ObjectGroupFlags flags) {
        maybeSweep(nullptr);
        flags_ |= flags;
    }

    void clearFlags(ObjectGroupFlags flags) {
        maybeSweep(nullptr);
        flags_ &= ~flags;
    }

    TypeNewScript* newScript() {
        maybeSweep(nullptr);
        return newScriptDontCheckGeneration();
    }

    void setNewScript(TypeNewScript* newScript) {
        setAddendum(Addendum_NewScript, newScript);
    }

    UnboxedLayout* maybeUnboxedLayout() {
        maybeSweep(nullptr);
        return maybeUnboxedLayoutDontCheckGeneration();
    }

    UnboxedLayout& unboxedLayout() {
        MOZ_ASSERT(addendumKind() == Addendum_UnboxedLayout);
        return *maybeUnboxedLayout();
    }

    void setUnboxedLayout(UnboxedLayout* layout) {
        setAddendum(Addendum_UnboxedLayout, layout);
    }

    ObjectGroup* maybeOriginalUnboxedGroup() const {
        if (addendumKind() == Addendum_OriginalUnboxedGroup)
            return reinterpret_cast<ObjectGroup*>(addendum_);
        return nullptr;
    }

    void setOriginalUnboxedGroup(ObjectGroup* group) {
        setAddendum(Addendum_OriginalUnboxedGroup, group);
    }

    TypeDescr* maybeTypeDescr() {
        // Note: there is no need to sweep when accessing the type descriptor
        // of an object, as it is strongly held and immutable.
        if (addendumKind() == Addendum_TypeDescr)
            return reinterpret_cast<TypeDescr*>(addendum_);
        return nullptr;
    }

    TypeDescr& typeDescr() {
        MOZ_ASSERT(addendumKind() == Addendum_TypeDescr);
        return *maybeTypeDescr();
    }

    void setTypeDescr(TypeDescr* descr) {
        setAddendum(Addendum_TypeDescr, descr);
    }

    JSFunction* maybeInterpretedFunction() {
        // Note: as with type descriptors, there is no need to sweep when
        // accessing the interpreted function associated with an object.
        if (addendumKind() == Addendum_InterpretedFunction)
            return reinterpret_cast<JSFunction*>(addendum_);
        return nullptr;
    }

    void setInterpretedFunction(JSFunction* fun) {
        setAddendum(Addendum_InterpretedFunction, fun);
    }

    class Property
    {
      public:
        // Identifier for this property, JSID_VOID for the aggregate integer
        // index property, or JSID_EMPTY for properties holding constraints
        // listening to changes in the group's state.
        HeapId id;

        // Possible own types for this property.
        HeapTypeSet types;

        explicit Property(jsid id)
          : id(id)
        {}

        Property(const Property& o)
          : id(o.id.get()), types(o.types)
        {}

        static uint32_t keyBits(jsid id) { return uint32_t(JSID_BITS(id)); }
        static jsid getKey(Property* p) { return p->id; }
    };

  private:
    /*
     * Properties of this object.
     *
     * The type sets in the properties of a group describe the possible values
     * that can be read out of that property in actual JS objects. In native
     * objects, property types account for plain data properties (those with a
     * slot and no getter or setter hook) and dense elements. In typed objects
     * and unboxed objects, property types account for object and value
     * properties and elements in the object.
     *
     * For accesses on these properties, the correspondence is as follows:
     *
     * 1. If the group has unknownProperties(), the possible properties and
     *    value types for associated JSObjects are unknown.
     *
     * 2. Otherwise, for any |obj| in |group|, and any |id| which is a property
     *    in |obj|, before obj->getProperty(id) the property in |group| for
     *    |id| must reflect the result of the getProperty.
     *
     * There are several exceptions to this:
     *
     * 1. For properties of global JS objects which are undefined at the point
     *    where the property was (lazily) generated, the property type set will
     *    remain empty, and the 'undefined' type will only be added after a
     *    subsequent assignment or deletion. After these properties have been
     *    assigned a defined value, the only way they can become undefined
     *    again is after such an assign or deletion.
     *
     * 2. Array lengths are special cased by the compiler and VM and are not
     *    reflected in property types.
     *
     * 3. In typed objects (but not unboxed objects), the initial values of
     *    properties (null pointers and undefined values) are not reflected in
     *    the property types. These values are always possible when reading the
     *    property.
     *
     * We establish these by using write barriers on calls to setProperty and
     * defineProperty which are on native properties, and on any jitcode which
     * might update the property with a new type.
     */
    Property** propertySet;
  public:

    inline ObjectGroup(const Class* clasp, TaggedProto proto, ObjectGroupFlags initialFlags);

    inline bool hasAnyFlags(ObjectGroupFlags flags) {
        MOZ_ASSERT((flags & OBJECT_FLAG_DYNAMIC_MASK) == flags);
        return !!(this->flags() & flags);
    }

    bool hasAllFlags(ObjectGroupFlags flags) {
        MOZ_ASSERT((flags & OBJECT_FLAG_DYNAMIC_MASK) == flags);
        return (this->flags() & flags) == flags;
    }

    bool hasAllFlagsDontCheckGeneration(ObjectGroupFlags flags) {
        MOZ_ASSERT((flags & OBJECT_FLAG_DYNAMIC_MASK) == flags);
        return (this->flagsDontCheckGeneration() & flags) == flags;
    }

    bool unknownProperties() {
        MOZ_ASSERT_IF(flags() & OBJECT_FLAG_UNKNOWN_PROPERTIES,
                      hasAllFlags(OBJECT_FLAG_DYNAMIC_MASK));
        return !!(flags() & OBJECT_FLAG_UNKNOWN_PROPERTIES);
    }

    bool unknownPropertiesDontCheckGeneration() {
        MOZ_ASSERT_IF(flagsDontCheckGeneration() & OBJECT_FLAG_UNKNOWN_PROPERTIES,
                      hasAllFlagsDontCheckGeneration(OBJECT_FLAG_DYNAMIC_MASK));
        return !!(flagsDontCheckGeneration() & OBJECT_FLAG_UNKNOWN_PROPERTIES);
    }

    bool shouldPreTenure() {
        return hasAnyFlags(OBJECT_FLAG_PRE_TENURE) && !unknownProperties();
    }

    gc::InitialHeap initialHeap(CompilerConstraintList* constraints);

    bool canPreTenure() {
        return !unknownProperties();
    }

    bool fromAllocationSite() {
        return flags() & OBJECT_FLAG_FROM_ALLOCATION_SITE;
    }

    void setShouldPreTenure(ExclusiveContext* cx) {
        MOZ_ASSERT(canPreTenure());
        setFlags(cx, OBJECT_FLAG_PRE_TENURE);
    }

    /*
     * Get or create a property of this object. Only call this for properties which
     * a script accesses explicitly.
     */
    inline HeapTypeSet* getProperty(ExclusiveContext* cx, jsid id);

    /* Get a property only if it already exists. */
    inline HeapTypeSet* maybeGetProperty(jsid id);

    inline unsigned getPropertyCount();
    inline Property* getProperty(unsigned i);

    /* Helpers */

    void updateNewPropertyTypes(ExclusiveContext* cx, jsid id, HeapTypeSet* types);
    bool addDefiniteProperties(ExclusiveContext* cx, Shape* shape);
    bool matchDefiniteProperties(HandleObject obj);
    void markPropertyNonData(ExclusiveContext* cx, jsid id);
    void markPropertyNonWritable(ExclusiveContext* cx, jsid id);
    void markStateChange(ExclusiveContext* cx);
    void setFlags(ExclusiveContext* cx, ObjectGroupFlags flags);
    void markUnknown(ExclusiveContext* cx);
    void maybeClearNewScriptOnOOM();
    void clearNewScript(ExclusiveContext* cx);
    bool isPropertyNonData(jsid id);
    bool isPropertyNonWritable(jsid id);

    void print();

    inline void clearProperties();
    void maybeSweep(AutoClearTypeInferenceStateOnOOM* oom);

  private:
#ifdef DEBUG
    bool needsSweep();
#endif

    uint32_t generation() {
        return (flags_ & OBJECT_FLAG_GENERATION_MASK) >> OBJECT_FLAG_GENERATION_SHIFT;
    }

  public:
    void setGeneration(uint32_t generation) {
        MOZ_ASSERT(generation <= (OBJECT_FLAG_GENERATION_MASK >> OBJECT_FLAG_GENERATION_SHIFT));
        flags_ &= ~OBJECT_FLAG_GENERATION_MASK;
        flags_ |= generation << OBJECT_FLAG_GENERATION_SHIFT;
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

    void finalize(FreeOp* fop);
    void fixupAfterMovingGC() {}

    static inline ThingRootKind rootKind() { return THING_ROOT_OBJECT_GROUP; }

    static inline uint32_t offsetOfClasp() {
        return offsetof(ObjectGroup, clasp_);
    }

    static inline uint32_t offsetOfProto() {
        return offsetof(ObjectGroup, proto_);
    }

    static inline uint32_t offsetOfAddendum() {
        return offsetof(ObjectGroup, addendum_);
    }

    static inline uint32_t offsetOfFlags() {
        return offsetof(ObjectGroup, flags_);
    }

  private:
    inline uint32_t basePropertyCount();
    inline void setBasePropertyCount(uint32_t count);

    static void staticAsserts() {
        JS_STATIC_ASSERT(offsetof(ObjectGroup, proto_) == offsetof(js::shadow::ObjectGroup, proto));
    }

  public:
    // Whether to make a deep cloned singleton when cloning fun.
    static bool useSingletonForClone(JSFunction* fun);

    // Whether to make a singleton when calling 'new' at script/pc.
    static bool useSingletonForNewObject(JSContext* cx, JSScript* script, jsbytecode* pc);

    // Whether to make a singleton object at an allocation site.
    static bool useSingletonForAllocationSite(JSScript* script, jsbytecode* pc,
                                              JSProtoKey key);
    static bool useSingletonForAllocationSite(JSScript* script, jsbytecode* pc,
                                              const Class* clasp);

    // Static accessors for ObjectGroupCompartment NewTable.

    static ObjectGroup* defaultNewGroup(ExclusiveContext* cx, const Class* clasp,
                                        TaggedProto proto,
                                        JSObject* associated = nullptr);
    static ObjectGroup* lazySingletonGroup(ExclusiveContext* cx, const Class* clasp,
                                           TaggedProto proto);

    static void setDefaultNewGroupUnknown(JSContext* cx, const js::Class* clasp, JS::HandleObject obj);

#ifdef DEBUG
    static bool hasDefaultNewGroup(JSObject* proto, const Class* clasp, ObjectGroup* group);
#endif

    // Static accessors for ObjectGroupCompartment ArrayObjectTable and PlainObjectTable.

    // Update the group of a freshly created array or plain object according to
    // the object's current contents.
    static void fixArrayGroup(ExclusiveContext* cx, ArrayObject* obj);
    static void fixPlainObjectGroup(ExclusiveContext* cx, PlainObject* obj);

    // Update the group of a freshly created 'rest' arguments object.
    static void fixRestArgumentsGroup(ExclusiveContext* cx, ArrayObject* obj);

    static PlainObject* newPlainObject(JSContext* cx, IdValuePair* properties, size_t nproperties);

    // Static accessors for ObjectGroupCompartment AllocationSiteTable.

    // Get a non-singleton group to use for objects created at the specified
    // allocation site.
    static ObjectGroup* allocationSiteGroup(JSContext* cx, JSScript* script, jsbytecode* pc,
                                            JSProtoKey key);

    // Get a non-singleton group to use for objects created in a JSNative call.
    static ObjectGroup* callingAllocationSiteGroup(JSContext* cx, JSProtoKey key);

    // Set the group or singleton-ness of an object created for an allocation site.
    static bool
    setAllocationSiteObjectGroup(JSContext* cx, HandleScript script, jsbytecode* pc,
                                 HandleObject obj, bool singleton);

    static ArrayObject* getOrFixupCopyOnWriteObject(JSContext* cx, HandleScript script,
                                                    jsbytecode* pc);
    static ArrayObject* getCopyOnWriteObject(JSScript* script, jsbytecode* pc);

    // Returns false if not found.
    static bool findAllocationSite(JSContext* cx, ObjectGroup* group,
                                   JSScript** script, uint32_t* offset);

  private:
    static ObjectGroup* defaultNewGroup(JSContext* cx, JSProtoKey key);
    static void setGroupToHomogenousArray(ExclusiveContext* cx, JSObject* obj,
                                          TypeSet::Type type);
};

// Structure used to manage the groups in a compartment.
class ObjectGroupCompartment
{
    friend class ObjectGroup;

    struct NewEntry;
    typedef HashSet<NewEntry, NewEntry, SystemAllocPolicy> NewTable;
    class NewTableRef;

    // Set of default 'new' or lazy groups in the compartment.
    NewTable* defaultNewTable;
    NewTable* lazyTable;

    struct ArrayObjectKey;
    typedef HashMap<ArrayObjectKey,
                    ReadBarrieredObjectGroup,
                    ArrayObjectKey,
                    SystemAllocPolicy> ArrayObjectTable;

    struct PlainObjectKey;
    struct PlainObjectEntry;
    typedef HashMap<PlainObjectKey,
                    PlainObjectEntry,
                    PlainObjectKey,
                    SystemAllocPolicy> PlainObjectTable;

    // Tables for managing groups common to the contents of large script
    // singleton objects and JSON objects. These are vanilla ArrayObjects and
    // PlainObjects, so we distinguish the groups of different ones by looking
    // at the types of their properties.
    //
    // All singleton/JSON arrays which have the same prototype, are homogenous
    // and of the same element type will share a group. All singleton/JSON
    // objects which have the same shape and property types will also share a
    // group. We don't try to collate arrays or objects with type mismatches.
    ArrayObjectTable* arrayObjectTable;
    PlainObjectTable* plainObjectTable;

    struct AllocationSiteKey;
    typedef HashMap<AllocationSiteKey,
                    ReadBarrieredObjectGroup,
                    AllocationSiteKey,
                    SystemAllocPolicy> AllocationSiteTable;

    // Table for referencing types of objects keyed to an allocation site.
    AllocationSiteTable* allocationSiteTable;

  public:
    ObjectGroupCompartment();
    ~ObjectGroupCompartment();

    void removeDefaultNewGroup(const Class* clasp, TaggedProto proto, JSObject* associated);
    void replaceDefaultNewGroup(const Class* clasp, TaggedProto proto, JSObject* associated,
                                ObjectGroup* group);

    static ObjectGroup* makeGroup(ExclusiveContext* cx, const Class* clasp,
                                  Handle<TaggedProto> proto,
                                  ObjectGroupFlags initialFlags = 0);

    void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                size_t* allocationSiteTables,
                                size_t* arrayGroupTables,
                                size_t* plainObjectGroupTables,
                                size_t* compartmentTables);

    void clearTables();

    void sweep(FreeOp* fop);

#ifdef JSGC_HASH_TABLE_CHECKS
    void checkTablesAfterMovingGC() {
        checkNewTableAfterMovingGC(defaultNewTable);
        checkNewTableAfterMovingGC(lazyTable);
    }
#endif

    void fixupTablesAfterMovingGC() {
        fixupNewTableAfterMovingGC(defaultNewTable);
        fixupNewTableAfterMovingGC(lazyTable);
    }

  private:
#ifdef JSGC_HASH_TABLE_CHECKS
    void checkNewTableAfterMovingGC(NewTable* table);
#endif

    void sweepNewTable(NewTable* table);
    void fixupNewTableAfterMovingGC(NewTable* table);

    static void newTablePostBarrier(ExclusiveContext* cx, NewTable* table,
                                    const Class* clasp, TaggedProto proto, JSObject* associated);
    static void updatePlainObjectEntryTypes(ExclusiveContext* cx, PlainObjectEntry& entry,
                                            IdValuePair* properties, size_t nproperties);
};

} // namespace js

#endif /* vm_ObjectGroup_h */
