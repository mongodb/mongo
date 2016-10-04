/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Definitions related to javascript type inference. */

#ifndef vm_TypeInference_h
#define vm_TypeInference_h

#include "mozilla/MemoryReporting.h"

#include "jsalloc.h"
#include "jsfriendapi.h"
#include "jstypes.h"

#include "ds/IdValuePair.h"
#include "ds/LifoAlloc.h"
#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "jit/IonTypes.h"
#include "js/UbiNode.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "vm/TaggedProto.h"

namespace js {

namespace jit {
    struct IonScript;
    class JitAllocPolicy;
    class TempAllocator;
} // namespace jit

struct TypeZone;
class TypeConstraint;
class TypeNewScript;
class CompilerConstraintList;
class HeapTypeSetKey;

/*
 * Type inference memory management overview.
 *
 * Type information about the values observed within scripts and about the
 * contents of the heap is accumulated as the program executes. Compilation
 * accumulates constraints relating type information on the heap with the
 * compilations that should be invalidated when those types change. Type
 * information and constraints are allocated in the zone's typeLifoAlloc,
 * and on GC all data referring to live things is copied into a new allocator.
 * Thus, type set and constraints only hold weak references.
 */

/* Flags and other state stored in TypeSet::flags */
enum : uint32_t {
    TYPE_FLAG_UNDEFINED =   0x1,
    TYPE_FLAG_NULL      =   0x2,
    TYPE_FLAG_BOOLEAN   =   0x4,
    TYPE_FLAG_INT32     =   0x8,
    TYPE_FLAG_DOUBLE    =  0x10,
    TYPE_FLAG_STRING    =  0x20,
    TYPE_FLAG_SYMBOL    =  0x40,
    TYPE_FLAG_LAZYARGS  =  0x80,
    TYPE_FLAG_ANYOBJECT = 0x100,

    /* Mask containing all primitives */
    TYPE_FLAG_PRIMITIVE = TYPE_FLAG_UNDEFINED | TYPE_FLAG_NULL | TYPE_FLAG_BOOLEAN |
                          TYPE_FLAG_INT32 | TYPE_FLAG_DOUBLE | TYPE_FLAG_STRING |
                          TYPE_FLAG_SYMBOL,

    /* Mask/shift for the number of objects in objectSet */
    TYPE_FLAG_OBJECT_COUNT_MASK     = 0x3e00,
    TYPE_FLAG_OBJECT_COUNT_SHIFT    = 9,
    TYPE_FLAG_OBJECT_COUNT_LIMIT    = 7,
    TYPE_FLAG_DOMOBJECT_COUNT_LIMIT =
        TYPE_FLAG_OBJECT_COUNT_MASK >> TYPE_FLAG_OBJECT_COUNT_SHIFT,

    /* Whether the contents of this type set are totally unknown. */
    TYPE_FLAG_UNKNOWN             = 0x00004000,

    /* Mask of normal type flags on a type set. */
    TYPE_FLAG_BASE_MASK           = 0x000041ff,

    /* Additional flags for HeapTypeSet sets. */

    /*
     * Whether the property has ever been deleted or reconfigured to behave
     * differently from a plain data property, other than making the property
     * non-writable.
     */
    TYPE_FLAG_NON_DATA_PROPERTY = 0x00008000,

    /* Whether the property has ever been made non-writable. */
    TYPE_FLAG_NON_WRITABLE_PROPERTY = 0x00010000,

    /* Whether the property might not be constant. */
    TYPE_FLAG_NON_CONSTANT_PROPERTY = 0x00020000,

    /*
     * Whether the property is definitely in a particular slot on all objects
     * from which it has not been deleted or reconfigured. For singletons
     * this may be a fixed or dynamic slot, and for other objects this will be
     * a fixed slot.
     *
     * If the property is definite, mask and shift storing the slot + 1.
     * Otherwise these bits are clear.
     */
    TYPE_FLAG_DEFINITE_MASK       = 0xfffc0000,
    TYPE_FLAG_DEFINITE_SHIFT      = 18
};
typedef uint32_t TypeFlags;

/* Flags and other state stored in ObjectGroup::Flags */
enum : uint32_t {
    /* Whether this group is associated with some allocation site. */
    OBJECT_FLAG_FROM_ALLOCATION_SITE  = 0x1,

    /* Whether this group is associated with a single object. */
    OBJECT_FLAG_SINGLETON             = 0x2,

    /*
     * Whether this group is used by objects whose singleton groups have not
     * been created yet.
     */
    OBJECT_FLAG_LAZY_SINGLETON        = 0x4,

    /* Mask/shift for the number of properties in propertySet */
    OBJECT_FLAG_PROPERTY_COUNT_MASK   = 0xfff8,
    OBJECT_FLAG_PROPERTY_COUNT_SHIFT  = 3,
    OBJECT_FLAG_PROPERTY_COUNT_LIMIT  =
        OBJECT_FLAG_PROPERTY_COUNT_MASK >> OBJECT_FLAG_PROPERTY_COUNT_SHIFT,

    /* Whether any objects this represents may have sparse indexes. */
    OBJECT_FLAG_SPARSE_INDEXES        = 0x00010000,

    /* Whether any objects this represents may not have packed dense elements. */
    OBJECT_FLAG_NON_PACKED            = 0x00020000,

    /*
     * Whether any objects this represents may be arrays whose length does not
     * fit in an int32.
     */
    OBJECT_FLAG_LENGTH_OVERFLOW       = 0x00040000,

    /* Whether any objects have been iterated over. */
    OBJECT_FLAG_ITERATED              = 0x00080000,

    /* For a global object, whether flags were set on the RegExpStatics. */
    OBJECT_FLAG_REGEXP_FLAGS_SET      = 0x00100000,

    /*
     * For the function on a run-once script, whether the function has actually
     * run multiple times.
     */
    OBJECT_FLAG_RUNONCE_INVALIDATED   = 0x00200000,

    /*
     * For a global object, whether any array buffers in this compartment with
     * typed object views have been neutered.
     */
    OBJECT_FLAG_TYPED_OBJECT_NEUTERED = 0x00400000,

    /*
     * Whether objects with this type should be allocated directly in the
     * tenured heap.
     */
    OBJECT_FLAG_PRE_TENURE            = 0x00800000,

    /* Whether objects with this type might have copy on write elements. */
    OBJECT_FLAG_COPY_ON_WRITE         = 0x01000000,

    /* Whether this type has had its 'new' script cleared in the past. */
    OBJECT_FLAG_NEW_SCRIPT_CLEARED    = 0x02000000,

    /*
     * Whether all properties of this object are considered unknown.
     * If set, all other flags in DYNAMIC_MASK will also be set.
     */
    OBJECT_FLAG_UNKNOWN_PROPERTIES    = 0x04000000,

    /* Flags which indicate dynamic properties of represented objects. */
    OBJECT_FLAG_DYNAMIC_MASK          = 0x07ff0000,

    // Mask/shift for the kind of addendum attached to this group.
    OBJECT_FLAG_ADDENDUM_MASK         = 0x38000000,
    OBJECT_FLAG_ADDENDUM_SHIFT        = 27,

    // Mask/shift for this group's generation. If out of sync with the
    // TypeZone's generation, this group hasn't been swept yet.
    OBJECT_FLAG_GENERATION_MASK       = 0x40000000,
    OBJECT_FLAG_GENERATION_SHIFT      = 30,
};
typedef uint32_t ObjectGroupFlags;

class StackTypeSet;
class HeapTypeSet;
class TemporaryTypeSet;

/*
 * Information about the set of types associated with an lvalue. There are
 * three kinds of type sets:
 *
 * - StackTypeSet are associated with TypeScripts, for arguments and values
 *   observed at property reads. These are implicitly frozen on compilation
 *   and only have constraints added to them which can trigger invalidation of
 *   TypeNewScript information.
 *
 * - HeapTypeSet are associated with the properties of ObjectGroups. These
 *   may have constraints added to them to trigger invalidation of either
 *   compiled code or TypeNewScript information.
 *
 * - TemporaryTypeSet are created during compilation and do not outlive
 *   that compilation.
 *
 * The contents of a type set completely describe the values that a particular
 * lvalue might have, except for the following cases:
 *
 * - If an object's prototype or class is dynamically mutated, its group will
 *   change. Type sets containing the old group will not necessarily contain
 *   the new group. When this occurs, the properties of the old and new group
 *   will both be marked as unknown, which will prevent Ion from optimizing
 *   based on the object's type information.
 *
 * - If an unboxed object is converted to a native object, its group will also
 *   change and type sets containing the old group will not necessarily contain
 *   the new group. Unlike the above case, this will not degrade property type
 *   information, but Ion will no longer optimize unboxed objects with the old
 *   group.
 */
class TypeSet
{
  public:
    // Type set entry for either a JSObject with singleton type or a
    // non-singleton ObjectGroup.
    class ObjectKey {
      public:
        static intptr_t keyBits(ObjectKey* obj) { return (intptr_t) obj; }
        static ObjectKey* getKey(ObjectKey* obj) { return obj; }

        static inline ObjectKey* get(JSObject* obj);
        static inline ObjectKey* get(ObjectGroup* group);

        bool isGroup() {
            return (uintptr_t(this) & 1) == 0;
        }
        bool isSingleton() {
            return (uintptr_t(this) & 1) != 0;
        }

        inline ObjectGroup* group();
        inline JSObject* singleton();

        inline ObjectGroup* groupNoBarrier();
        inline JSObject* singletonNoBarrier();

        const Class* clasp();
        TaggedProto proto();
        TypeNewScript* newScript();

        bool unknownProperties();
        bool hasFlags(CompilerConstraintList* constraints, ObjectGroupFlags flags);
        bool hasStableClassAndProto(CompilerConstraintList* constraints);
        void watchStateChangeForInlinedCall(CompilerConstraintList* constraints);
        void watchStateChangeForTypedArrayData(CompilerConstraintList* constraints);
        void watchStateChangeForUnboxedConvertedToNative(CompilerConstraintList* constraints);
        HeapTypeSetKey property(jsid id);
        void ensureTrackedProperty(JSContext* cx, jsid id);

        ObjectGroup* maybeGroup();
    };

    // Information about a single concrete type. We pack this into one word,
    // where small values are particular primitive or other singleton types and
    // larger values are either specific JS objects or object groups.
    class Type : public JS::Traceable
    {
        friend class TypeSet;

        uintptr_t data;
        explicit Type(uintptr_t data) : data(data) {}

      public:

        uintptr_t raw() const { return data; }

        bool isPrimitive() const {
            return data < JSVAL_TYPE_OBJECT;
        }

        bool isPrimitive(JSValueType type) const {
            MOZ_ASSERT(type < JSVAL_TYPE_OBJECT);
            return (uintptr_t) type == data;
        }

        JSValueType primitive() const {
            MOZ_ASSERT(isPrimitive());
            return (JSValueType) data;
        }

        bool isMagicArguments() const {
            return primitive() == JSVAL_TYPE_MAGIC;
        }

        bool isSomeObject() const {
            return data == JSVAL_TYPE_OBJECT || data > JSVAL_TYPE_UNKNOWN;
        }

        bool isAnyObject() const {
            return data == JSVAL_TYPE_OBJECT;
        }

        bool isUnknown() const {
            return data == JSVAL_TYPE_UNKNOWN;
        }

        /* Accessors for types that are either JSObject or ObjectGroup. */

        bool isObject() const {
            MOZ_ASSERT(!isAnyObject() && !isUnknown());
            return data > JSVAL_TYPE_UNKNOWN;
        }

        bool isObjectUnchecked() const {
            return data > JSVAL_TYPE_UNKNOWN;
        }

        inline ObjectKey* objectKey() const;

        /* Accessors for JSObject types */

        bool isSingleton() const {
            return isObject() && !!(data & 1);
        }
        bool isSingletonUnchecked() const {
            return isObjectUnchecked() && !!(data & 1);
        }

        inline JSObject* singleton() const;
        inline JSObject* singletonNoBarrier() const;

        /* Accessors for ObjectGroup types */

        bool isGroup() const {
            return isObject() && !(data & 1);
        }
        bool isGroupUnchecked() const {
            return isObjectUnchecked() && !(data & 1);
        }

        inline ObjectGroup* group() const;
        inline ObjectGroup* groupNoBarrier() const;

        static void trace(Type* v, JSTracer* trc) {
            MarkTypeUnbarriered(trc, v, "TypeSet::Type");
        }

        bool operator == (Type o) const { return data == o.data; }
        bool operator != (Type o) const { return data != o.data; }
    };

    static inline Type UndefinedType() { return Type(JSVAL_TYPE_UNDEFINED); }
    static inline Type NullType()      { return Type(JSVAL_TYPE_NULL); }
    static inline Type BooleanType()   { return Type(JSVAL_TYPE_BOOLEAN); }
    static inline Type Int32Type()     { return Type(JSVAL_TYPE_INT32); }
    static inline Type DoubleType()    { return Type(JSVAL_TYPE_DOUBLE); }
    static inline Type StringType()    { return Type(JSVAL_TYPE_STRING); }
    static inline Type SymbolType()    { return Type(JSVAL_TYPE_SYMBOL); }
    static inline Type MagicArgType()  { return Type(JSVAL_TYPE_MAGIC); }
    static inline Type AnyObjectType() { return Type(JSVAL_TYPE_OBJECT); }
    static inline Type UnknownType()   { return Type(JSVAL_TYPE_UNKNOWN); }

    static inline Type PrimitiveType(JSValueType type) {
        MOZ_ASSERT(type < JSVAL_TYPE_UNKNOWN);
        return Type(type);
    }

    static inline Type ObjectType(JSObject* obj);
    static inline Type ObjectType(ObjectGroup* group);
    static inline Type ObjectType(ObjectKey* key);

    static const char* NonObjectTypeString(Type type);

#ifdef DEBUG
    static const char* TypeString(Type type);
    static const char* ObjectGroupString(ObjectGroup* group);
#else
    static const char* TypeString(Type type) { return nullptr; }
    static const char* ObjectGroupString(ObjectGroup* group) { return nullptr; }
#endif

  protected:
    /* Flags for this type set. */
    TypeFlags flags;

    /* Possible objects this type set can represent. */
    ObjectKey** objectSet;

  public:

    TypeSet()
      : flags(0), objectSet(nullptr)
    {}

    void print(FILE* fp = stderr);

    /* Whether this set contains a specific type. */
    inline bool hasType(Type type) const;

    TypeFlags baseFlags() const { return flags & TYPE_FLAG_BASE_MASK; }
    bool unknown() const { return !!(flags & TYPE_FLAG_UNKNOWN); }
    bool unknownObject() const { return !!(flags & (TYPE_FLAG_UNKNOWN | TYPE_FLAG_ANYOBJECT)); }
    bool empty() const { return !baseFlags() && !baseObjectCount(); }

    bool hasAnyFlag(TypeFlags flags) const {
        MOZ_ASSERT((flags & TYPE_FLAG_BASE_MASK) == flags);
        return !!(baseFlags() & flags);
    }

    bool nonDataProperty() const {
        return flags & TYPE_FLAG_NON_DATA_PROPERTY;
    }
    bool nonWritableProperty() const {
        return flags & TYPE_FLAG_NON_WRITABLE_PROPERTY;
    }
    bool nonConstantProperty() const {
        return flags & TYPE_FLAG_NON_CONSTANT_PROPERTY;
    }
    bool definiteProperty() const { return flags & TYPE_FLAG_DEFINITE_MASK; }
    unsigned definiteSlot() const {
        MOZ_ASSERT(definiteProperty());
        return (flags >> TYPE_FLAG_DEFINITE_SHIFT) - 1;
    }

    /* Join two type sets into a new set. The result should not be modified further. */
    static TemporaryTypeSet* unionSets(TypeSet* a, TypeSet* b, LifoAlloc* alloc);
    /* Return the intersection of the 2 TypeSets. The result should not be modified further */
    static TemporaryTypeSet* intersectSets(TemporaryTypeSet* a, TemporaryTypeSet* b, LifoAlloc* alloc);
    /*
     * Returns a copy of TypeSet a excluding/removing the types in TypeSet b.
     * TypeSet b can only contain primitives or be any object. No support for
     * specific objects. The result should not be modified further.
     */
    static TemporaryTypeSet* removeSet(TemporaryTypeSet* a, TemporaryTypeSet* b, LifoAlloc* alloc);

    /* Add a type to this set using the specified allocator. */
    void addType(Type type, LifoAlloc* alloc);

    /* Get a list of all types in this set. */
    typedef Vector<Type, 1, SystemAllocPolicy> TypeList;
    template <class TypeListT> bool enumerateTypes(TypeListT* list) const;

    /*
     * Iterate through the objects in this set. getObjectCount overapproximates
     * in the hash case (see SET_ARRAY_SIZE in TypeInference-inl.h), and
     * getObject may return nullptr.
     */
    inline unsigned getObjectCount() const;
    inline ObjectKey* getObject(unsigned i) const;
    inline JSObject* getSingleton(unsigned i) const;
    inline ObjectGroup* getGroup(unsigned i) const;
    inline JSObject* getSingletonNoBarrier(unsigned i) const;
    inline ObjectGroup* getGroupNoBarrier(unsigned i) const;

    /* The Class of an object in this set. */
    inline const Class* getObjectClass(unsigned i) const;

    bool canSetDefinite(unsigned slot) {
        // Note: the cast is required to work around an MSVC issue.
        return (slot + 1) <= (unsigned(TYPE_FLAG_DEFINITE_MASK) >> TYPE_FLAG_DEFINITE_SHIFT);
    }
    void setDefinite(unsigned slot) {
        MOZ_ASSERT(canSetDefinite(slot));
        flags |= ((slot + 1) << TYPE_FLAG_DEFINITE_SHIFT);
        MOZ_ASSERT(definiteSlot() == slot);
    }

    /* Whether any values in this set might have the specified type. */
    bool mightBeMIRType(jit::MIRType type) const;

    /*
     * Get whether this type set is known to be a subset of other.
     * This variant doesn't freeze constraints. That variant is called knownSubset
     */
    bool isSubset(const TypeSet* other) const;

    /*
     * Get whether the objects in this TypeSet are a subset of the objects
     * in other.
     */
    bool objectsAreSubset(TypeSet* other);

    /* Whether this TypeSet contains exactly the same types as other. */
    bool equals(const TypeSet* other) const {
        return this->isSubset(other) && other->isSubset(this);
    }

    bool objectsIntersect(const TypeSet* other) const;

    /* Forward all types in this set to the specified constraint. */
    bool addTypesToConstraint(JSContext* cx, TypeConstraint* constraint);

    // Clone a type set into an arbitrary allocator.
    TemporaryTypeSet* clone(LifoAlloc* alloc) const;
    bool clone(LifoAlloc* alloc, TemporaryTypeSet* result) const;

    // Create a new TemporaryTypeSet where undefined and/or null has been filtered out.
    TemporaryTypeSet* filter(LifoAlloc* alloc, bool filterUndefined, bool filterNull) const;
    // Create a new TemporaryTypeSet where the type has been set to object.
    TemporaryTypeSet* cloneObjectsOnly(LifoAlloc* alloc);
    TemporaryTypeSet* cloneWithoutObjects(LifoAlloc* alloc);

    // Trigger a read barrier on all the contents of a type set.
    static void readBarrier(const TypeSet* types);

  protected:
    uint32_t baseObjectCount() const {
        return (flags & TYPE_FLAG_OBJECT_COUNT_MASK) >> TYPE_FLAG_OBJECT_COUNT_SHIFT;
    }
    inline void setBaseObjectCount(uint32_t count);

    void clearObjects();

  public:
    static inline Type GetValueType(const Value& val);

    static inline bool IsUntrackedValue(const Value& val);

    // Get the type of a possibly optimized out or uninitialized let value.
    // This generally only happens on unconditional type monitors on bailing
    // out of Ion, such as for argument and local types.
    static inline Type GetMaybeUntrackedValueType(const Value& val);

    static void MarkTypeRoot(JSTracer* trc, Type* v, const char* name);
    static void MarkTypeUnbarriered(JSTracer* trc, Type* v, const char* name);
    static bool IsTypeMarked(Type* v);
    static bool IsTypeAllocatedDuringIncremental(Type v);
    static bool IsTypeAboutToBeFinalized(Type* v);
};

/*
 * A constraint which listens to additions to a type set and propagates those
 * changes to other type sets.
 */
class TypeConstraint
{
public:
    /* Next constraint listening to the same type set. */
    TypeConstraint* next;

    TypeConstraint()
        : next(nullptr)
    {}

    /* Debugging name for this kind of constraint. */
    virtual const char* kind() = 0;

    /* Register a new type for the set this constraint is listening to. */
    virtual void newType(JSContext* cx, TypeSet* source, TypeSet::Type type) = 0;

    /*
     * For constraints attached to an object property's type set, mark the
     * property as having changed somehow.
     */
    virtual void newPropertyState(JSContext* cx, TypeSet* source) {}

    /*
     * For constraints attached to the JSID_EMPTY type set on an object,
     * indicate a change in one of the object's dynamic property flags or other
     * state.
     */
    virtual void newObjectState(JSContext* cx, ObjectGroup* group) {}

    /*
     * If the data this constraint refers to is still live, copy it into the
     * zone's new allocator. Type constraints only hold weak references.
     */
    virtual bool sweep(TypeZone& zone, TypeConstraint** res) = 0;
};

// If there is an OOM while sweeping types, the type information is deoptimized
// so that it stays correct (i.e. overapproximates the possible types in the
// zone), but constraints might not have been triggered on the deoptimization
// or even copied over completely. In this case, destroy all JIT code and new
// script information in the zone, the only things whose correctness depends on
// the type constraints.
class AutoClearTypeInferenceStateOnOOM
{
    Zone* zone;
    bool oom;

  public:
    explicit AutoClearTypeInferenceStateOnOOM(Zone* zone)
      : zone(zone), oom(false)
    {}

    ~AutoClearTypeInferenceStateOnOOM();

    void setOOM() {
        oom = true;
    }
    bool hadOOM() const {
        return oom;
    }
};

/* Superclass common to stack and heap type sets. */
class ConstraintTypeSet : public TypeSet
{
  public:
    /* Chain of constraints which propagate changes out from this type set. */
    TypeConstraint* constraintList;

    ConstraintTypeSet() : constraintList(nullptr) {}

    /*
     * Add a type to this set, calling any constraint handlers if this is a new
     * possible type.
     */
    void addType(ExclusiveContext* cx, Type type);

    // Trigger a post barrier when writing to this set, if necessary.
    // addType(cx, type) takes care of this automatically.
    void postWriteBarrier(ExclusiveContext* cx, Type type);

    /* Add a new constraint to this set. */
    bool addConstraint(JSContext* cx, TypeConstraint* constraint, bool callExisting = true);

    inline void sweep(JS::Zone* zone, AutoClearTypeInferenceStateOnOOM& oom);
    inline void trace(JS::Zone* zone, JSTracer* trc);
};

class StackTypeSet : public ConstraintTypeSet
{
  public:
};

class HeapTypeSet : public ConstraintTypeSet
{
    inline void newPropertyState(ExclusiveContext* cx);

  public:
    /* Mark this type set as representing a non-data property. */
    inline void setNonDataProperty(ExclusiveContext* cx);

    /* Mark this type set as representing a non-writable property. */
    inline void setNonWritableProperty(ExclusiveContext* cx);

    // Mark this type set as being non-constant.
    inline void setNonConstantProperty(ExclusiveContext* cx);
};

CompilerConstraintList*
NewCompilerConstraintList(jit::TempAllocator& alloc);

class TemporaryTypeSet : public TypeSet
{
  public:
    TemporaryTypeSet() {}
    TemporaryTypeSet(LifoAlloc* alloc, Type type);

    TemporaryTypeSet(uint32_t flags, ObjectKey** objectSet) {
        this->flags = flags;
        this->objectSet = objectSet;
    }

    /*
     * Constraints for JIT compilation.
     *
     * Methods for JIT compilation. These must be used when a script is
     * currently being compiled (see AutoEnterCompilation) and will add
     * constraints ensuring that if the return value change in the future due
     * to new type information, the script's jitcode will be discarded.
     */

    /* Get any type tag which all values in this set must have. */
    jit::MIRType getKnownMIRType();

    bool isMagicArguments() { return getKnownMIRType() == jit::MIRType_MagicOptimizedArguments; }

    /* Whether this value may be an object. */
    bool maybeObject() { return unknownObject() || baseObjectCount() > 0; }

    /*
     * Whether this typeset represents a potentially sentineled object value:
     * the value may be an object or null or undefined.
     * Returns false if the value cannot ever be an object.
     */
    bool objectOrSentinel() {
        TypeFlags flags = TYPE_FLAG_UNDEFINED | TYPE_FLAG_NULL | TYPE_FLAG_ANYOBJECT;
        if (baseFlags() & (~flags & TYPE_FLAG_BASE_MASK))
            return false;

        return hasAnyFlag(TYPE_FLAG_ANYOBJECT) || baseObjectCount() > 0;
    }

    /* Whether the type set contains objects with any of a set of flags. */
    bool hasObjectFlags(CompilerConstraintList* constraints, ObjectGroupFlags flags);

    /* Get the class shared by all objects in this set, or nullptr. */
    const Class* getKnownClass(CompilerConstraintList* constraints);

    /* Result returned from forAllClasses */
    enum ForAllResult {
        EMPTY=1,                // Set empty
        ALL_TRUE,               // Set not empty and predicate returned true for all classes
        ALL_FALSE,              // Set not empty and predicate returned false for all classes
        MIXED,                  // Set not empty and predicate returned false for some classes
                                // and true for others, or set contains an unknown or non-object
                                // type
    };

    /* Apply func to the members of the set and return an appropriate result.
     * The iteration may end early if the result becomes known early.
     */
    ForAllResult forAllClasses(CompilerConstraintList* constraints,
                               bool (*func)(const Class* clasp));

    /*
     * Returns true if all objects in this set have the same prototype, and
     * assigns this object to *proto. The proto can be nullptr.
     */
    bool getCommonPrototype(CompilerConstraintList* constraints, JSObject** proto);

    /* Whether the buffer mapped by a TypedArray is shared memory or not */
    enum TypedArraySharedness {
        UnknownSharedness=1,    // We can't determine sharedness
        KnownShared,            // We know for sure the buffer is shared
        KnownUnshared           // We know for sure the buffer is unshared
    };

    /* Get the typed array type of all objects in this set, or Scalar::MaxTypedArrayViewType.
     * If there is such a common type and sharedness is not nullptr then
     * *sharedness is set to what we know about the sharedness of the memory.
     */
    Scalar::Type getTypedArrayType(CompilerConstraintList* constraints,
                                   TypedArraySharedness* sharedness = nullptr);

    /* Whether all objects have JSCLASS_IS_DOMJSCLASS set. */
    bool isDOMClass(CompilerConstraintList* constraints);

    /* Whether clasp->isCallable() is true for one or more objects in this set. */
    bool maybeCallable(CompilerConstraintList* constraints);

    /* Whether clasp->emulatesUndefined() is true for one or more objects in this set. */
    bool maybeEmulatesUndefined(CompilerConstraintList* constraints);

    /* Get the single value which can appear in this type set, otherwise nullptr. */
    JSObject* maybeSingleton();

    /* Whether any objects in the type set needs a barrier on id. */
    bool propertyNeedsBarrier(CompilerConstraintList* constraints, jsid id);

    /*
     * Whether this set contains all types in other, except (possibly) the
     * specified type.
     */
    bool filtersType(const TemporaryTypeSet* other, Type type) const;

    enum DoubleConversion {
        /* All types in the set should use eager double conversion. */
        AlwaysConvertToDoubles,

        /* Some types in the set should use eager double conversion. */
        MaybeConvertToDoubles,

        /* No types should use eager double conversion. */
        DontConvertToDoubles,

        /* Some types should use eager double conversion, others cannot. */
        AmbiguousDoubleConversion
    };

    /*
     * Whether known double optimizations are possible for element accesses on
     * objects in this type set.
     */
    DoubleConversion convertDoubleElements(CompilerConstraintList* constraints);

  private:
    void getTypedArraySharedness(CompilerConstraintList* constraints,
                                 TypedArraySharedness* sharedness);
};

bool
AddClearDefiniteGetterSetterForPrototypeChain(JSContext* cx, ObjectGroup* group, HandleId id);

bool
AddClearDefiniteFunctionUsesInScript(JSContext* cx, ObjectGroup* group,
                                     JSScript* script, JSScript* calleeScript);

// For groups where only a small number of objects have been allocated, this
// structure keeps track of all objects in the group. Once COUNT objects have
// been allocated, this structure is cleared and the objects are analyzed, to
// perform the new script properties analyses or determine if an unboxed
// representation can be used.
class PreliminaryObjectArray
{
  public:
    static const uint32_t COUNT = 20;

  private:
    // All objects with the type which have been allocated. The pointers in
    // this array are weak.
    JSObject* objects[COUNT];

  public:
    PreliminaryObjectArray() {
        mozilla::PodZero(this);
    }

    void registerNewObject(JSObject* res);
    void unregisterObject(JSObject* obj);

    JSObject* get(size_t i) const {
        MOZ_ASSERT(i < COUNT);
        return objects[i];
    }

    bool full() const;
    bool empty() const;
    void sweep();
};

class PreliminaryObjectArrayWithTemplate : public PreliminaryObjectArray
{
    RelocatablePtrShape shape_;

  public:
    explicit PreliminaryObjectArrayWithTemplate(Shape* shape)
      : shape_(shape)
    {}

    void clear() {
        shape_.init(nullptr);
    }

    Shape* shape() {
        return shape_;
    }

    void maybeAnalyze(ExclusiveContext* cx, ObjectGroup* group, bool force = false);

    void trace(JSTracer* trc);

    static void writeBarrierPre(PreliminaryObjectArrayWithTemplate* preliminaryObjects);
};

// New script properties analyses overview.
//
// When constructing objects using 'new' on a script, we attempt to determine
// the properties which that object will eventually have. This is done via two
// analyses. One of these, the definite properties analysis, is static, and the
// other, the acquired properties analysis, is dynamic. As objects are
// constructed using 'new' on some script to create objects of group G, our
// analysis strategy is as follows:
//
// - When the first objects are created, no analysis is immediately performed.
//   Instead, all objects of group G are accumulated in an array.
//
// - After a certain number of such objects have been created, the definite
//   properties analysis is performed. This analyzes the body of the
//   constructor script and any other functions it calls to look for properties
//   which will definitely be added by the constructor in a particular order,
//   creating an object with shape S.
//
// - The properties in S are compared with the greatest common prefix P of the
//   shapes of the objects that have been created. If P has more properties
//   than S, the acquired properties analysis is performed.
//
// - The acquired properties analysis marks all properties in P as definite
//   in G, and creates a new group IG for objects which are partially
//   initialized. Objects of group IG are initially created with shape S, and if
//   they are later given shape P, their group can be changed to G.
//
// For objects which are rarely created, the definite properties analysis can
// be triggered after only one or a few objects have been allocated, when code
// being Ion compiled might access them. In this case type information in the
// constructor might not be good enough for the definite properties analysis to
// compute useful information, but the acquired properties analysis will still
// be able to identify definite properties in this case.
//
// This layered approach is designed to maximize performance on easily
// analyzable code, while still allowing us to determine definite properties
// robustly when code consistently adds the same properties to objects, but in
// complex ways which can't be understood statically.
class TypeNewScript
{
  public:
    struct Initializer {
        enum Kind {
            SETPROP,
            SETPROP_FRAME,
            DONE
        } kind;
        uint32_t offset;
        Initializer(Kind kind, uint32_t offset)
          : kind(kind), offset(offset)
        {}
    };

  private:
    // Scripted function which this information was computed for.
    RelocatablePtrFunction function_;

    // Any preliminary objects with the type. The analyses are not performed
    // until this array is cleared.
    PreliminaryObjectArray* preliminaryObjects;

    // After the new script properties analyses have been performed, a template
    // object to use for newly constructed objects. The shape of this object
    // reflects all definite properties the object will have, and the
    // allocation kind to use. This is null if the new objects have an unboxed
    // layout, in which case the UnboxedLayout provides the initial structure
    // of the object.
    RelocatablePtrPlainObject templateObject_;

    // Order in which definite properties become initialized. We need this in
    // case the definite properties are invalidated (such as by adding a setter
    // to an object on the prototype chain) while an object is in the middle of
    // being initialized, so we can walk the stack and fixup any objects which
    // look for in-progress objects which were prematurely set with an incorrect
    // shape. Property assignments in inner frames are preceded by a series of
    // SETPROP_FRAME entries specifying the stack down to the frame containing
    // the write.
    Initializer* initializerList;

    // If there are additional properties found by the acquired properties
    // analysis which were not found by the definite properties analysis, this
    // shape contains all such additional properties (plus the definite
    // properties). When an object of this group acquires this shape, it is
    // fully initialized and its group can be changed to initializedGroup.
    RelocatablePtrShape initializedShape_;

    // Group with definite properties set for all properties found by
    // both the definite and acquired properties analyses.
    RelocatablePtrObjectGroup initializedGroup_;

  public:
    TypeNewScript() { mozilla::PodZero(this); }
    ~TypeNewScript() {
        js_delete(preliminaryObjects);
        js_free(initializerList);
    }

    void clear() {
        function_.init(nullptr);
        templateObject_.init(nullptr);
        initializedShape_.init(nullptr);
        initializedGroup_.init(nullptr);
    }

    static void writeBarrierPre(TypeNewScript* newScript);

    bool analyzed() const {
        return preliminaryObjects == nullptr;
    }

    PlainObject* templateObject() const {
        return templateObject_;
    }

    Shape* initializedShape() const {
        return initializedShape_;
    }

    ObjectGroup* initializedGroup() const {
        return initializedGroup_;
    }

    JSFunction* function() const {
        return function_;
    }

    void trace(JSTracer* trc);
    void sweep();

    void registerNewObject(PlainObject* res);
    bool maybeAnalyze(JSContext* cx, ObjectGroup* group, bool* regenerate, bool force = false);

    bool rollbackPartiallyInitializedObjects(JSContext* cx, ObjectGroup* group);

    static bool make(JSContext* cx, ObjectGroup* group, JSFunction* fun);
    static TypeNewScript* makeNativeVersion(JSContext* cx, TypeNewScript* newScript,
                                            PlainObject* templateObject);

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

/* Is this a reasonable PC to be doing inlining on? */
inline bool isInlinableCall(jsbytecode* pc);

bool
ClassCanHaveExtraProperties(const Class* clasp);

/* Persistent type information for a script, retained across GCs. */
class TypeScript
{
    friend class ::JSScript;

    // Variable-size array
    StackTypeSet typeArray_[1];

  public:
    /* Array of type sets for variables and JOF_TYPESET ops. */
    StackTypeSet* typeArray() const {
        // Ensure typeArray_ is the last data member of TypeScript.
        JS_STATIC_ASSERT(sizeof(TypeScript) ==
                         sizeof(typeArray_) + offsetof(TypeScript, typeArray_));
        return const_cast<StackTypeSet*>(typeArray_);
    }

    static inline size_t SizeIncludingTypeArray(size_t arraySize) {
        // Ensure typeArray_ is the last data member of TypeScript.
        JS_STATIC_ASSERT(sizeof(TypeScript) ==
            sizeof(StackTypeSet) + offsetof(TypeScript, typeArray_));
        return offsetof(TypeScript, typeArray_) + arraySize * sizeof(StackTypeSet);
    }

    static inline unsigned NumTypeSets(JSScript* script);

    static inline StackTypeSet* ThisTypes(JSScript* script);
    static inline StackTypeSet* ArgTypes(JSScript* script, unsigned i);

    /* Get the type set for values observed at an opcode. */
    static inline StackTypeSet* BytecodeTypes(JSScript* script, jsbytecode* pc);

    template <typename TYPESET>
    static inline TYPESET* BytecodeTypes(JSScript* script, jsbytecode* pc, uint32_t* bytecodeMap,
                                         uint32_t* hint, TYPESET* typeArray);

    /*
     * Monitor a bytecode pushing any value. This must be called for any opcode
     * which is JOF_TYPESET, and where either the script has not been analyzed
     * by type inference or where the pc has type barriers. For simplicity, we
     * always monitor JOF_TYPESET opcodes in the interpreter and stub calls,
     * and only look at barriers when generating JIT code for the script.
     */
    static inline void Monitor(JSContext* cx, JSScript* script, jsbytecode* pc,
                               const js::Value& val);
    static inline void Monitor(JSContext* cx, JSScript* script, jsbytecode* pc,
                               TypeSet::Type type);
    static inline void Monitor(JSContext* cx, const js::Value& rval);

    /* Monitor an assignment at a SETELEM on a non-integer identifier. */
    static inline void MonitorAssign(JSContext* cx, HandleObject obj, jsid id);

    /* Add a type for a variable in a script. */
    static inline void SetThis(JSContext* cx, JSScript* script, TypeSet::Type type);
    static inline void SetThis(JSContext* cx, JSScript* script, const js::Value& value);
    static inline void SetArgument(JSContext* cx, JSScript* script, unsigned arg,
                                   TypeSet::Type type);
    static inline void SetArgument(JSContext* cx, JSScript* script, unsigned arg,
                                   const js::Value& value);

    /*
     * Freeze all the stack type sets in a script, for a compilation. Returns
     * copies of the type sets which will be checked against the actual ones
     * under FinishCompilation, to detect any type changes.
     */
    static bool FreezeTypeSets(CompilerConstraintList* constraints, JSScript* script,
                               TemporaryTypeSet** pThisTypes,
                               TemporaryTypeSet** pArgTypes,
                               TemporaryTypeSet** pBytecodeTypes);

    static void Purge(JSContext* cx, HandleScript script);

    void destroy();

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this);
    }

#ifdef DEBUG
    void printTypes(JSContext* cx, HandleScript script) const;
#endif
};

void
FillBytecodeTypeMap(JSScript* script, uint32_t* bytecodeMap);

class RecompileInfo;

// Allocate a CompilerOutput for a finished compilation and generate the type
// constraints for the compilation. Sets |isValidOut| based on whether the type
// constraints still hold.
bool
FinishCompilation(JSContext* cx, HandleScript script, CompilerConstraintList* constraints,
                  RecompileInfo* precompileInfo, bool* isValidOut);

// Reset any CompilerOutput present for a script.
void
InvalidateCompilerOutputsForScript(JSContext* cx, HandleScript script);

// Update the actual types in any scripts queried by constraints with any
// speculative types added during the definite properties analysis.
void
FinishDefinitePropertiesAnalysis(JSContext* cx, CompilerConstraintList* constraints);

// Representation of a heap type property which may or may not be instantiated.
// Heap properties for singleton types are instantiated lazily as they are used
// by the compiler, but this is only done on the main thread. If we are
// compiling off thread and use a property which has not yet been instantiated,
// it will be treated as empty and non-configured and will be instantiated when
// rejoining to the main thread. If it is in fact not empty, the compilation
// will fail; to avoid this, we try to instantiate singleton property types
// during generation of baseline caches.
class HeapTypeSetKey
{
    friend class TypeSet::ObjectKey;

    // Object and property being accessed.
    TypeSet::ObjectKey* object_;
    jsid id_;

    // If instantiated, the underlying heap type set.
    HeapTypeSet* maybeTypes_;

  public:
    HeapTypeSetKey()
      : object_(nullptr), id_(JSID_EMPTY), maybeTypes_(nullptr)
    {}

    TypeSet::ObjectKey* object() const { return object_; }
    jsid id() const { return id_; }
    HeapTypeSet* maybeTypes() const { return maybeTypes_; }

    bool instantiate(JSContext* cx);

    void freeze(CompilerConstraintList* constraints);
    jit::MIRType knownMIRType(CompilerConstraintList* constraints);
    bool nonData(CompilerConstraintList* constraints);
    bool nonWritable(CompilerConstraintList* constraints);
    bool isOwnProperty(CompilerConstraintList* constraints, bool allowEmptyTypesForGlobal = false);
    bool knownSubset(CompilerConstraintList* constraints, const HeapTypeSetKey& other);
    JSObject* singleton(CompilerConstraintList* constraints);
    bool needsBarrier(CompilerConstraintList* constraints);
    bool constant(CompilerConstraintList* constraints, Value* valOut);
    bool couldBeConstant(CompilerConstraintList* constraints);
};

/*
 * Information about the result of the compilation of a script.  This structure
 * stored in the TypeCompartment is indexed by the RecompileInfo. This
 * indirection enables the invalidation of all constraints related to the same
 * compilation.
 */
class CompilerOutput
{
    // If this compilation has not been invalidated, the associated script and
    // kind of compilation being performed.
    JSScript* script_;

    // Whether this compilation is about to be invalidated.
    bool pendingInvalidation_ : 1;

    // During sweeping, the list of compiler outputs is compacted and invalidated
    // outputs are removed. This gives the new index for a valid compiler output.
    uint32_t sweepIndex_ : 31;

  public:
    static const uint32_t INVALID_SWEEP_INDEX = static_cast<uint32_t>(1 << 31) - 1;

    CompilerOutput()
      : script_(nullptr),
        pendingInvalidation_(false), sweepIndex_(INVALID_SWEEP_INDEX)
    {}

    explicit CompilerOutput(JSScript* script)
      : script_(script),
        pendingInvalidation_(false), sweepIndex_(INVALID_SWEEP_INDEX)
    {}

    JSScript* script() const { return script_; }

    inline jit::IonScript* ion() const;

    bool isValid() const {
        return script_ != nullptr;
    }
    void invalidate() {
        script_ = nullptr;
    }

    void setPendingInvalidation() {
        pendingInvalidation_ = true;
    }
    bool pendingInvalidation() {
        return pendingInvalidation_;
    }

    void setSweepIndex(uint32_t index) {
        if (index >= INVALID_SWEEP_INDEX)
            MOZ_CRASH();
        sweepIndex_ = index;
    }
    uint32_t sweepIndex() {
        MOZ_ASSERT(sweepIndex_ != INVALID_SWEEP_INDEX);
        return sweepIndex_;
    }
};

class RecompileInfo
{
    // Index in the TypeZone's compilerOutputs or sweepCompilerOutputs arrays,
    // depending on the generation value.
    uint32_t outputIndex : 31;

    // If out of sync with the TypeZone's generation, this index is for the
    // zone's sweepCompilerOutputs rather than compilerOutputs.
    uint32_t generation : 1;

  public:
    RecompileInfo(uint32_t outputIndex, uint32_t generation)
      : outputIndex(outputIndex), generation(generation)
    {}

    RecompileInfo()
      : outputIndex(JS_BITMASK(31)), generation(0)
    {}

    CompilerOutput* compilerOutput(TypeZone& types) const;
    CompilerOutput* compilerOutput(JSContext* cx) const;
    bool shouldSweep(TypeZone& types);
};

typedef Vector<RecompileInfo, 0, SystemAllocPolicy> RecompileInfoVector;

struct AutoEnterAnalysis;

struct TypeZone
{
    JS::Zone* zone_;

    /* Pool for type information in this zone. */
    static const size_t TYPE_LIFO_ALLOC_PRIMARY_CHUNK_SIZE = 8 * 1024;
    LifoAlloc typeLifoAlloc;

    // Current generation for sweeping.
    uint32_t generation : 1;

    /*
     * All Ion compilations that have occured in this zone, for indexing via
     * RecompileInfo. This includes both valid and invalid compilations, though
     * invalidated compilations are swept on GC.
     */
    typedef Vector<CompilerOutput, 4, SystemAllocPolicy> CompilerOutputVector;
    CompilerOutputVector* compilerOutputs;

    // During incremental sweeping, allocator holding the old type information
    // for the zone.
    LifoAlloc sweepTypeLifoAlloc;

    // During incremental sweeping, the old compiler outputs for use by
    // recompile indexes with a stale generation.
    CompilerOutputVector* sweepCompilerOutputs;

    // During incremental sweeping, whether to try to destroy all type
    // information attached to scripts.
    bool sweepReleaseTypes;

    // The topmost AutoEnterAnalysis on the stack, if there is one.
    AutoEnterAnalysis* activeAnalysis;

    explicit TypeZone(JS::Zone* zone);
    ~TypeZone();

    JS::Zone* zone() const { return zone_; }

    void beginSweep(FreeOp* fop, bool releaseTypes, AutoClearTypeInferenceStateOnOOM& oom);
    void endSweep(JSRuntime* rt);
    void clearAllNewScriptsOnOOM();

    /* Mark a script as needing recompilation once inference has finished. */
    void addPendingRecompile(JSContext* cx, const RecompileInfo& info);
    void addPendingRecompile(JSContext* cx, JSScript* script);

    void processPendingRecompiles(FreeOp* fop, RecompileInfoVector& recompiles);
};

enum SpewChannel {
    ISpewOps,      /* ops: New constraints and types. */
    ISpewResult,   /* result: Final type sets. */
    SPEW_COUNT
};

#ifdef DEBUG

const char * InferSpewColorReset();
const char * InferSpewColor(TypeConstraint* constraint);
const char * InferSpewColor(TypeSet* types);

void InferSpew(SpewChannel which, const char* fmt, ...);

/* Check that the type property for id in group contains value. */
bool ObjectGroupHasProperty(JSContext* cx, ObjectGroup* group, jsid id, const Value& value);

#else

inline const char * InferSpewColorReset() { return nullptr; }
inline const char * InferSpewColor(TypeConstraint* constraint) { return nullptr; }
inline const char * InferSpewColor(TypeSet* types) { return nullptr; }
inline void InferSpew(SpewChannel which, const char* fmt, ...) {}

#endif

/* Print a warning, dump state and abort the program. */
MOZ_NORETURN MOZ_COLD void TypeFailure(JSContext* cx, const char* fmt, ...);

// Prints type information for a context if spew is enabled or force is set.
void
PrintTypes(JSContext* cx, JSCompartment* comp, bool force);

} /* namespace js */

// JS::ubi::Nodes can point to object groups; they're js::gc::Cell instances
// with no associated compartment.
namespace JS {
namespace ubi {

template<>
struct Concrete<js::ObjectGroup> : TracerConcrete<js::ObjectGroup> {
    Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  protected:
    explicit Concrete(js::ObjectGroup *ptr) : TracerConcrete<js::ObjectGroup>(ptr) { }

  public:
    static void construct(void *storage, js::ObjectGroup *ptr) { new (storage) Concrete(ptr); }
};

} // namespace ubi
} // namespace JS

#endif /* vm_TypeInference_h */
