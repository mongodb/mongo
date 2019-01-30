/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Shape_h
#define vm_Shape_h

#include "mozilla/Attributes.h"
#include "mozilla/GuardObjects.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/TemplateLib.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "gc/Barrier.h"
#include "gc/Heap.h"
#include "gc/Rooting.h"
#include "js/HashTable.h"
#include "js/MemoryMetrics.h"
#include "js/RootingAPI.h"
#include "js/UbiNode.h"
#include "vm/JSAtom.h"
#include "vm/ObjectGroup.h"
#include "vm/Printer.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"

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
 * To save memory, shape tables can be discarded on GC and recreated when
 * needed. AutoKeepShapeTables can be used to avoid discarding shape tables
 * for a particular zone. Methods operating on ShapeTables take either an
 * AutoCheckCannotGC or AutoKeepShapeTables argument, to help ensure tables
 * are not purged while we're using them.
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

MOZ_ALWAYS_INLINE size_t
JSSLOT_FREE(const js::Class* clasp)
{
    // Proxy classes have reserved slots, but proxies manage their own slot
    // layout.
    MOZ_ASSERT(!clasp->isProxy());
    return JSCLASS_RESERVED_SLOTS(clasp);
}

namespace js {

class Shape;
struct StackShape;

struct ShapeHasher : public DefaultHasher<Shape*> {
    typedef Shape* Key;
    typedef StackShape Lookup;

    static MOZ_ALWAYS_INLINE HashNumber hash(const Lookup& l);
    static MOZ_ALWAYS_INLINE bool match(Key k, const Lookup& l);
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

#ifdef DEBUG
    JS::Zone* zone_;
#endif

    bool insertChild(JSContext* cx, Shape* parent, Shape* child);

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

    explicit PropertyTree(JS::Zone* zone)
#ifdef DEBUG
      : zone_(zone)
#endif
    {
    }

    MOZ_ALWAYS_INLINE Shape* inlinedGetChild(JSContext* cx, Shape* parent,
                                             JS::Handle<StackShape> child);
    Shape* getChild(JSContext* cx, Shape* parent, JS::Handle<StackShape> child);
};

class TenuringTracer;

typedef JSGetterOp GetterOp;
typedef JSSetterOp SetterOp;

/* Limit on the number of slotful properties in an object. */
static const uint32_t SHAPE_INVALID_SLOT = JS_BIT(24) - 1;
static const uint32_t SHAPE_MAXIMUM_SLOT = JS_BIT(24) - 2;

enum class MaybeAdding { Adding = true, NotAdding = false };

class AutoKeepShapeTables;

/*
 * Shapes use multiplicative hashing, but specialized to
 * minimize footprint.
 */
class ShapeTable {
  public:
    friend class NativeObject;
    friend class BaseShape;
    friend class Shape;
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
        bool isLive() const { return !isFree() && !isRemoved(); }
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

    template<MaybeAdding Adding>
    MOZ_ALWAYS_INLINE Entry& searchUnchecked(jsid id);

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

    // init() is fallible and reports OOM to the context.
    bool init(JSContext* cx, Shape* lastProp);

    // change() is fallible but does not report OOM.
    bool change(JSContext* cx, int log2Delta);

    template<MaybeAdding Adding>
    MOZ_ALWAYS_INLINE Entry& search(jsid id, const AutoKeepShapeTables&);

    template<MaybeAdding Adding>
    MOZ_ALWAYS_INLINE Entry& search(jsid id, const JS::AutoCheckCannotGC&);

    void trace(JSTracer* trc);
#ifdef JSGC_HASH_TABLE_CHECKS
    void checkAfterMovingGC();
#endif

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
    bool grow(JSContext* cx);
};

// Ensures no shape tables are purged in the current zone.
class MOZ_RAII AutoKeepShapeTables
{
    JSContext* cx_;
    bool prev_;

    AutoKeepShapeTables(const AutoKeepShapeTables&) = delete;
    void operator=(const AutoKeepShapeTables&) = delete;

  public:
    explicit inline AutoKeepShapeTables(JSContext* cx);
    inline ~AutoKeepShapeTables();
};

/*
 * Use the reserved attribute bit to mean shadowability.
 */
#define JSPROP_SHADOWABLE       JSPROP_INTERNAL_USE_BIT

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
 * Owned BaseShapes are used for shapes which have shape tables, including the
 * last properties in all dictionaries. Unowned BaseShapes compactly store
 * information common to many shapes. In a given zone there is a single
 * BaseShape for each combination of BaseShape information. This information is
 * cloned in owned BaseShapes so that information can be quickly looked up for a
 * given object or shape without regard to whether the base shape is owned or
 * not.
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
 * itself. This includes the object's class and various flags that may be set
 * for the object. Except for the class, this information is mutable and may
 * change when the object has an established property lineage. On such changes
 * the entire property lineage is not updated, but rather only the last property
 * (and its base shape). This works because only the object's last property is
 * used to query information about the object. Care must be taken to call
 * JSObject::canRemoveLastProperty when unwinding an object to an earlier
 * property, however.
 */

class AccessorShape;
class Shape;
class UnownedBaseShape;
struct StackBaseShape;

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
        HAS_INTERESTING_SYMBOL = 0x40,
        HAD_ELEMENTS_ACCESS =   0x80,
        // 0x100 is unused.
        ITERATED_SINGLETON  =  0x200,
        NEW_GROUP_UNKNOWN   =  0x400,
        UNCACHEABLE_PROTO   =  0x800,
        IMMUTABLE_PROTOTYPE = 0x1000,

        // See JSObject::isQualifiedVarObj().
        QUALIFIED_VAROBJ    = 0x2000,

        // 0x4000 is unused.

        // For a function used as an interpreted constructor, whether a 'new'
        // type had constructor information cleared.
        NEW_SCRIPT_CLEARED  = 0x8000,

        OBJECT_FLAG_MASK    = 0xfff8
    };

  private:
    const Class*        clasp_;        /* Class of referring object. */
    uint32_t            flags;          /* Vector of above flags. */
    uint32_t            slotSpan_;      /* Object slot span for BaseShapes at
                                         * dictionary last properties. */

    /* For owned BaseShapes, the canonical unowned BaseShape. */
    GCPtrUnownedBaseShape unowned_;

    /* For owned BaseShapes, the shape's shape table. */
    ShapeTable*      table_;

#if JS_BITS_PER_WORD == 32
    // Ensure sizeof(BaseShape) is a multiple of gc::CellAlignBytes.
    uint32_t padding_;
#endif

    BaseShape(const BaseShape& base) = delete;
    BaseShape& operator=(const BaseShape& other) = delete;

  public:
    void finalize(FreeOp* fop);

    explicit inline BaseShape(const StackBaseShape& base);

    /* Not defined: BaseShapes must not be stack allocated. */
    ~BaseShape();

    const Class* clasp() const { return clasp_; }

    bool isOwned() const { return !!(flags & OWNED_SHAPE); }

    static void copyFromUnowned(BaseShape& dest, UnownedBaseShape& src);
    inline void adoptUnowned(UnownedBaseShape* other);

    void setOwned(UnownedBaseShape* unowned) {
        flags |= OWNED_SHAPE;
        unowned_ = unowned;
    }

    uint32_t getObjectFlags() const { return flags & OBJECT_FLAG_MASK; }

    bool hasTable() const { MOZ_ASSERT_IF(table_, isOwned()); return table_ != nullptr; }
    void setTable(ShapeTable* table) { MOZ_ASSERT(isOwned()); table_ = table; }

    ShapeTable* maybeTable(const AutoKeepShapeTables&) const {
        MOZ_ASSERT_IF(table_, isOwned());
        return table_;
    }
    ShapeTable* maybeTable(const JS::AutoCheckCannotGC&) const {
        MOZ_ASSERT_IF(table_, isOwned());
        return table_;
    }
    void maybePurgeTable() {
        if (table_ && table_->freeList() == SHAPE_INVALID_SLOT) {
            js_delete(table_);
            table_ = nullptr;
        }
    }

    uint32_t slotSpan() const { MOZ_ASSERT(isOwned()); return slotSpan_; }
    void setSlotSpan(uint32_t slotSpan) { MOZ_ASSERT(isOwned()); slotSpan_ = slotSpan; }

    /*
     * Lookup base shapes from the zone's baseShapes table, adding if not
     * already found.
     */
    static UnownedBaseShape* getUnowned(JSContext* cx, StackBaseShape& base);

    /* Get the canonical base shape. */
    inline UnownedBaseShape* unowned();

    /* Get the canonical base shape for an owned one. */
    inline UnownedBaseShape* baseUnowned();

    /* Get the canonical base shape for an unowned one (i.e. identity). */
    inline UnownedBaseShape* toUnowned();

    /* Check that an owned base shape is consistent with its unowned base. */
    void assertConsistency();

    /* For JIT usage */
    static inline size_t offsetOfFlags() { return offsetof(BaseShape, flags); }

    static const JS::TraceKind TraceKind = JS::TraceKind::BaseShape;

    void traceChildren(JSTracer* trc);
    void traceChildrenSkipShapeTable(JSTracer* trc);

#ifdef DEBUG
    bool canSkipMarkingShapeTable(Shape* lastShape);
#endif

  private:
    static void staticAsserts() {
        JS_STATIC_ASSERT(offsetof(BaseShape, clasp_) == offsetof(js::shadow::BaseShape, clasp_));
        static_assert(sizeof(BaseShape) % gc::CellAlignBytes == 0,
                      "Things inheriting from gc::Cell must have a size that's "
                      "a multiple of gc::CellAlignBytes");
    }

    void traceShapeTable(JSTracer* trc);
};

class UnownedBaseShape : public BaseShape {};

UnownedBaseShape*
BaseShape::unowned()
{
    return isOwned() ? baseUnowned() : toUnowned();
}

UnownedBaseShape*
BaseShape::toUnowned()
{
    MOZ_ASSERT(!isOwned() && !unowned_);
    return static_cast<UnownedBaseShape*>(this);
}

UnownedBaseShape*
BaseShape::baseUnowned()
{
    MOZ_ASSERT(isOwned() && unowned_);
    return unowned_;
}

/* Entries for the per-zone baseShapes set of unowned base shapes. */
struct StackBaseShape : public DefaultHasher<ReadBarriered<UnownedBaseShape*>>
{
    uint32_t flags;
    const Class* clasp;

    explicit StackBaseShape(BaseShape* base)
      : flags(base->flags & BaseShape::OBJECT_FLAG_MASK),
        clasp(base->clasp_)
    {}

    inline StackBaseShape(const Class* clasp, uint32_t objectFlags);
    explicit inline StackBaseShape(Shape* shape);

    struct Lookup
    {
        uint32_t flags;
        const Class* clasp;

        MOZ_IMPLICIT Lookup(const StackBaseShape& base)
          : flags(base.flags), clasp(base.clasp)
        {}

        MOZ_IMPLICIT Lookup(UnownedBaseShape* base)
          : flags(base->getObjectFlags()), clasp(base->clasp())
        {
            MOZ_ASSERT(!base->isOwned());
        }

        explicit Lookup(const ReadBarriered<UnownedBaseShape*>& base)
          : flags(base.unbarrieredGet()->getObjectFlags()), clasp(base.unbarrieredGet()->clasp())
        {
            MOZ_ASSERT(!base.unbarrieredGet()->isOwned());
        }
    };

    static HashNumber hash(const Lookup& lookup) {
        return mozilla::HashGeneric(lookup.flags, lookup.clasp);
    }
    static inline bool match(const ReadBarriered<UnownedBaseShape*>& key, const Lookup& lookup) {
        return key.unbarrieredGet()->flags == lookup.flags &&
               key.unbarrieredGet()->clasp_ == lookup.clasp;
    }
};

static MOZ_ALWAYS_INLINE js::HashNumber
HashId(jsid id)
{
    // HashGeneric alone would work, but bits of atom and symbol addresses
    // could then be recovered from the hash code. See bug 1330769.
    if (MOZ_LIKELY(JSID_IS_ATOM(id)))
        return JSID_TO_ATOM(id)->hash();
    if (JSID_IS_SYMBOL(id))
        return JSID_TO_SYMBOL(id)->hash();
    return mozilla::HashGeneric(JSID_BITS(id));
}

template <>
struct DefaultHasher<jsid>
{
    typedef jsid Lookup;
    static HashNumber hash(jsid id) {
        return HashId(id);
    }
    static bool match(jsid id1, jsid id2) {
        return id1 == id2;
    }
};

using BaseShapeSet = JS::WeakCache<JS::GCHashSet<ReadBarriered<UnownedBaseShape*>,
                                                 StackBaseShape,
                                                 SystemAllocPolicy>>;

class Shape : public gc::TenuredCell
{
    friend class ::JSObject;
    friend class ::JSFunction;
    friend class NativeObject;
    friend class PropertyTree;
    friend class TenuringTracer;
    friend struct StackBaseShape;
    friend struct StackShape;
    friend class JS::ubi::Concrete<Shape>;
    friend class js::gc::RelocationOverlay;

  protected:
    GCPtrBaseShape base_;
    GCPtrShape parent;
    PreBarrieredId propid_;

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
         * Mask to get the index in object slots for isDataProperty() shapes.
         * For other shapes in the property tree with a parent, stores the
         * parent's slot index (which may be invalid), and invalid for all
         * other shapes.
         */
        SLOT_MASK              = JS_BIT(24) - 1
    };

    uint32_t            slotInfo;       /* mask of above info */
    uint8_t             attrs;          /* attributes, see jsapi.h JSPROP_* */
    uint8_t             flags;          /* flags, see below for defines */

    /* kids is valid when !inDictionary(), listp is valid when inDictionary(). */
    union {
        KidsPointer kids;         /* null, single child, or a tagged ptr
                                     to many-kids data structure */
        GCPtrShape* listp;        /* dictionary list starting at shape_
                                     has a double-indirect back pointer,
                                     either to the next shape's parent if not
                                     last, else to obj->shape_ */
    };

    template<MaybeAdding Adding = MaybeAdding::NotAdding>
    static MOZ_ALWAYS_INLINE Shape* search(JSContext* cx, Shape* start, jsid id);

    template<MaybeAdding Adding = MaybeAdding::NotAdding>
    static inline MOZ_MUST_USE bool search(JSContext* cx, Shape* start, jsid id,
                                           const AutoKeepShapeTables&,
                                           Shape** pshape, ShapeTable** ptable,
                                           ShapeTable::Entry** pentry);

    static inline Shape* searchNoHashify(Shape* start, jsid id);

    void removeFromDictionary(NativeObject* obj);
    void insertIntoDictionary(GCPtrShape* dictp);

    inline void initDictionaryShape(const StackShape& child, uint32_t nfixed,
                                    GCPtrShape* dictp);

    /* Replace the base shape of the last shape in a non-dictionary lineage with base. */
    static Shape* replaceLastProperty(JSContext* cx, StackBaseShape& base,
                                      TaggedProto proto, HandleShape shape);

    /*
     * This function is thread safe if every shape in the lineage of |shape|
     * is thread local, which is the case when we clone the entire shape
     * lineage in preparation for converting an object to dictionary mode.
     */
    static bool hashify(JSContext* cx, Shape* shape);
    void handoffTableTo(Shape* newShape);

    void setParent(Shape* p) {
        MOZ_ASSERT_IF(p && !p->hasMissingSlot() && !inDictionary(),
                      p->maybeSlot() <= maybeSlot());
        MOZ_ASSERT_IF(p && !inDictionary(),
                      isDataProperty() == (p->maybeSlot() != maybeSlot()));
        parent = p;
    }

    bool ensureOwnBaseShape(JSContext* cx) {
        if (base()->isOwned())
            return true;
        return makeOwnBaseShape(cx);
    }

    bool makeOwnBaseShape(JSContext* cx);

    MOZ_ALWAYS_INLINE MOZ_MUST_USE bool maybeCreateTableForLookup(JSContext* cx);

    MOZ_ALWAYS_INLINE void updateDictionaryTable(ShapeTable* table, ShapeTable::Entry* entry,
                                                 const AutoKeepShapeTables& keep);

  public:
    bool hasTable() const { return base()->hasTable(); }

    ShapeTable* maybeTable(const AutoKeepShapeTables& keep) const {
        return base()->maybeTable(keep);
    }
    ShapeTable* maybeTable(const JS::AutoCheckCannotGC& check) const {
        return base()->maybeTable(check);
    }

    template <typename T>
    MOZ_MUST_USE ShapeTable* ensureTableForDictionary(JSContext* cx, const T& nogc) {
        MOZ_ASSERT(inDictionary());
        if (ShapeTable* table = maybeTable(nogc))
            return table;
        if (!hashify(cx, this))
            return nullptr;
        ShapeTable* table = maybeTable(nogc);
        MOZ_ASSERT(table);
        return table;
    }

    void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                JS::ShapeInfo* info) const
    {
        JS::AutoCheckCannotGC nogc;
        if (ShapeTable* table = maybeTable(nogc)) {
            if (inDictionary())
                info->shapesMallocHeapDictTables += table->sizeOfIncludingThis(mallocSizeOf);
            else
                info->shapesMallocHeapTreeTables += table->sizeOfIncludingThis(mallocSizeOf);
        }

        if (!inDictionary() && kids.isHash())
            info->shapesMallocHeapTreeKids += kids.toHash()->sizeOfIncludingThis(mallocSizeOf);
    }

    bool isAccessorShape() const {
        MOZ_ASSERT_IF(flags & ACCESSOR_SHAPE, getAllocKind() == gc::AllocKind::ACCESSOR_SHAPE);
        return flags & ACCESSOR_SHAPE;
    }
    AccessorShape& asAccessorShape() const {
        MOZ_ASSERT(isAccessorShape());
        return *(AccessorShape*)this;
    }

    const GCPtrShape& previous() const { return parent; }

    template <AllowGC allowGC>
    class Range {
      protected:
        friend class Shape;

        typename MaybeRooted<Shape*, allowGC>::RootType cursor;

      public:
        Range(JSContext* cx, Shape* shape) : cursor(cx, shape) {
            JS_STATIC_ASSERT(allowGC == CanGC);
        }

        explicit Range(Shape* shape) : cursor((JSContext*) nullptr, shape) {
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

    static Shape* setObjectFlags(JSContext* cx,
                                 BaseShape::Flag flag, TaggedProto proto, Shape* last);

    uint32_t getObjectFlags() const { return base()->getObjectFlags(); }
    bool hasAllObjectFlags(BaseShape::Flag flags) const {
        MOZ_ASSERT(flags);
        MOZ_ASSERT(!(flags & ~BaseShape::OBJECT_FLAG_MASK));
        return (base()->flags & flags) == flags;
    }

  protected:
    /*
     * Implementation-private bits stored in shape->flags. See public: enum {}
     * flags further below, which were allocated FCFS over time, so interleave
     * with these bits.
     */
    enum {
        /* Property stored in per-object dictionary, not shared property tree. */
        IN_DICTIONARY   = 0x01,

        /*
         * Slotful property was stored to more than once. This is used as a
         * hint for type inference.
         */
        OVERWRITTEN     = 0x02,

        /*
         * This shape is an AccessorShape, a fat Shape that can store
         * getter/setter information.
         */
        ACCESSOR_SHAPE  = 0x04,

        /* Flags used to speed up isBigEnoughForAShapeTable(). */
        HAS_CACHED_BIG_ENOUGH_FOR_SHAPE_TABLE = 0x08,
        CACHED_BIG_ENOUGH_FOR_SHAPE_TABLE = 0x10,
    };

    /* Get a shape identical to this one, without parent/kids information. */
    inline Shape(const StackShape& other, uint32_t nfixed);

    /* Used by EmptyShape (see jsscopeinlines.h). */
    inline Shape(UnownedBaseShape* base, uint32_t nfixed);

    /* Copy constructor disabled, to avoid misuse of the above form. */
    Shape(const Shape& other) = delete;

    /* Allocate a new shape based on the given StackShape. */
    static inline Shape* new_(JSContext* cx, Handle<StackShape> other, uint32_t nfixed);

    /*
     * Whether this shape has a valid slot value. This may be true even if
     * !isDataProperty() (see SlotInfo comment above), and may be false even if
     * isDataProperty() if the shape is being constructed and has not had a slot
     * assigned yet. After construction, isDataProperty() implies
     * !hasMissingSlot().
     */
    bool hasMissingSlot() const { return maybeSlot() == SHAPE_INVALID_SLOT; }

  public:
    bool inDictionary() const {
        return (flags & IN_DICTIONARY) != 0;
    }

    inline GetterOp getter() const;
    bool hasDefaultGetter() const { return !getter(); }
    GetterOp getterOp() const { MOZ_ASSERT(!hasGetterValue()); return getter(); }
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

    inline SetterOp setter() const;
    bool hasDefaultSetter() const { return !setter(); }
    SetterOp setterOp() const { MOZ_ASSERT(!hasSetterValue()); return setter(); }
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

    bool matches(const Shape* other) const {
        return propid_.get() == other->propid_.get() &&
               matchesParamsAfterId(other->base(), other->maybeSlot(), other->attrs,
                                    other->getter(), other->setter());
    }

    inline bool matches(const StackShape& other) const;

    bool matchesParamsAfterId(BaseShape* base, uint32_t aslot, unsigned aattrs,
                              GetterOp rawGetter, SetterOp rawSetter) const
    {
        return base->unowned() == this->base()->unowned() &&
               maybeSlot() == aslot &&
               attrs == aattrs &&
               getter() == rawGetter &&
               setter() == rawSetter;
    }

    BaseShape* base() const { return base_.get(); }

    static bool isDataProperty(unsigned attrs, GetterOp getter, SetterOp setter) {
        return !(attrs & (JSPROP_GETTER | JSPROP_SETTER)) && !getter && !setter;
    }

    bool isDataProperty() const {
        MOZ_ASSERT(!isEmptyShape());
        return isDataProperty(attrs, getter(), setter());
    }
    uint32_t slot() const { MOZ_ASSERT(isDataProperty() && !hasMissingSlot()); return maybeSlot(); }
    uint32_t maybeSlot() const {
        return slotInfo & SLOT_MASK;
    }

    bool isEmptyShape() const {
        MOZ_ASSERT_IF(JSID_IS_EMPTY(propid_), hasMissingSlot());
        return JSID_IS_EMPTY(propid_);
    }

    uint32_t slotSpan(const Class* clasp) const {
        MOZ_ASSERT(!inDictionary());
        // Proxy classes have reserved slots, but proxies manage their own slot
        // layout. This means all non-native object shapes have nfixed == 0 and
        // slotSpan == 0.
        uint32_t free = clasp->isProxy() ? 0 : JSSLOT_FREE(clasp);
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

    bool hasShadowable() const { return attrs & JSPROP_SHADOWABLE; }

    uint32_t entryCount() {
        JS::AutoCheckCannotGC nogc;
        if (ShapeTable* table = maybeTable(nogc))
            return table->entryCount();
        uint32_t count = 0;
        for (Shape::Range<NoGC> r(this); !r.empty(); r.popFront())
            ++count;
        return count;
    }

  private:
    bool isBigEnoughForAShapeTableSlow() {
        uint32_t count = 0;
        for (Shape::Range<NoGC> r(this); !r.empty(); r.popFront()) {
            ++count;
            if (count >= ShapeTable::MIN_ENTRIES)
                return true;
        }
        return false;
    }
    void clearCachedBigEnoughForShapeTable() {
        flags &= ~(HAS_CACHED_BIG_ENOUGH_FOR_SHAPE_TABLE | CACHED_BIG_ENOUGH_FOR_SHAPE_TABLE);
    }

  public:
    bool isBigEnoughForAShapeTable() {
        MOZ_ASSERT(!hasTable());

        // isBigEnoughForAShapeTableSlow is pretty inefficient so we only call
        // it once and cache the result.

        if (flags & HAS_CACHED_BIG_ENOUGH_FOR_SHAPE_TABLE) {
            bool res = flags & CACHED_BIG_ENOUGH_FOR_SHAPE_TABLE;
            MOZ_ASSERT(res == isBigEnoughForAShapeTableSlow());
            return res;
        }

        MOZ_ASSERT(!(flags & CACHED_BIG_ENOUGH_FOR_SHAPE_TABLE));

        bool res = isBigEnoughForAShapeTableSlow();
        if (res)
            flags |= CACHED_BIG_ENOUGH_FOR_SHAPE_TABLE;
        flags |= HAS_CACHED_BIG_ENOUGH_FOR_SHAPE_TABLE;
        return res;
    }

#ifdef DEBUG
    void dump(js::GenericPrinter& out) const;
    void dump() const;
    void dumpSubtree(int level, js::GenericPrinter& out) const;
#endif

    void sweep();
    void finalize(FreeOp* fop);
    void removeChild(Shape* child);

    static const JS::TraceKind TraceKind = JS::TraceKind::Shape;

    void traceChildren(JSTracer* trc);

    MOZ_ALWAYS_INLINE Shape* search(JSContext* cx, jsid id);
    MOZ_ALWAYS_INLINE Shape* searchLinear(jsid id);

    void fixupAfterMovingGC();
    void fixupGetterSetterForBarrier(JSTracer* trc);
    void updateBaseShapeAfterMovingGC();

#ifdef DEBUG
    // For JIT usage.
    static inline size_t offsetOfSlotInfo() { return offsetof(Shape, slotInfo); }
    static inline uint32_t fixedSlotsMask() { return FIXED_SLOTS_MASK; }
#endif

  private:
    void fixupDictionaryShapeAfterMovingGC();
    void fixupShapeTreeAfterMovingGC();

    static Shape* fromParentFieldPointer(uintptr_t p) {
        return reinterpret_cast<Shape*>(p - offsetof(Shape, parent));
    }

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
    friend class NativeObject;

    union {
        GetterOp rawGetter;     /* getter hook for shape */
        JSObject* getterObj;    /* user-defined callable "get" object or
                                   null if shape->hasGetterValue() */
    };
    union {
        SetterOp rawSetter;     /* setter hook for shape */
        JSObject* setterObj;    /* user-defined callable "set" object or
                                   null if shape->hasSetterValue() */
    };

  public:
    /* Get a shape identical to this one, without parent/kids information. */
    inline AccessorShape(const StackShape& other, uint32_t nfixed);
};

inline
StackBaseShape::StackBaseShape(Shape* shape)
  : flags(shape->getObjectFlags()),
    clasp(shape->getObjectClass())
{}

class MOZ_RAII AutoRooterGetterSetter
{
    class Inner final : private JS::CustomAutoRooter
    {
      public:
        inline Inner(JSContext* cx, uint8_t attrs, GetterOp* pgetter_, SetterOp* psetter_);

      private:
        virtual void trace(JSTracer* trc) override;

        uint8_t attrs;
        GetterOp* pgetter;
        SetterOp* psetter;
    };

  public:
    inline AutoRooterGetterSetter(JSContext* cx, uint8_t attrs,
                                  GetterOp* pgetter, SetterOp* psetter
                                  MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    inline AutoRooterGetterSetter(JSContext* cx, uint8_t attrs,
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
    { }

    static Shape* new_(JSContext* cx, Handle<UnownedBaseShape*> base, uint32_t nfixed);

    /*
     * Lookup an initial shape matching the given parameters, creating an empty
     * shape if none was found.
     */
    static Shape* getInitialShape(JSContext* cx, const Class* clasp,
                                  TaggedProto proto, size_t nfixed, uint32_t objectFlags = 0);
    static Shape* getInitialShape(JSContext* cx, const Class* clasp,
                                  TaggedProto proto, gc::AllocKind kind, uint32_t objectFlags = 0);

    /*
     * Reinsert an alternate initial shape, to be returned by future
     * getInitialShape calls, until the new shape becomes unreachable in a GC
     * and the table entry is purged.
     */
    static void insertInitialShape(JSContext* cx, HandleShape shape, HandleObject proto);

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
    ensureInitialCustomShape(JSContext* cx, Handle<ObjectSubclass*> obj);
};

// InitialShapeProto stores either:
//
// * A TaggedProto (or ReadBarriered<TaggedProto>).
//
// * A JSProtoKey. This is used instead of the TaggedProto if the proto is one
//   of the global's builtin prototypes. For instance, if the proto is the
//   initial Object.prototype, we use key_ = JSProto_Object, proto_ = nullptr.
//
// Using the JSProtoKey here is an optimization that lets us share more shapes
// across compartments within a zone.
template <typename PtrType>
class InitialShapeProto
{
    template <typename T> friend class InitialShapeProto;

    JSProtoKey key_;
    PtrType proto_;

  public:
    InitialShapeProto()
      : key_(JSProto_LIMIT), proto_()
    {}

    InitialShapeProto(JSProtoKey key, TaggedProto proto)
      : key_(key), proto_(proto)
    {}

    template <typename T>
    explicit InitialShapeProto(const InitialShapeProto<T>& other)
      : key_(other.key()), proto_(other.proto_)
    {}

    explicit InitialShapeProto(TaggedProto proto)
      : key_(JSProto_LIMIT), proto_(proto)
    {}
    explicit InitialShapeProto(JSProtoKey key)
      : key_(key), proto_(nullptr)
    {
        MOZ_ASSERT(key < JSProto_LIMIT);
    }

    JSProtoKey key() const {
        return key_;
    }
    const PtrType& proto() const {
        return proto_;
    }
    void setProto(TaggedProto proto) {
        proto_ = proto;
    }

    bool operator==(const InitialShapeProto& other) const {
        return key_ == other.key_ && proto_ == other.proto_;
    }
};

template <>
struct MovableCellHasher<InitialShapeProto<ReadBarriered<TaggedProto>>>
{
    using Key = InitialShapeProto<ReadBarriered<TaggedProto>>;
    using Lookup = InitialShapeProto<TaggedProto>;

    static bool hasHash(const Lookup& l) {
        return MovableCellHasher<TaggedProto>::hasHash(l.proto());
    }
    static bool ensureHash(const Lookup& l) {
        return MovableCellHasher<TaggedProto>::ensureHash(l.proto());
    }
    static HashNumber hash(const Lookup& l) {
        HashNumber hash = MovableCellHasher<TaggedProto>::hash(l.proto());
        return mozilla::AddToHash(hash, l.key());
    }
    static bool match(const Key& k, const Lookup& l) {
        return k.key() == l.key() &&
               MovableCellHasher<TaggedProto>::match(k.proto().unbarrieredGet(), l.proto());
    }
};

/*
 * Entries for the per-zone initialShapes set indexing initial shapes for
 * objects in the zone and the associated types.
 */
struct InitialShapeEntry
{
    /*
     * Initial shape to give to the object. This is an empty shape, except for
     * certain classes (e.g. String, RegExp) which may add certain baked-in
     * properties.
     */
    ReadBarriered<Shape*> shape;

    /*
     * Matching prototype for the entry. The shape of an object determines its
     * prototype, but the prototype cannot be determined from the shape itself.
     */
    using ShapeProto = InitialShapeProto<ReadBarriered<TaggedProto>>;
    ShapeProto proto;

    /* State used to determine a match on an initial shape. */
    struct Lookup {
        using ShapeProto = InitialShapeProto<TaggedProto>;
        const Class* clasp;
        ShapeProto proto;
        uint32_t nfixed;
        uint32_t baseFlags;

        Lookup(const Class* clasp, ShapeProto proto, uint32_t nfixed, uint32_t baseFlags)
          : clasp(clasp), proto(proto), nfixed(nfixed), baseFlags(baseFlags)
        {}

        explicit Lookup(const InitialShapeEntry& entry)
          : proto(entry.proto.key(),
                  entry.proto.proto().unbarrieredGet())
        {
            const Shape* shape = entry.shape.unbarrieredGet();
            clasp = shape->getObjectClass();
            nfixed = shape->numFixedSlots();
            baseFlags = shape->getObjectFlags();
        }
    };

    inline InitialShapeEntry();
    inline InitialShapeEntry(Shape* shape, const Lookup::ShapeProto& proto);

    static HashNumber hash(const Lookup& lookup) {
        HashNumber hash = MovableCellHasher<ShapeProto>::hash(lookup.proto);
        return mozilla::AddToHash(hash, mozilla::HashGeneric(lookup.clasp, lookup.nfixed));
    }
    static inline bool match(const InitialShapeEntry& key, const Lookup& lookup) {
        const Shape* shape = key.shape.unbarrieredGet();
        return lookup.clasp == shape->getObjectClass()
            && lookup.nfixed == shape->numFixedSlots()
            && lookup.baseFlags == shape->getObjectFlags()
            && MovableCellHasher<ShapeProto>::match(key.proto, lookup.proto);
    }
    static void rekey(InitialShapeEntry& k, const InitialShapeEntry& newKey) {
        k = newKey;
    }

    bool needsSweep() {
        Shape* ushape = shape.unbarrieredGet();
        TaggedProto uproto = proto.proto().unbarrieredGet();
        JSObject* protoObj = uproto.raw();
        return (gc::IsAboutToBeFinalizedUnbarriered(&ushape) ||
                (uproto.isObject() && gc::IsAboutToBeFinalizedUnbarriered(&protoObj)));
    }

    bool operator==(const InitialShapeEntry& other) const {
        return shape == other.shape && proto == other.proto;
    }
};

using InitialShapeSet = JS::WeakCache<JS::GCHashSet<InitialShapeEntry,
                                                    InitialShapeEntry,
                                                    SystemAllocPolicy>>;

struct StackShape
{
    /* For performance, StackShape only roots when absolutely necessary. */
    UnownedBaseShape* base;
    jsid propid;
    GetterOp rawGetter;
    SetterOp rawSetter;
    uint32_t slot_;
    uint8_t attrs;
    uint8_t flags;

    explicit StackShape(UnownedBaseShape* base, jsid propid, uint32_t slot,
                        unsigned attrs)
      : base(base),
        propid(propid),
        rawGetter(nullptr),
        rawSetter(nullptr),
        slot_(slot),
        attrs(uint8_t(attrs)),
        flags(0)
    {
        MOZ_ASSERT(base);
        MOZ_ASSERT(!JSID_IS_VOID(propid));
        MOZ_ASSERT(slot <= SHAPE_INVALID_SLOT);
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

    void updateGetterSetter(GetterOp rawGetter, SetterOp rawSetter) {
        if (rawGetter || rawSetter || (attrs & (JSPROP_GETTER|JSPROP_SETTER)))
            flags |= Shape::ACCESSOR_SHAPE;
        else
            flags &= ~Shape::ACCESSOR_SHAPE;

        this->rawGetter = rawGetter;
        this->rawSetter = rawSetter;
    }

    bool isDataProperty() const {
        MOZ_ASSERT(!JSID_IS_EMPTY(propid));
        return Shape::isDataProperty(attrs, rawGetter, rawSetter);
    }
    bool hasMissingSlot() const { return maybeSlot() == SHAPE_INVALID_SLOT; }

    uint32_t slot() const { MOZ_ASSERT(isDataProperty() && !hasMissingSlot()); return slot_; }
    uint32_t maybeSlot() const { return slot_; }

    void setSlot(uint32_t slot) {
        MOZ_ASSERT(slot <= SHAPE_INVALID_SLOT);
        slot_ = slot;
    }

    bool isAccessorShape() const {
        return flags & Shape::ACCESSOR_SHAPE;
    }

    HashNumber hash() const {
        HashNumber hash = HashId(propid);
        return mozilla::AddToHash(hash,
                   mozilla::HashGeneric(base, attrs, slot_, rawGetter, rawSetter));
    }

    // Traceable implementation.
    static void trace(StackShape* stackShape, JSTracer* trc) { stackShape->trace(trc); }
    void trace(JSTracer* trc);
};

template <typename Wrapper>
class WrappedPtrOperations<StackShape, Wrapper>
{
    const StackShape& ss() const { return static_cast<const Wrapper*>(this)->get(); }

  public:
    bool isDataProperty() const { return ss().isDataProperty(); }
    bool hasMissingSlot() const { return ss().hasMissingSlot(); }
    uint32_t slot() const { return ss().slot(); }
    uint32_t maybeSlot() const { return ss().maybeSlot(); }
    uint32_t slotSpan() const { return ss().slotSpan(); }
    bool isAccessorShape() const { return ss().isAccessorShape(); }
    uint8_t attrs() const { return ss().attrs; }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<StackShape, Wrapper>
  : public WrappedPtrOperations<StackShape, Wrapper>
{
    StackShape& ss() { return static_cast<Wrapper*>(this)->get(); }

  public:
    void updateGetterSetter(GetterOp rawGetter, SetterOp rawSetter) {
        ss().updateGetterSetter(rawGetter, rawSetter);
    }
    void setSlot(uint32_t slot) { ss().setSlot(slot); }
    void setBase(UnownedBaseShape* base) { ss().base = base; }
    void setAttrs(uint8_t attrs) { ss().attrs = attrs; }
};

inline
Shape::Shape(const StackShape& other, uint32_t nfixed)
  : base_(other.base),
    parent(nullptr),
    propid_(other.propid),
    slotInfo(other.maybeSlot() | (nfixed << FIXED_SLOTS_SHIFT)),
    attrs(other.attrs),
    flags(other.flags)
{
#ifdef DEBUG
    gc::AllocKind allocKind = getAllocKind();
    MOZ_ASSERT_IF(other.isAccessorShape(), allocKind == gc::AllocKind::ACCESSOR_SHAPE);
    MOZ_ASSERT_IF(allocKind == gc::AllocKind::SHAPE, !other.isAccessorShape());
#endif

    MOZ_ASSERT_IF(!isEmptyShape(), AtomIsMarked(zone(), propid()));

    kids.setNull();
}

// This class is used to update any shapes in a zone that have nursery objects
// as getters/setters.  It updates the pointers and the shapes' entries in the
// parents' KidsHash tables.
class NurseryShapesRef : public gc::BufferableRef
{
    Zone* zone_;

  public:
    explicit NurseryShapesRef(Zone* zone) : zone_(zone) {}
    void trace(JSTracer* trc) override;
};

inline
Shape::Shape(UnownedBaseShape* base, uint32_t nfixed)
  : base_(base),
    parent(nullptr),
    propid_(JSID_EMPTY),
    slotInfo(SHAPE_INVALID_SLOT | (nfixed << FIXED_SLOTS_SHIFT)),
    attrs(0),
    flags(0)
{
    MOZ_ASSERT(base);
    kids.setNull();
}

inline GetterOp
Shape::getter() const
{
    return isAccessorShape() ? asAccessorShape().rawGetter : nullptr;
}

inline SetterOp
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

inline Shape*
Shape::searchLinear(jsid id)
{
    for (Shape* shape = this; shape; ) {
        if (shape->propidRef() == id)
            return shape;
        shape = shape->parent;
    }

    return nullptr;
}

inline bool
Shape::matches(const StackShape& other) const
{
    return propid_.get() == other.propid &&
           matchesParamsAfterId(other.base, other.slot_, other.attrs,
                                other.rawGetter, other.rawSetter);
}

Shape*
ReshapeForAllocKind(JSContext* cx, Shape* shape, TaggedProto proto,
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

template<>
class Concrete<js::Shape> : TracerConcrete<js::Shape> {
  protected:
    explicit Concrete(js::Shape *ptr) : TracerConcrete<js::Shape>(ptr) { }

  public:
    static void construct(void *storage, js::Shape *ptr) { new (storage) Concrete(ptr); }

    Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

    const char16_t* typeName() const override { return concreteTypeName; }
    static const char16_t concreteTypeName[];
};

template<>
class Concrete<js::BaseShape> : TracerConcrete<js::BaseShape> {
  protected:
    explicit Concrete(js::BaseShape *ptr) : TracerConcrete<js::BaseShape>(ptr) { }

  public:
    static void construct(void *storage, js::BaseShape *ptr) { new (storage) Concrete(ptr); }

    Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

    const char16_t* typeName() const override { return concreteTypeName; }
    static const char16_t concreteTypeName[];
};

} // namespace ubi
} // namespace JS

#endif /* vm_Shape_h */
