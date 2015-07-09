/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Shape_h
#define vm_Shape_h

#include "mozilla/Attributes.h"
#include "mozilla/GuardObjects.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/TemplateLib.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jspropertytree.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "gc/Barrier.h"
#include "gc/Heap.h"
#include "gc/Marking.h"
#include "gc/Rooting.h"
#include "js/HashTable.h"
#include "js/MemoryMetrics.h"
#include "js/RootingAPI.h"
#include "js/UbiNode.h"
#include "vm/ObjectGroup.h"
#include "vm/PropDesc.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4800)
#pragma warning(push)
#pragma warning(disable:4100) /* Silence unreferenced formal parameter warnings */
#endif

/*
 * In isolation, a Shape represents a property that exists in one or more
 * objects; it has an id, flags, etc. (But it doesn't represent the property's
 * value.)  However, Shapes are always stored in linked linear sequence of
 * Shapes, called "shape lineages". Each shape lineage represents the layout of
 * an entire object.
 *
 * Every JSObject has a pointer, |shape_|, accessible via lastProperty(), to
 * the last Shape in a shape lineage, which identifies the property most
 * recently added to the object.  This pointer permits fast object layout
 * tests. The shape lineage order also dictates the enumeration order for the
 * object; ECMA requires no particular order but this implementation has
 * promised and delivered property definition order.
 *
 * Shape lineages occur in two kinds of data structure.
 *
 * 1. N-ary property trees. Each path from a non-root node to the root node in
 *    a property tree is a shape lineage. Property trees permit full (or
 *    partial) sharing of Shapes between objects that have fully (or partly)
 *    identical layouts. The root is an EmptyShape whose identity is determined
 *    by the object's class, compartment and prototype. These Shapes are shared
 *    and immutable.
 *
 * 2. Dictionary mode lists. Shapes in such lists are said to be "in
 *    dictionary mode", as are objects that point to such Shapes. These Shapes
 *    are unshared, private to a single object, and immutable except for their
 *    links in the dictionary list.
 *
 * All shape lineages are bi-directionally linked, via the |parent| and
 * |kids|/|listp| members.
 *
 * Shape lineages start out life in the property tree. They can be converted
 * (by copying) to dictionary mode lists in the following circumstances.
 *
 * 1. The shape lineage's size reaches MAX_HEIGHT. This reasonable limit avoids
 *    potential worst cases involving shape lineage mutations.
 *
 * 2. A property represented by a non-last Shape in a shape lineage is removed
 *    from an object. (In the last Shape case, obj->shape_ can be easily
 *    adjusted to point to obj->shape_->parent.)  We originally tried lazy
 *    forking of the property tree, but this blows up for delete/add
 *    repetitions.
 *
 * 3. A property represented by a non-last Shape in a shape lineage has its
 *    attributes modified.
 *
 * To find the Shape for a particular property of an object initially requires
 * a linear search. But if the number of searches starting at any particular
 * Shape in the property tree exceeds LINEAR_SEARCHES_MAX and the Shape's
 * lineage has (excluding the EmptyShape) at least MIN_ENTRIES, we create an
 * auxiliary hash table -- the ShapeTable -- that allows faster lookup.
 * Furthermore, a ShapeTable is always created for dictionary mode lists,
 * and it is attached to the last Shape in the lineage. Shape tables for
 * property tree Shapes never change, but shape tables for dictionary mode
 * Shapes can grow and shrink.
 *
 * There used to be a long, math-heavy comment here explaining why property
 * trees are more space-efficient than alternatives.  This was removed in bug
 * 631138; see that bug for the full details.
 *
 * For getters/setters, an AccessorShape is allocated. This is a slightly fatter
 * type with extra fields for the getter/setter data.
 *
 * Because many Shapes have similar data, there is actually a secondary type
 * called a BaseShape that holds some of a Shape's data.  Many shapes can share
 * a single BaseShape.
 */

#define JSSLOT_FREE(clasp)  JSCLASS_RESERVED_SLOTS(clasp)

namespace js {

class Bindings;
class Debugger;
class Nursery;
class StaticBlockObject;

typedef JSPropertyOp         PropertyOp;
typedef JSStrictPropertyOp   StrictPropertyOp;
typedef JSPropertyDescriptor PropertyDescriptor;

/* Limit on the number of slotful properties in an object. */
static const uint32_t SHAPE_INVALID_SLOT = JS_BIT(24) - 1;
static const uint32_t SHAPE_MAXIMUM_SLOT = JS_BIT(24) - 2;

/*
 * Shapes use multiplicative hashing, but specialized to
 * minimize footprint.
 */
class ShapeTable {
  public:
    friend class NativeObject;
    static const uint32_t MIN_ENTRIES   = 11;

    class Entry {
        // js::Shape pointer tag bit indicating a collision.
        static const uintptr_t SHAPE_COLLISION = 1;
        static Shape* const SHAPE_REMOVED; // = SHAPE_COLLISION

        Shape* shape_;

        Entry() = delete;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;

      public:
        bool isFree() const { return shape_ == nullptr; }
        bool isRemoved() const { return shape_ == SHAPE_REMOVED; }
        bool hadCollision() const { return uintptr_t(shape_) & SHAPE_COLLISION; }

        void setFree() { shape_ = nullptr; }
        void setRemoved() { shape_ = SHAPE_REMOVED; }

        Shape* shape() const {
            return reinterpret_cast<Shape*>(uintptr_t(shape_) & ~SHAPE_COLLISION);
        }

        void setShape(Shape* shape) {
            MOZ_ASSERT(isFree());
            MOZ_ASSERT(shape);
            MOZ_ASSERT(shape != SHAPE_REMOVED);
            shape_ = shape;
            MOZ_ASSERT(!hadCollision());
        }

        void flagCollision() {
            shape_ = reinterpret_cast<Shape*>(uintptr_t(shape_) | SHAPE_COLLISION);
        }
        void setPreservingCollision(Shape* shape) {
            shape_ = reinterpret_cast<Shape*>(uintptr_t(shape) | uintptr_t(hadCollision()));
        }
    };

  private:
    static const uint32_t HASH_BITS     = mozilla::tl::BitSize<HashNumber>::value;

    // This value is low because it's common for a ShapeTable to be created
    // with an entryCount of zero.
    static const uint32_t MIN_SIZE_LOG2 = 2;
    static const uint32_t MIN_SIZE      = JS_BIT(MIN_SIZE_LOG2);

    uint32_t        hashShift_;         /* multiplicative hash shift */

    uint32_t        entryCount_;        /* number of entries in table */
    uint32_t        removedCount_;      /* removed entry sentinels in table */

    uint32_t        freeList_;          /* SHAPE_INVALID_SLOT or head of slot
                                           freelist in owning dictionary-mode
                                           object */

    Entry*          entries_;          /* table of ptrs to shared tree nodes */

  public:
    explicit ShapeTable(uint32_t nentries)
      : hashShift_(HASH_BITS - MIN_SIZE_LOG2),
        entryCount_(nentries),
        removedCount_(0),
        freeList_(SHAPE_INVALID_SLOT),
        entries_(nullptr)
    {
        /* NB: entries is set by init, which must be called. */
    }

    ~ShapeTable() {
        js_free(entries_);
    }

    uint32_t entryCount() const { return entryCount_; }

    uint32_t freeList() const { return freeList_; }
    void setFreeList(uint32_t slot) { freeList_ = slot; }

    /*
     * This counts the ShapeTable object itself (which must be
     * heap-allocated) and its |entries| array.
     */
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) + mallocSizeOf(entries_);
    }

    /*
     * NB: init and change are fallible but do not report OOM, so callers can
     * cope or ignore. They do however use the context's calloc method in
     * order to update the malloc counter on success.
     */
    bool init(ExclusiveContext* cx, Shape* lastProp);
    bool change(int log2Delta, ExclusiveContext* cx);
    Entry& search(jsid id, bool adding);

  private:
    Entry& getEntry(uint32_t i) const {
        MOZ_ASSERT(i < capacity());
        return entries_[i];
    }
    void decEntryCount() {
        MOZ_ASSERT(entryCount_ > 0);
        entryCount_--;
    }
    void incEntryCount() {
        entryCount_++;
        MOZ_ASSERT(entryCount_ + removedCount_ <= capacity());
    }
    void incRemovedCount() {
        removedCount_++;
        MOZ_ASSERT(entryCount_ + removedCount_ <= capacity());
    }

    /* By definition, hashShift = HASH_BITS - log2(capacity). */
    uint32_t capacity() const { return JS_BIT(HASH_BITS - hashShift_); }

    /* Whether we need to grow.  We want to do this if the load factor is >= 0.75 */
    bool needsToGrow() const {
        uint32_t size = capacity();
        return entryCount_ + removedCount_ >= size - (size >> 2);
    }

    /*
     * Try to grow the table.  On failure, reports out of memory on cx
     * and returns false.  This will make any extant pointers into the
     * table invalid.  Don't call this unless needsToGrow() is true.
     */
    bool grow(ExclusiveContext* cx);
};

/*
 * Reuse the API-only JSPROP_INDEX attribute to mean shadowability.
 */
#define JSPROP_SHADOWABLE       JSPROP_INDEX

/*
 * Shapes encode information about both a property lineage *and* a particular
 * property. This information is split across the Shape and the BaseShape
 * at shape->base(). Both Shape and BaseShape can be either owned or unowned
 * by, respectively, the Object or Shape referring to them.
 *
 * Owned Shapes are used in dictionary objects, and form a doubly linked list
 * whose entries are all owned by that dictionary. Unowned Shapes are all in
 * the property tree.
 *
 * Owned BaseShapes are used for shapes which have shape tables, including
 * the last properties in all dictionaries. Unowned BaseShapes compactly store
 * information common to many shapes. In a given compartment there is a single
 * BaseShape for each combination of BaseShape information. This information
 * is cloned in owned BaseShapes so that information can be quickly looked up
 * for a given object or shape without regard to whether the base shape is
 * owned or not.
 *
 * All combinations of owned/unowned Shapes/BaseShapes are possible:
 *
 * Owned Shape, Owned BaseShape:
 *
 *     Last property in a dictionary object. The BaseShape is transferred from
 *     property to property as the object's last property changes.
 *
 * Owned Shape, Unowned BaseShape:
 *
 *     Property in a dictionary object other than the last one.
 *
 * Unowned Shape, Owned BaseShape:
 *
 *     Property in the property tree which has a shape table.
 *
 * Unowned Shape, Unowned BaseShape:
 *
 *     Property in the property tree which does not have a shape table.
 *
 * BaseShapes additionally encode some information about the referring object
 * itself. This includes the object's class, parent and various flags that may
 * be set for the object. Except for the class, this information is mutable and
 * may change when the object has an established property lineage. On such
 * changes the entire property lineage is not updated, but rather only the
 * last property (and its base shape). This works because only the object's
 * last property is used to query information about the object. Care must be
 * taken to call JSObject::canRemoveLastProperty when unwinding an object to
 * an earlier property, however.
 */

class AccessorShape;
class Shape;
class UnownedBaseShape;
struct StackBaseShape;

namespace gc {
void MergeCompartments(JSCompartment* source, JSCompartment* target);
}

// This class is used to add a post barrier on the AccessorShape's getter/setter
// objects. It updates the shape's entry in the parent's KidsHash table.
class ShapeGetterSetterRef : public gc::BufferableRef
{
    AccessorShape* shape;
    JSObject** objp;

  public:
    ShapeGetterSetterRef(AccessorShape* shape, JSObject** objp)
      : shape(shape), objp(objp)
    {}

    void mark(JSTracer* trc);
};

static inline void
GetterSetterWriteBarrierPost(AccessorShape* shape, JSObject** objp)
{
    MOZ_ASSERT(shape);
    MOZ_ASSERT(objp);
    MOZ_ASSERT(*objp);
    gc::Cell** cellp = reinterpret_cast<gc::Cell**>(objp);
    if (gc::StoreBuffer* sb = (*cellp)->storeBuffer())
        sb->putGeneric(ShapeGetterSetterRef(shape, objp));
}

static inline void
GetterSetterWriteBarrierPostRemove(JSRuntime* rt, JSObject** objp)
{
    JS::shadow::Runtime* shadowRuntime = JS::shadow::Runtime::asShadowRuntime(rt);
    shadowRuntime->gcStoreBufferPtr()->removeRelocatableCellFromAnyThread(reinterpret_cast<gc::Cell**>(objp));
}

class BaseShape : public gc::TenuredCell
{
  public:
    friend class Shape;
    friend struct StackBaseShape;
    friend struct StackShape;
    friend void gc::MergeCompartments(JSCompartment* source, JSCompartment* target);

    enum Flag {
        /* Owned by the referring shape. */
        OWNED_SHAPE        = 0x1,

        /* (0x2 and 0x4 are unused) */

        /*
         * Flags set which describe the referring object. Once set these cannot
         * be unset (except during object densification of sparse indexes), and
         * are transferred from shape to shape as the object's last property
         * changes.
         *
         * If you add a new flag here, please add appropriate code to
         * JSObject::dump to dump it as part of object representation.
         */

        DELEGATE            =    0x8,
        NOT_EXTENSIBLE      =   0x10,
        INDEXED             =   0x20,
        BOUND_FUNCTION      =   0x40,
        HAD_ELEMENTS_ACCESS =   0x80,
        WATCHED             =  0x100,
        ITERATED_SINGLETON  =  0x200,
        NEW_GROUP_UNKNOWN   =  0x400,
        UNCACHEABLE_PROTO   =  0x800,
        IMMUTABLE_PROTOTYPE = 0x1000,

        // These two flags control which scope a new variables ends up on in the
        // scope chain. If the variable is "qualified" (i.e., if it was defined
        // using var, let, or const) then it ends up on the lowest scope in the
        // chain that has the QUALIFIED_VAROBJ flag set. If it's "unqualified"
        // (i.e., if it was introduced without any var, let, or const, which
        // incidentally is an error in strict mode) then it goes on the lowest
        // scope in the chain with the UNQUALIFIED_VAROBJ flag set (which is
        // typically the global).
        QUALIFIED_VAROBJ    = 0x2000,
        UNQUALIFIED_VAROBJ  = 0x4000,

        // For a function used as an interpreted constructor, whether a 'new'
        // type had constructor information cleared.
        NEW_SCRIPT_CLEARED  = 0x8000,

        OBJECT_FLAG_MASK    = 0xfff8
    };

  private:
    const Class*        clasp_;        /* Class of referring object. */
    HeapPtrObject       parent;         /* Parent of referring object. */
    HeapPtrObject       metadata;       /* Optional holder of metadata about
                                         * the referring object. */
    JSCompartment*      compartment_;  /* Compartment shape belongs to. */
    uint32_t            flags;          /* Vector of above flags. */
    uint32_t            slotSpan_;      /* Object slot span for BaseShapes at
                                         * dictionary last properties. */

    /* For owned BaseShapes, the canonical unowned BaseShape. */
    HeapPtrUnownedBaseShape unowned_;

    /* For owned BaseShapes, the shape's shape table. */
    ShapeTable*      table_;

    BaseShape(const BaseShape& base) = delete;

  public:
    void finalize(FreeOp* fop);

    BaseShape(JSCompartment* comp, const Class* clasp, JSObject* parent, JSObject* metadata,
              uint32_t objectFlags)
    {
        MOZ_ASSERT(!(objectFlags & ~OBJECT_FLAG_MASK));
        mozilla::PodZero(this);
        this->clasp_ = clasp;
        this->parent = parent;
        this->metadata = metadata;
        this->flags = objectFlags;
        this->compartment_ = comp;
    }

    BaseShape(JSCompartment* comp, const Class* clasp, JSObject* parent, JSObject* metadata,
              uint32_t objectFlags, uint8_t attrs)
    {
        MOZ_ASSERT(!(objectFlags & ~OBJECT_FLAG_MASK));
        mozilla::PodZero(this);
        this->clasp_ = clasp;
        this->parent = parent;
        this->metadata = metadata;
        this->flags = objectFlags;
        this->compartment_ = comp;
    }

    explicit inline BaseShape(const StackBaseShape& base);

    /* Not defined: BaseShapes must not be stack allocated. */
    ~BaseShape();

    BaseShape& operator=(const BaseShape& other) {
        clasp_ = other.clasp_;
        parent = other.parent;
        metadata = other.metadata;
        flags = other.flags;
        slotSpan_ = other.slotSpan_;
        compartment_ = other.compartment_;
        return *this;
    }

    const Class* clasp() const { return clasp_; }

    bool isOwned() const { return !!(flags & OWNED_SHAPE); }

    inline void adoptUnowned(UnownedBaseShape* other);

    void setOwned(UnownedBaseShape* unowned) {
        flags |= OWNED_SHAPE;
        this->unowned_ = unowned;
    }

    JSObject* getObjectParent() const { return parent; }
    JSObject* getObjectMetadata() const { return metadata; }
    uint32_t getObjectFlags() const { return flags & OBJECT_FLAG_MASK; }

    bool hasTable() const { MOZ_ASSERT_IF(table_, isOwned()); return table_ != nullptr; }
    ShapeTable& table() const { MOZ_ASSERT(table_ && isOwned()); return *table_; }
    void setTable(ShapeTable* table) { MOZ_ASSERT(isOwned()); table_ = table; }

    uint32_t slotSpan() const { MOZ_ASSERT(isOwned()); return slotSpan_; }
    void setSlotSpan(uint32_t slotSpan) { MOZ_ASSERT(isOwned()); slotSpan_ = slotSpan; }

    JSCompartment* compartment() const { return compartment_; }

    /*
     * Lookup base shapes from the compartment's baseShapes table, adding if
     * not already found.
     */
    static UnownedBaseShape* getUnowned(ExclusiveContext* cx, StackBaseShape& base);

    /* Get the canonical base shape. */
    inline UnownedBaseShape* unowned();

    /* Get the canonical base shape for an owned one. */
    inline UnownedBaseShape* baseUnowned();

    /* Get the canonical base shape for an unowned one (i.e. identity). */
    inline UnownedBaseShape* toUnowned();

    /* Check that an owned base shape is consistent with its unowned base. */
    void assertConsistency();

    /* For JIT usage */
    static inline size_t offsetOfParent() { return offsetof(BaseShape, parent); }
    static inline size_t offsetOfFlags() { return offsetof(BaseShape, flags); }

    static inline ThingRootKind rootKind() { return THING_ROOT_BASE_SHAPE; }

    void markChildren(JSTracer* trc) {
        if (isOwned())
            gc::MarkBaseShape(trc, &unowned_, "base");

        if (parent)
            gc::MarkObject(trc, &parent, "parent");

        if (metadata)
            gc::MarkObject(trc, &metadata, "metadata");
    }

    void fixupAfterMovingGC() {}
    bool fixupBaseShapeTableEntry();

  private:
    static void staticAsserts() {
        JS_STATIC_ASSERT(offsetof(BaseShape, clasp_) == offsetof(js::shadow::BaseShape, clasp_));
    }
};

class UnownedBaseShape : public BaseShape {};

inline void
BaseShape::adoptUnowned(UnownedBaseShape* other)
{
    // This is a base shape owned by a dictionary object, update it to reflect the
    // unowned base shape of a new last property.
    MOZ_ASSERT(isOwned());

    uint32_t span = slotSpan();
    ShapeTable* table = &this->table();

    *this = *other;
    setOwned(other);
    setTable(table);
    setSlotSpan(span);

    assertConsistency();
}

UnownedBaseShape*
BaseShape::unowned()
{
    return isOwned() ? baseUnowned() : toUnowned();
}

UnownedBaseShape*
BaseShape::toUnowned()
{
    MOZ_ASSERT(!isOwned() && !unowned_); return static_cast<UnownedBaseShape*>(this);
}

UnownedBaseShape*
BaseShape::baseUnowned()
{
    MOZ_ASSERT(isOwned() && unowned_); return unowned_;
}

/* Entries for the per-compartment baseShapes set of unowned base shapes. */
struct StackBaseShape : public DefaultHasher<ReadBarrieredUnownedBaseShape>
{
    uint32_t flags;
    const Class* clasp;
    JSObject* parent;
    JSObject* metadata;
    JSCompartment* compartment;

    explicit StackBaseShape(BaseShape* base)
      : flags(base->flags & BaseShape::OBJECT_FLAG_MASK),
        clasp(base->clasp_),
        parent(base->parent),
        metadata(base->metadata),
        compartment(base->compartment())
    {}

    inline StackBaseShape(ExclusiveContext* cx, const Class* clasp,
                          JSObject* parent, JSObject* metadata, uint32_t objectFlags);
    explicit inline StackBaseShape(Shape* shape);

    struct Lookup
    {
        uint32_t flags;
        const Class* clasp;
        JSObject* hashParent;
        JSObject* matchParent;
        JSObject* hashMetadata;
        JSObject* matchMetadata;

        MOZ_IMPLICIT Lookup(const StackBaseShape& base)
          : flags(base.flags),
            clasp(base.clasp),
            hashParent(base.parent),
            matchParent(base.parent),
            hashMetadata(base.metadata),
            matchMetadata(base.metadata)
        {}

        MOZ_IMPLICIT Lookup(UnownedBaseShape* base)
          : flags(base->getObjectFlags()),
            clasp(base->clasp()),
            hashParent(base->getObjectParent()),
            matchParent(base->getObjectParent()),
            hashMetadata(base->getObjectMetadata()),
            matchMetadata(base->getObjectMetadata())
        {
            MOZ_ASSERT(!base->isOwned());
        }

        // For use by generational GC post barriers.
        Lookup(uint32_t flags, const Class* clasp,
               JSObject* hashParent, JSObject* matchParent,
               JSObject* hashMetadata, JSObject* matchMetadata)
          : flags(flags),
            clasp(clasp),
            hashParent(hashParent),
            matchParent(matchParent),
            hashMetadata(hashMetadata),
            matchMetadata(matchMetadata)
        {}
    };

    static inline HashNumber hash(const Lookup& lookup);
    static inline bool match(UnownedBaseShape* key, const Lookup& lookup);

    // For RootedGeneric<StackBaseShape*>
    void trace(JSTracer* trc);
};

inline
BaseShape::BaseShape(const StackBaseShape& base)
{
    mozilla::PodZero(this);
    this->clasp_ = base.clasp;
    this->parent = base.parent;
    this->metadata = base.metadata;
    this->flags = base.flags;
    this->compartment_ = base.compartment;
}

typedef HashSet<ReadBarrieredUnownedBaseShape,
                StackBaseShape,
                SystemAllocPolicy> BaseShapeSet;


class Shape : public gc::TenuredCell
{
    friend class ::JSObject;
    friend class ::JSFunction;
    friend class Bindings;
    friend class Nursery;
    friend class NativeObject;
    friend class PropertyTree;
    friend class StaticBlockObject;
    friend class ShapeGetterSetterRef;
    friend struct StackShape;
    friend struct StackBaseShape;

  protected:
    HeapPtrBaseShape    base_;
    PreBarrieredId      propid_;

    enum SlotInfo : uint32_t
    {
        /* Number of fixed slots in objects with this shape. */
        // FIXED_SLOTS_MAX is the biggest count of fixed slots a Shape can store
        FIXED_SLOTS_MAX        = 0x1f,
        FIXED_SLOTS_SHIFT      = 27,
        FIXED_SLOTS_MASK       = uint32_t(FIXED_SLOTS_MAX << FIXED_SLOTS_SHIFT),

        /*
         * numLinearSearches starts at zero and is incremented initially on
         * search() calls. Once numLinearSearches reaches LINEAR_SEARCHES_MAX,
         * the table is created on the next search() call. The table can also
         * be created when hashifying for dictionary mode.
         */
        LINEAR_SEARCHES_MAX    = 0x7,
        LINEAR_SEARCHES_SHIFT  = 24,
        LINEAR_SEARCHES_MASK   = LINEAR_SEARCHES_MAX << LINEAR_SEARCHES_SHIFT,

        /*
         * Mask to get the index in object slots for shapes which hasSlot().
         * For !hasSlot() shapes in the property tree with a parent, stores the
         * parent's slot index (which may be invalid), and invalid for all
         * other shapes.
         */
        SLOT_MASK              = JS_BIT(24) - 1
    };

    uint32_t            slotInfo;       /* mask of above info */
    uint8_t             attrs;          /* attributes, see jsapi.h JSPROP_* */
    uint8_t             flags;          /* flags, see below for defines */

    HeapPtrShape        parent;        /* parent node, reverse for..in order */
    /* kids is valid when !inDictionary(), listp is valid when inDictionary(). */
    union {
        KidsPointer kids;       /* null, single child, or a tagged ptr
                                   to many-kids data structure */
        HeapPtrShape* listp;    /* dictionary list starting at shape_
                                   has a double-indirect back pointer,
                                   either to the next shape's parent if not
                                   last, else to obj->shape_ */
    };

    static inline Shape* search(ExclusiveContext* cx, Shape* start, jsid id,
                                ShapeTable::Entry** pentry, bool adding = false);
    static inline Shape* searchNoHashify(Shape* start, jsid id);

    void removeFromDictionary(NativeObject* obj);
    void insertIntoDictionary(HeapPtrShape* dictp);

    inline void initDictionaryShape(const StackShape& child, uint32_t nfixed, HeapPtrShape* dictp);

    /* Replace the base shape of the last shape in a non-dictionary lineage with base. */
    static Shape* replaceLastProperty(ExclusiveContext* cx, StackBaseShape& base,
                                      TaggedProto proto, HandleShape shape);

    /*
     * This function is thread safe if every shape in the lineage of |shape|
     * is thread local, which is the case when we clone the entire shape
     * lineage in preparation for converting an object to dictionary mode.
     */
    static bool hashify(ExclusiveContext* cx, Shape* shape);
    void handoffTableTo(Shape* newShape);

    void setParent(Shape* p) {
        MOZ_ASSERT_IF(p && !p->hasMissingSlot() && !inDictionary(),
                      p->maybeSlot() <= maybeSlot());
        MOZ_ASSERT_IF(p && !inDictionary(),
                      hasSlot() == (p->maybeSlot() != maybeSlot()));
        parent = p;
    }

    bool ensureOwnBaseShape(ExclusiveContext* cx) {
        if (base()->isOwned())
            return true;
        return makeOwnBaseShape(cx);
    }

    bool makeOwnBaseShape(ExclusiveContext* cx);

  public:
    bool hasTable() const { return base()->hasTable(); }
    ShapeTable& table() const { return base()->table(); }

    void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                JS::ClassInfo* info) const
    {
        if (hasTable()) {
            if (inDictionary())
                info->shapesMallocHeapDictTables += table().sizeOfIncludingThis(mallocSizeOf);
            else
                info->shapesMallocHeapTreeTables += table().sizeOfIncludingThis(mallocSizeOf);
        }

        if (!inDictionary() && kids.isHash())
            info->shapesMallocHeapTreeKids += kids.toHash()->sizeOfIncludingThis(mallocSizeOf);
    }

    bool isNative() const {
        MOZ_ASSERT(!(flags & NON_NATIVE) == getObjectClass()->isNative());
        return !(flags & NON_NATIVE);
    }

    bool isAccessorShape() const {
        MOZ_ASSERT_IF(flags & ACCESSOR_SHAPE, getAllocKind() == gc::FINALIZE_ACCESSOR_SHAPE);
        return flags & ACCESSOR_SHAPE;
    }
    AccessorShape& asAccessorShape() const {
        MOZ_ASSERT(isAccessorShape());
        return *(AccessorShape*)this;
    }

    const HeapPtrShape& previous() const { return parent; }
    JSCompartment* compartment() const { return base()->compartment(); }

    template <AllowGC allowGC>
    class Range {
      protected:
        friend class Shape;

        typename MaybeRooted<Shape*, allowGC>::RootType cursor;

      public:
        Range(ExclusiveContext* cx, Shape* shape) : cursor(cx, shape) {
            JS_STATIC_ASSERT(allowGC == CanGC);
        }

        explicit Range(Shape* shape) : cursor((ExclusiveContext*) nullptr, shape) {
            JS_STATIC_ASSERT(allowGC == NoGC);
        }

        bool empty() const {
            return !cursor || cursor->isEmptyShape();
        }

        Shape& front() const {
            MOZ_ASSERT(!empty());
            return *cursor;
        }

        void popFront() {
            MOZ_ASSERT(!empty());
            cursor = cursor->parent;
        }
    };

    const Class* getObjectClass() const {
        return base()->clasp_;
    }
    JSObject* getObjectParent() const { return base()->parent; }
    JSObject* getObjectMetadata() const { return base()->metadata; }

    static Shape* setObjectParent(ExclusiveContext* cx,
                                  JSObject* obj, TaggedProto proto, Shape* last);
    static Shape* setObjectMetadata(JSContext* cx,
                                    JSObject* metadata, TaggedProto proto, Shape* last);
    static Shape* setObjectFlags(ExclusiveContext* cx,
                                 BaseShape::Flag flag, TaggedProto proto, Shape* last);

    uint32_t getObjectFlags() const { return base()->getObjectFlags(); }
    bool hasObjectFlag(BaseShape::Flag flag) const {
        MOZ_ASSERT(!(flag & ~BaseShape::OBJECT_FLAG_MASK));
        return !!(base()->flags & flag);
    }

  protected:
    /*
     * Implementation-private bits stored in shape->flags. See public: enum {}
     * flags further below, which were allocated FCFS over time, so interleave
     * with these bits.
     */
    enum {
        /* Property is placeholder for a non-native class. */
        NON_NATIVE      = 0x01,

        /* Property stored in per-object dictionary, not shared property tree. */
        IN_DICTIONARY   = 0x02,

        /*
         * Slotful property was stored to more than once. This is used as a
         * hint for type inference.
         */
        OVERWRITTEN     = 0x04,

        /*
         * This shape is an AccessorShape, a fat Shape that can store
         * getter/setter information.
         */
        ACCESSOR_SHAPE  = 0x08,

        UNUSED_BITS     = 0x3C
    };

    /* Get a shape identical to this one, without parent/kids information. */
    inline Shape(const StackShape& other, uint32_t nfixed);

    /* Used by EmptyShape (see jsscopeinlines.h). */
    inline Shape(UnownedBaseShape* base, uint32_t nfixed);

    /* Copy constructor disabled, to avoid misuse of the above form. */
    Shape(const Shape& other) = delete;

    /* Allocate a new shape based on the given StackShape. */
    static inline Shape* new_(ExclusiveContext* cx, StackShape& unrootedOther, uint32_t nfixed);

    /*
     * Whether this shape has a valid slot value. This may be true even if
     * !hasSlot() (see SlotInfo comment above), and may be false even if
     * hasSlot() if the shape is being constructed and has not had a slot
     * assigned yet. After construction, hasSlot() implies !hasMissingSlot().
     */
    bool hasMissingSlot() const { return maybeSlot() == SHAPE_INVALID_SLOT; }

  public:
    bool inDictionary() const {
        return (flags & IN_DICTIONARY) != 0;
    }

    inline PropertyOp getter() const;
    bool hasDefaultGetter() const { return !getter(); }
    PropertyOp getterOp() const { MOZ_ASSERT(!hasGetterValue()); return getter(); }
    inline JSObject* getterObject() const;
    bool hasGetterObject() const { return hasGetterValue() && getterObject(); }

    // Per ES5, decode null getterObj as the undefined value, which encodes as null.
    Value getterValue() const {
        MOZ_ASSERT(hasGetterValue());
        if (JSObject* getterObj = getterObject())
            return ObjectValue(*getterObj);
        return UndefinedValue();
    }

    Value getterOrUndefined() const {
        return hasGetterValue() ? getterValue() : UndefinedValue();
    }

    inline StrictPropertyOp setter() const;
    bool hasDefaultSetter() const { return !setter(); }
    StrictPropertyOp setterOp() const { MOZ_ASSERT(!hasSetterValue()); return setter(); }
    inline JSObject* setterObject() const;
    bool hasSetterObject() const { return hasSetterValue() && setterObject(); }

    // Per ES5, decode null setterObj as the undefined value, which encodes as null.
    Value setterValue() const {
        MOZ_ASSERT(hasSetterValue());
        if (JSObject* setterObj = setterObject())
            return ObjectValue(*setterObj);
        return UndefinedValue();
    }

    Value setterOrUndefined() const {
        return hasSetterValue() ? setterValue() : UndefinedValue();
    }

    void setOverwritten() {
        flags |= OVERWRITTEN;
    }
    bool hadOverwrite() const {
        return flags & OVERWRITTEN;
    }

    void update(PropertyOp getter, StrictPropertyOp setter, uint8_t attrs);

    bool matches(const Shape* other) const {
        return propid_.get() == other->propid_.get() &&
               matchesParamsAfterId(other->base(), other->maybeSlot(), other->attrs, other->flags,
                                    other->getter(), other->setter());
    }

    inline bool matches(const StackShape& other) const;

    bool matchesParamsAfterId(BaseShape* base, uint32_t aslot, unsigned aattrs, unsigned aflags,
                              PropertyOp rawGetter, StrictPropertyOp rawSetter) const
    {
        return base->unowned() == this->base()->unowned() &&
               maybeSlot() == aslot &&
               attrs == aattrs &&
               getter() == rawGetter &&
               setter() == rawSetter;
    }

    bool set(JSContext* cx, HandleNativeObject obj, HandleObject receiver, bool strict,
             MutableHandleValue vp);

    BaseShape* base() const { return base_.get(); }

    bool hasSlot() const {
        return (attrs & JSPROP_SHARED) == 0;
    }
    uint32_t slot() const { MOZ_ASSERT(hasSlot() && !hasMissingSlot()); return maybeSlot(); }
    uint32_t maybeSlot() const {
        return slotInfo & SLOT_MASK;
    }

    bool isEmptyShape() const {
        MOZ_ASSERT_IF(JSID_IS_EMPTY(propid_), hasMissingSlot());
        return JSID_IS_EMPTY(propid_);
    }

    uint32_t slotSpan(const Class* clasp) const {
        MOZ_ASSERT(!inDictionary());
        uint32_t free = JSSLOT_FREE(clasp);
        return hasMissingSlot() ? free : Max(free, maybeSlot() + 1);
    }

    uint32_t slotSpan() const {
        return slotSpan(getObjectClass());
    }

    void setSlot(uint32_t slot) {
        MOZ_ASSERT(slot <= SHAPE_INVALID_SLOT);
        slotInfo = slotInfo & ~Shape::SLOT_MASK;
        slotInfo = slotInfo | slot;
    }

    uint32_t numFixedSlots() const {
        return slotInfo >> FIXED_SLOTS_SHIFT;
    }

    void setNumFixedSlots(uint32_t nfixed) {
        MOZ_ASSERT(nfixed < FIXED_SLOTS_MAX);
        slotInfo = slotInfo & ~FIXED_SLOTS_MASK;
        slotInfo = slotInfo | (nfixed << FIXED_SLOTS_SHIFT);
    }

    uint32_t numLinearSearches() const {
        return (slotInfo & LINEAR_SEARCHES_MASK) >> LINEAR_SEARCHES_SHIFT;
    }

    void incrementNumLinearSearches() {
        uint32_t count = numLinearSearches();
        MOZ_ASSERT(count < LINEAR_SEARCHES_MAX);
        slotInfo = slotInfo & ~LINEAR_SEARCHES_MASK;
        slotInfo = slotInfo | ((count + 1) << LINEAR_SEARCHES_SHIFT);
    }

    const PreBarrieredId& propid() const {
        MOZ_ASSERT(!isEmptyShape());
        MOZ_ASSERT(!JSID_IS_VOID(propid_));
        return propid_;
    }
    PreBarrieredId& propidRef() { MOZ_ASSERT(!JSID_IS_VOID(propid_)); return propid_; }
    jsid propidRaw() const {
        // Return the actual jsid, not an internal reference.
        return propid();
    }

    uint8_t attributes() const { return attrs; }
    bool configurable() const { return (attrs & JSPROP_PERMANENT) == 0; }
    bool enumerable() const { return (attrs & JSPROP_ENUMERATE) != 0; }
    bool writable() const {
        return (attrs & JSPROP_READONLY) == 0;
    }
    bool hasGetterValue() const { return attrs & JSPROP_GETTER; }
    bool hasSetterValue() const { return attrs & JSPROP_SETTER; }

    bool isDataDescriptor() const {
        return (attrs & (JSPROP_SETTER | JSPROP_GETTER)) == 0;
    }
    bool isAccessorDescriptor() const {
        return (attrs & (JSPROP_SETTER | JSPROP_GETTER)) != 0;
    }

    PropDesc::Writability writability() const {
        return (attrs & JSPROP_READONLY) ? PropDesc::NonWritable : PropDesc::Writable;
    }
    PropDesc::Enumerability enumerability() const {
        return (attrs & JSPROP_ENUMERATE) ? PropDesc::Enumerable : PropDesc::NonEnumerable;
    }
    PropDesc::Configurability configurability() const {
        return (attrs & JSPROP_PERMANENT) ? PropDesc::NonConfigurable : PropDesc::Configurable;
    }

    /*
     * For ES5 compatibility, we allow properties with PropertyOp-flavored
     * setters to be shadowed when set. The "own" property thereby created in
     * the directly referenced object will have the same getter and setter as
     * the prototype property. See bug 552432.
     */
    bool shadowable() const {
        MOZ_ASSERT_IF(isDataDescriptor(), writable());
        return hasSlot() || (attrs & JSPROP_SHADOWABLE);
    }

    uint32_t entryCount() {
        if (hasTable())
            return table().entryCount();
        uint32_t count = 0;
        for (Shape::Range<NoGC> r(this); !r.empty(); r.popFront())
            ++count;
        return count;
    }

    bool isBigEnoughForAShapeTable() {
        MOZ_ASSERT(!hasTable());
        Shape* shape = this;
        uint32_t count = 0;
        for (Shape::Range<NoGC> r(shape); !r.empty(); r.popFront()) {
            ++count;
            if (count >= ShapeTable::MIN_ENTRIES)
                return true;
        }
        return false;
    }

#ifdef DEBUG
    void dump(JSContext* cx, FILE* fp) const;
    void dumpSubtree(JSContext* cx, int level, FILE* fp) const;
#endif

    void sweep();
    void finalize(FreeOp* fop);
    void removeChild(Shape* child);

    static inline ThingRootKind rootKind() { return THING_ROOT_SHAPE; }

    inline void markChildren(JSTracer* trc);

    inline Shape* search(ExclusiveContext* cx, jsid id);
    inline Shape* searchLinear(jsid id);

    void fixupAfterMovingGC();

    /* For JIT usage */
    static inline size_t offsetOfBase() { return offsetof(Shape, base_); }
    static inline size_t offsetOfSlotInfo() { return offsetof(Shape, slotInfo); }
    static inline uint32_t fixedSlotsMask() { return FIXED_SLOTS_MASK; }

  private:
    void fixupDictionaryShapeAfterMovingGC();
    void fixupShapeTreeAfterMovingGC();

    static void staticAsserts() {
        JS_STATIC_ASSERT(offsetof(Shape, base_) == offsetof(js::shadow::Shape, base));
        JS_STATIC_ASSERT(offsetof(Shape, slotInfo) == offsetof(js::shadow::Shape, slotInfo));
        JS_STATIC_ASSERT(FIXED_SLOTS_SHIFT == js::shadow::Shape::FIXED_SLOTS_SHIFT);
    }
};

/* Fat Shape used for accessor properties. */
class AccessorShape : public Shape
{
    friend class Shape;
    friend class ShapeGetterSetterRef;
    friend class NativeObject;

    union {
        PropertyOp      rawGetter;      /* getter hook for shape */
        JSObject*       getterObj;     /* user-defined callable "get" object or
                                           null if shape->hasGetterValue() */
    };
    union {
        StrictPropertyOp rawSetter;     /* setter hook for shape */
        JSObject*       setterObj;     /* user-defined callable "set" object or
                                           null if shape->hasSetterValue() */
    };

  public:
    /* Get a shape identical to this one, without parent/kids information. */
    inline AccessorShape(const StackShape& other, uint32_t nfixed);
};

inline
StackBaseShape::StackBaseShape(Shape* shape)
  : flags(shape->getObjectFlags()),
    clasp(shape->getObjectClass()),
    parent(shape->getObjectParent()),
    metadata(shape->getObjectMetadata()),
    compartment(shape->compartment())
{}

class AutoRooterGetterSetter
{
    class Inner : private JS::CustomAutoRooter
    {
      public:
        inline Inner(ExclusiveContext* cx, uint8_t attrs,
                     PropertyOp* pgetter_, StrictPropertyOp* psetter_);

      private:
        virtual void trace(JSTracer* trc);

        uint8_t attrs;
        PropertyOp* pgetter;
        StrictPropertyOp* psetter;
    };

  public:
    inline AutoRooterGetterSetter(ExclusiveContext* cx, uint8_t attrs,
                                  PropertyOp* pgetter, StrictPropertyOp* psetter
                                  MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    inline AutoRooterGetterSetter(ExclusiveContext* cx, uint8_t attrs,
                                  JSNative* pgetter, JSNative* psetter
                                  MOZ_GUARD_OBJECT_NOTIFIER_PARAM);

  private:
    mozilla::Maybe<Inner> inner;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

struct EmptyShape : public js::Shape
{
    EmptyShape(UnownedBaseShape* base, uint32_t nfixed)
      : js::Shape(base, nfixed)
    {
        // Only empty shapes can be NON_NATIVE.
        if (!getObjectClass()->isNative())
            flags |= NON_NATIVE;
    }

    static Shape* new_(ExclusiveContext* cx, Handle<UnownedBaseShape*> base, uint32_t nfixed);

    /*
     * Lookup an initial shape matching the given parameters, creating an empty
     * shape if none was found.
     */
    static Shape* getInitialShape(ExclusiveContext* cx, const Class* clasp,
                                  TaggedProto proto, JSObject* parent,
                                  JSObject* metadata, size_t nfixed, uint32_t objectFlags = 0);
    static Shape* getInitialShape(ExclusiveContext* cx, const Class* clasp,
                                  TaggedProto proto, JSObject* parent,
                                  JSObject* metadata, gc::AllocKind kind, uint32_t objectFlags = 0);

    /*
     * Reinsert an alternate initial shape, to be returned by future
     * getInitialShape calls, until the new shape becomes unreachable in a GC
     * and the table entry is purged.
     */
    static void insertInitialShape(ExclusiveContext* cx, HandleShape shape, HandleObject proto);

    /*
     * Some object subclasses are allocated with a built-in set of properties.
     * The first time such an object is created, these built-in properties must
     * be set manually, to compute an initial shape.  Afterward, that initial
     * shape can be reused for newly-created objects that use the subclass's
     * standard prototype.  This method should be used in a post-allocation
     * init method, to ensure that objects of such subclasses compute and cache
     * the initial shape, if it hasn't already been computed.
     */
    template<class ObjectSubclass>
    static inline bool
    ensureInitialCustomShape(ExclusiveContext* cx, Handle<ObjectSubclass*> obj);
};

/*
 * Entries for the per-compartment initialShapes set indexing initial shapes
 * for objects in the compartment and the associated types.
 */
struct InitialShapeEntry
{
    /*
     * Initial shape to give to the object. This is an empty shape, except for
     * certain classes (e.g. String, RegExp) which may add certain baked-in
     * properties.
     */
    ReadBarrieredShape shape;

    /*
     * Matching prototype for the entry. The shape of an object determines its
     * prototype, but the prototype cannot be determined from the shape itself.
     */
    TaggedProto proto;

    /* State used to determine a match on an initial shape. */
    struct Lookup {
        const Class* clasp;
        TaggedProto hashProto;
        TaggedProto matchProto;
        JSObject* hashParent;
        JSObject* matchParent;
        JSObject* hashMetadata;
        JSObject* matchMetadata;
        uint32_t nfixed;
        uint32_t baseFlags;
        Lookup(const Class* clasp, TaggedProto proto, JSObject* parent, JSObject* metadata,
               uint32_t nfixed, uint32_t baseFlags)
          : clasp(clasp),
            hashProto(proto), matchProto(proto),
            hashParent(parent), matchParent(parent),
            hashMetadata(metadata), matchMetadata(metadata),
            nfixed(nfixed), baseFlags(baseFlags)
        {}

        /*
         * For use by generational GC post barriers. Look up an entry whose
         * parent and metadata fields may have been moved, but was hashed with
         * the original values.
         */
        Lookup(const Class* clasp, TaggedProto proto,
               JSObject* hashParent, JSObject* matchParent,
               JSObject* hashMetadata, JSObject* matchMetadata,
               uint32_t nfixed, uint32_t baseFlags)
          : clasp(clasp),
            hashProto(proto), matchProto(proto),
            hashParent(hashParent), matchParent(matchParent),
            hashMetadata(hashMetadata), matchMetadata(matchMetadata),
            nfixed(nfixed), baseFlags(baseFlags)
        {}
    };

    inline InitialShapeEntry();
    inline InitialShapeEntry(const ReadBarrieredShape& shape, TaggedProto proto);

    inline Lookup getLookup() const;

    static inline HashNumber hash(const Lookup& lookup);
    static inline bool match(const InitialShapeEntry& key, const Lookup& lookup);
    static void rekey(InitialShapeEntry& k, const InitialShapeEntry& newKey) { k = newKey; }
};

typedef HashSet<InitialShapeEntry, InitialShapeEntry, SystemAllocPolicy> InitialShapeSet;

struct StackShape
{
    /* For performance, StackShape only roots when absolutely necessary. */
    UnownedBaseShape* base;
    jsid             propid;
    PropertyOp       rawGetter;
    StrictPropertyOp rawSetter;
    uint32_t         slot_;
    uint8_t          attrs;
    uint8_t          flags;

    explicit StackShape(UnownedBaseShape* base, jsid propid, uint32_t slot,
                        unsigned attrs, unsigned flags)
      : base(base),
        propid(propid),
        rawGetter(nullptr),
        rawSetter(nullptr),
        slot_(slot),
        attrs(uint8_t(attrs)),
        flags(uint8_t(flags))
    {
        MOZ_ASSERT(base);
        MOZ_ASSERT(!JSID_IS_VOID(propid));
        MOZ_ASSERT(slot <= SHAPE_INVALID_SLOT);
        MOZ_ASSERT_IF(attrs & (JSPROP_GETTER | JSPROP_SETTER), attrs & JSPROP_SHARED);
    }

    explicit StackShape(Shape* shape)
      : base(shape->base()->unowned()),
        propid(shape->propidRef()),
        rawGetter(shape->getter()),
        rawSetter(shape->setter()),
        slot_(shape->maybeSlot()),
        attrs(shape->attrs),
        flags(shape->flags)
    {}

    void updateGetterSetter(PropertyOp rawGetter, StrictPropertyOp rawSetter) {
        MOZ_ASSERT_IF((attrs & JSPROP_GETTER) && rawGetter, !IsPoisonedPtr(rawGetter));
        MOZ_ASSERT_IF((attrs & JSPROP_SETTER) && rawSetter, !IsPoisonedPtr(rawSetter));

        if (rawGetter || rawSetter || (attrs & (JSPROP_GETTER|JSPROP_SETTER)))
            flags |= Shape::ACCESSOR_SHAPE;
        else
            flags &= ~Shape::ACCESSOR_SHAPE;

        this->rawGetter = rawGetter;
        this->rawSetter = rawSetter;
    }

    bool hasSlot() const { return (attrs & JSPROP_SHARED) == 0; }
    bool hasMissingSlot() const { return maybeSlot() == SHAPE_INVALID_SLOT; }

    uint32_t slot() const { MOZ_ASSERT(hasSlot() && !hasMissingSlot()); return slot_; }
    uint32_t maybeSlot() const { return slot_; }

    uint32_t slotSpan() const {
        uint32_t free = JSSLOT_FREE(base->clasp_);
        return hasMissingSlot() ? free : (maybeSlot() + 1);
    }

    void setSlot(uint32_t slot) {
        MOZ_ASSERT(slot <= SHAPE_INVALID_SLOT);
        slot_ = slot;
    }

    bool isAccessorShape() const {
        return flags & Shape::ACCESSOR_SHAPE;
    }

    HashNumber hash() const {
        HashNumber hash = uintptr_t(base);

        /* Accumulate from least to most random so the low bits are most random. */
        hash = mozilla::RotateLeft(hash, 4) ^ attrs;
        hash = mozilla::RotateLeft(hash, 4) ^ slot_;
        hash = mozilla::RotateLeft(hash, 4) ^ JSID_BITS(propid);
        hash = mozilla::RotateLeft(hash, 4) ^ uintptr_t(rawGetter);
        hash = mozilla::RotateLeft(hash, 4) ^ uintptr_t(rawSetter);
        return hash;
    }

    // For RootedGeneric<StackShape*>
    void trace(JSTracer* trc);
};

inline
Shape::Shape(const StackShape& other, uint32_t nfixed)
  : base_(other.base),
    propid_(other.propid),
    slotInfo(other.maybeSlot() | (nfixed << FIXED_SLOTS_SHIFT)),
    attrs(other.attrs),
    flags(other.flags),
    parent(nullptr)
{
#ifdef DEBUG
    gc::AllocKind allocKind = getAllocKind();
    MOZ_ASSERT_IF(other.isAccessorShape(), allocKind == gc::FINALIZE_ACCESSOR_SHAPE);
    MOZ_ASSERT_IF(allocKind == gc::FINALIZE_SHAPE, !other.isAccessorShape());
#endif

    MOZ_ASSERT_IF(attrs & (JSPROP_GETTER | JSPROP_SETTER), attrs & JSPROP_SHARED);
    kids.setNull();
}

inline
AccessorShape::AccessorShape(const StackShape& other, uint32_t nfixed)
  : Shape(other, nfixed),
    rawGetter(other.rawGetter),
    rawSetter(other.rawSetter)
{
    MOZ_ASSERT(getAllocKind() == gc::FINALIZE_ACCESSOR_SHAPE);

    if ((attrs & JSPROP_GETTER) && rawGetter)
        GetterSetterWriteBarrierPost(this, &this->getterObj);
    if ((attrs & JSPROP_SETTER) && rawSetter)
        GetterSetterWriteBarrierPost(this, &this->setterObj);
}

inline
Shape::Shape(UnownedBaseShape* base, uint32_t nfixed)
  : base_(base),
    propid_(JSID_EMPTY),
    slotInfo(SHAPE_INVALID_SLOT | (nfixed << FIXED_SLOTS_SHIFT)),
    attrs(JSPROP_SHARED),
    flags(0),
    parent(nullptr)
{
    MOZ_ASSERT(base);
    kids.setNull();
}

inline PropertyOp
Shape::getter() const
{
    return isAccessorShape() ? asAccessorShape().rawGetter : nullptr;
}

inline StrictPropertyOp
Shape::setter() const
{
    return isAccessorShape() ? asAccessorShape().rawSetter : nullptr;
}

inline JSObject*
Shape::getterObject() const
{
    MOZ_ASSERT(hasGetterValue());
    return asAccessorShape().getterObj;
}

inline JSObject*
Shape::setterObject() const
{
    MOZ_ASSERT(hasSetterValue());
    return asAccessorShape().setterObj;
}

inline void
Shape::initDictionaryShape(const StackShape& child, uint32_t nfixed, HeapPtrShape* dictp)
{
    if (child.isAccessorShape())
        new (this) AccessorShape(child, nfixed);
    else
        new (this) Shape(child, nfixed);
    this->flags |= IN_DICTIONARY;

    this->listp = nullptr;
    if (dictp)
        insertIntoDictionary(dictp);
}

inline Shape*
Shape::searchLinear(jsid id)
{
    /*
     * Non-dictionary shapes can acquire a table at any point the main thread
     * is operating on it, so other threads inspecting such shapes can't use
     * their table without racing. This function can be called from any thread
     * on any non-dictionary shape.
     */
    MOZ_ASSERT(!inDictionary());

    for (Shape* shape = this; shape; ) {
        if (shape->propidRef() == id)
            return shape;
        shape = shape->parent;
    }

    return nullptr;
}

inline void
Shape::markChildren(JSTracer* trc)
{
    MarkBaseShape(trc, &base_, "base");
    gc::MarkId(trc, &propidRef(), "propid");
    if (parent)
        MarkShape(trc, &parent, "parent");

    if (hasGetterObject())
        gc::MarkObjectUnbarriered(trc, &asAccessorShape().getterObj, "getter");

    if (hasSetterObject())
        gc::MarkObjectUnbarriered(trc, &asAccessorShape().setterObj, "setter");
}

/*
 * Keep this function in sync with search. It neither hashifies the start
 * shape nor increments linear search count.
 */
inline Shape*
Shape::searchNoHashify(Shape* start, jsid id)
{
    /*
     * If we have a table, search in the shape table, else do a linear
     * search. We never hashify into a table in parallel.
     */
    if (start->hasTable()) {
        ShapeTable::Entry& entry = start->table().search(id, false);
        return entry.shape();
    }

    return start->searchLinear(id);
}

inline bool
Shape::matches(const StackShape& other) const
{
    return propid_.get() == other.propid &&
           matchesParamsAfterId(other.base, other.slot_, other.attrs, other.flags,
                                other.rawGetter, other.rawSetter);
}

template<> struct RootKind<Shape*> : SpecificRootKind<Shape*, THING_ROOT_SHAPE> {};
template<> struct RootKind<BaseShape*> : SpecificRootKind<BaseShape*, THING_ROOT_BASE_SHAPE> {};

// Property lookup hooks on objects are required to return a non-nullptr shape
// to signify that the property has been found. For cases where the property is
// not actually represented by a Shape, use a dummy value. This includes all
// properties of non-native objects, and dense elements for native objects.
// Use separate APIs for these two cases.

template <AllowGC allowGC>
static inline void
MarkNonNativePropertyFound(typename MaybeRooted<Shape*, allowGC>::MutableHandleType propp)
{
    propp.set(reinterpret_cast<Shape*>(1));
}

template <AllowGC allowGC>
static inline void
MarkDenseOrTypedArrayElementFound(typename MaybeRooted<Shape*, allowGC>::MutableHandleType propp)
{
    propp.set(reinterpret_cast<Shape*>(1));
}

static inline bool
IsImplicitDenseOrTypedArrayElement(Shape* prop)
{
    return prop == reinterpret_cast<Shape*>(1);
}

Shape*
ReshapeForParentAndAllocKind(JSContext* cx, Shape* shape, TaggedProto proto, JSObject* parent,
                             gc::AllocKind allocKind);

} // namespace js

#ifdef _MSC_VER
#pragma warning(pop)
#pragma warning(pop)
#endif

// JS::ubi::Nodes can point to Shapes and BaseShapes; they're js::gc::Cell
// instances that occupy a compartment.
namespace JS {
namespace ubi {
template<> struct Concrete<js::Shape> : TracerConcreteWithCompartment<js::Shape> { };
template<> struct Concrete<js::BaseShape> : TracerConcreteWithCompartment<js::BaseShape> { };
}
}

#endif /* vm_Shape_h */
