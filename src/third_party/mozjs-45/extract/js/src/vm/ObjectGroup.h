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
#include "vm/TaggedProto.h"
#include "vm/TypeInference.h"

namespace js {

class TypeDescr;
class UnboxedLayout;

class PreliminaryObjectArrayWithTemplate;
class TypeNewScript;
class HeapTypeSet;
class AutoClearTypeInferenceStateOnOOM;
class CompilerConstraintList;

namespace gc {
void MergeCompartments(JSCompartment* source, JSCompartment* target);
} // namespace gc

/*
 * The NewObjectKind allows an allocation site to specify the type properties
 * and lifetime requirements that must be fixed at allocation time.
 */
enum NewObjectKind {
    /* This is the default. Most objects are generic. */
    GenericObject,

    /*
     * Singleton objects are treated specially by the type system. This flag
     * ensures that the new object is automatically set up correctly as a
     * singleton and is allocated in the tenured heap.
     */
    SingletonObject,

    /*
     * Objects which will not benefit from being allocated in the nursery
     * (e.g. because they are known to have a long lifetime) may be allocated
     * with this kind to place them immediately into the tenured generation.
     */
    TenuredObject
};

/*
 * Lazy object groups overview.
 *
 * Object groups which represent at most one JS object are constructed lazily.
 * These include groups for native functions, standard classes, scripted
 * functions defined at the top level of global/eval scripts, objects which
 * dynamically become the prototype of some other object, and in some other
 * cases. Typical web workloads often create many windows (and many copies of
 * standard natives) and many scripts, with comparatively few non-singleton
 * groups.
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
    friend void gc::MergeCompartments(JSCompartment* source, JSCompartment* target);

    /* Class shared by objects in this group. */
    const Class* clasp_;

    /* Prototype shared by objects in this group. */
    HeapPtr<TaggedProto> proto_;

    /* Compartment shared by objects in this group. */
    JSCompartment* compartment_;

  public:

    const Class* clasp() const {
        return clasp_;
    }

    void setClasp(const Class* clasp) {
        clasp_ = clasp;
    }

    const HeapPtr<TaggedProto>& proto() const {
        return proto_;
    }

    HeapPtr<TaggedProto>& proto() {
        return proto_;
    }

    void setProto(TaggedProto proto);
    void setProtoUnchecked(TaggedProto proto);

    bool singleton() const {
        return flagsDontCheckGeneration() & OBJECT_FLAG_SINGLETON;
    }

    bool lazy() const {
        bool res = flagsDontCheckGeneration() & OBJECT_FLAG_LAZY_SINGLETON;
        MOZ_ASSERT_IF(res, singleton());
        return res;
    }

    JSCompartment* compartment() const { return compartment_; }
    JSCompartment* maybeCompartment() const { return compartment(); }

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

        // For some plain objects, the addendum stores a PreliminaryObjectArrayWithTemplate.
        Addendum_PreliminaryObjects,

        // When objects in this group have an unboxed representation, the
        // addendum stores an UnboxedLayout (which might have a TypeNewScript
        // as well, if the group is also constructed using 'new').
        Addendum_UnboxedLayout,

        // If this group is used by objects that have been converted from an
        // unboxed representation and/or have the same allocation kind as such
        // objects, the addendum points to that unboxed group.
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

    TypeNewScript* anyNewScript();
    void detachNewScript(bool writeBarrier, ObjectGroup* replacement);

    ObjectGroupFlags flagsDontCheckGeneration() const {
        return flags_;
    }

  public:

    inline ObjectGroupFlags flags();
    inline void addFlags(ObjectGroupFlags flags);
    inline void clearFlags(ObjectGroupFlags flags);
    inline TypeNewScript* newScript();

    void setNewScript(TypeNewScript* newScript) {
        setAddendum(Addendum_NewScript, newScript);
    }

    inline PreliminaryObjectArrayWithTemplate* maybePreliminaryObjects();

    PreliminaryObjectArrayWithTemplate* maybePreliminaryObjectsDontCheckGeneration() {
        if (addendumKind() == Addendum_PreliminaryObjects)
            return reinterpret_cast<PreliminaryObjectArrayWithTemplate*>(addendum_);
        return nullptr;
    }

    void setPreliminaryObjects(PreliminaryObjectArrayWithTemplate* preliminaryObjects) {
        setAddendum(Addendum_PreliminaryObjects, preliminaryObjects);
    }

    void detachPreliminaryObjects() {
        MOZ_ASSERT(maybePreliminaryObjectsDontCheckGeneration());
        setAddendum(Addendum_None, nullptr);
    }

    bool hasUnanalyzedPreliminaryObjects() {
        return (newScriptDontCheckGeneration() && !newScriptDontCheckGeneration()->analyzed()) ||
               maybePreliminaryObjectsDontCheckGeneration();
    }

    inline UnboxedLayout* maybeUnboxedLayout();
    inline UnboxedLayout& unboxedLayout();

    UnboxedLayout* maybeUnboxedLayoutDontCheckGeneration() const {
        if (addendumKind() == Addendum_UnboxedLayout)
            return reinterpret_cast<UnboxedLayout*>(addendum_);
        return nullptr;
    }

    UnboxedLayout& unboxedLayoutDontCheckGeneration() const {
        MOZ_ASSERT(addendumKind() == Addendum_UnboxedLayout);
        return *maybeUnboxedLayoutDontCheckGeneration();
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
     * properties and elements in the object, and expando properties in unboxed
     * objects.
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

    inline ObjectGroup(const Class* clasp, TaggedProto proto, JSCompartment* comp,
                       ObjectGroupFlags initialFlags);

    inline bool hasAnyFlags(ObjectGroupFlags flags);
    inline bool hasAllFlags(ObjectGroupFlags flags);

    bool hasAllFlagsDontCheckGeneration(ObjectGroupFlags flags) {
        MOZ_ASSERT((flags & OBJECT_FLAG_DYNAMIC_MASK) == flags);
        return (this->flagsDontCheckGeneration() & flags) == flags;
    }

    inline bool unknownProperties();

    bool unknownPropertiesDontCheckGeneration() {
        MOZ_ASSERT_IF(flagsDontCheckGeneration() & OBJECT_FLAG_UNKNOWN_PROPERTIES,
                      hasAllFlagsDontCheckGeneration(OBJECT_FLAG_DYNAMIC_MASK));
        return !!(flagsDontCheckGeneration() & OBJECT_FLAG_UNKNOWN_PROPERTIES);
    }

    inline bool shouldPreTenure();

    gc::InitialHeap initialHeap(CompilerConstraintList* constraints);

    inline bool canPreTenure();
    inline bool fromAllocationSite();
    inline void setShouldPreTenure(ExclusiveContext* cx);

    /*
     * Get or create a property of this object. Only call this for properties which
     * a script accesses explicitly.
     */
    inline HeapTypeSet* getProperty(ExclusiveContext* cx, JSObject* obj, jsid id);

    /* Get a property only if it already exists. */
    inline HeapTypeSet* maybeGetProperty(jsid id);

    /*
     * Iterate through the group's properties. getPropertyCount overapproximates
     * in the hash case (see SET_ARRAY_SIZE in TypeInference-inl.h), and
     * getProperty may return nullptr.
     */
    inline unsigned getPropertyCount();
    inline Property* getProperty(unsigned i);

    /* Helpers */

    void updateNewPropertyTypes(ExclusiveContext* cx, JSObject* obj, jsid id, HeapTypeSet* types);
    void addDefiniteProperties(ExclusiveContext* cx, Shape* shape);
    bool matchDefiniteProperties(HandleObject obj);
    void markPropertyNonData(ExclusiveContext* cx, JSObject* obj, jsid id);
    void markPropertyNonWritable(ExclusiveContext* cx, JSObject* obj, jsid id);
    void markStateChange(ExclusiveContext* cx);
    void setFlags(ExclusiveContext* cx, ObjectGroupFlags flags);
    void markUnknown(ExclusiveContext* cx);
    void maybeClearNewScriptOnOOM();
    void clearNewScript(ExclusiveContext* cx, ObjectGroup* replacement = nullptr);

    void print();

    inline void clearProperties();
    void traceChildren(JSTracer* trc);

    inline bool needsSweep();
    inline void maybeSweep(AutoClearTypeInferenceStateOnOOM* oom);

  private:
    void sweep(AutoClearTypeInferenceStateOnOOM* oom);

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

    const ObjectGroupFlags* addressOfFlags() const {
        return &flags_;
    }

    // Get the bit pattern stored in an object's addendum when it has an
    // original unboxed group.
    static inline int32_t addendumOriginalUnboxedGroupValue() {
        return Addendum_OriginalUnboxedGroup << OBJECT_FLAG_ADDENDUM_SHIFT;
    }

    inline uint32_t basePropertyCount();

  private:
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

    enum class NewArrayKind {
        Normal,       // Specialize array group based on its element type.
        CopyOnWrite,  // Make an array with copy-on-write elements.
        UnknownIndex  // Make an array with an unknown element type.
    };

    // Create an ArrayObject or UnboxedArrayObject with the specified elements
    // and a group specialized for the elements.
    static JSObject* newArrayObject(ExclusiveContext* cx, const Value* vp, size_t length,
                                    NewObjectKind newKind,
                                    NewArrayKind arrayKind = NewArrayKind::Normal);

    // Create a PlainObject or UnboxedPlainObject with the specified properties
    // and a group specialized for those properties.
    static JSObject* newPlainObject(ExclusiveContext* cx,
                                    IdValuePair* properties, size_t nproperties,
                                    NewObjectKind newKind);

    // Static accessors for ObjectGroupCompartment AllocationSiteTable.

    // Get a non-singleton group to use for objects created at the specified
    // allocation site.
    static ObjectGroup* allocationSiteGroup(JSContext* cx, JSScript* script, jsbytecode* pc,
                                            JSProtoKey key, HandleObject proto = nullptr);

    // Get a non-singleton group to use for objects created in a JSNative call.
    static ObjectGroup* callingAllocationSiteGroup(JSContext* cx, JSProtoKey key,
                                                   HandleObject proto = nullptr);

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

    void replaceAllocationSiteGroup(JSScript* script, jsbytecode* pc,
                                    JSProtoKey kind, ObjectGroup* group);

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
};

PlainObject*
NewPlainObjectWithProperties(ExclusiveContext* cx, IdValuePair* properties, size_t nproperties,
                             NewObjectKind newKind);

bool
CombineArrayElementTypes(ExclusiveContext* cx, JSObject* newObj,
                         const Value* compare, size_t ncompare);

bool
CombinePlainObjectPropertyTypes(ExclusiveContext* cx, JSObject* newObj,
                                const Value* compare, size_t ncompare);

} // namespace js

#endif /* vm_ObjectGroup_h */
