/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ParseMaps_h
#define frontend_ParseMaps_h

#include "mozilla/Attributes.h"
#include "mozilla/TypeTraits.h"

#include "ds/InlineMap.h"
#include "gc/Barrier.h"
#include "js/Vector.h"

class JSAtom;

typedef uintptr_t jsatomid;

namespace js {

class LifoAlloc;

namespace frontend {

class DefinitionSingle;
class DefinitionList;

typedef InlineMap<JSAtom*, jsatomid, 24> AtomIndexMap;
typedef InlineMap<JSAtom*, DefinitionSingle, 24> AtomDefnMap;
typedef InlineMap<JSAtom*, DefinitionList, 24> AtomDefnListMap;

/*
 * For all unmapped atoms recorded in al, add a mapping from the atom's index
 * to its address. map->length must already be set to the number of atoms in
 * the list and map->vector must point to pre-allocated memory.
 */
void
InitAtomMap(AtomIndexMap* indices, HeapPtrAtom* atoms);

/*
 * A pool that permits the reuse of the backing storage for the defn, index, or
 * defn-or-header (multi) maps.
 *
 * The pool owns all the maps that are given out, and is responsible for
 * relinquishing all resources when |purgeAll| is triggered.
 */
class ParseMapPool
{
    typedef Vector<void*, 32, SystemAllocPolicy> RecyclableMaps;

    RecyclableMaps      all;
    RecyclableMaps      recyclable;

    void checkInvariants();

    void recycle(void* map) {
        MOZ_ASSERT(map);
#ifdef DEBUG
        bool ok = false;
        /* Make sure the map is in |all| but not already in |recyclable|. */
        for (void** it = all.begin(); it != all.end(); ++it) {
            if (*it == map) {
                ok = true;
                break;
            }
        }
        MOZ_ASSERT(ok);
        for (void** it = recyclable.begin(); it != recyclable.end(); ++it)
            MOZ_ASSERT(*it != map);
#endif
        MOZ_ASSERT(recyclable.length() < all.length());
        recyclable.infallibleAppend(map); /* Reserved in allocateFresh. */
    }

    void* allocateFresh();
    void* allocate() {
        if (recyclable.empty())
            return allocateFresh();

        void* map = recyclable.popCopy();
        asAtomMap(map)->clear();
        return map;
    }

    /* Arbitrary atom map type, that has keys and values of the same kind. */
    typedef AtomIndexMap AtomMapT;

    static AtomMapT* asAtomMap(void* ptr) {
        return reinterpret_cast<AtomMapT*>(ptr);
    }

  public:
    ~ParseMapPool() {
        purgeAll();
    }

    void purgeAll();

    bool empty() const {
        return all.empty();
    }

    /* Fallibly aquire one of the supported map types from the pool. */
    template <typename T>
    T* acquire() {
        return reinterpret_cast<T*>(allocate());
    }

    /* Release one of the supported map types back to the pool. */

    void release(AtomIndexMap* map) {
        recycle((void*) map);
    }

    void release(AtomDefnMap* map) {
        recycle((void*) map);
    }

    void release(AtomDefnListMap* map) {
        recycle((void*) map);
    }
}; /* ParseMapPool */

/*
 * N.B. This is a POD-type so that it can be included in the ParseNode union.
 * If possible, use the corresponding |OwnedAtomThingMapPtr| variant.
 */
template <class Map>
struct AtomThingMapPtr
{
    Map* map_;

    void init() { clearMap(); }

    bool ensureMap(ExclusiveContext* cx);
    void releaseMap(ExclusiveContext* cx);

    bool hasMap() const { return map_; }
    Map* getMap() { return map_; }
    void setMap(Map* newMap) { MOZ_ASSERT(!map_); map_ = newMap; }
    void clearMap() { map_ = nullptr; }

    Map* operator->() { return map_; }
    const Map* operator->() const { return map_; }
    Map& operator*() const { return *map_; }
};

typedef AtomThingMapPtr<AtomIndexMap> AtomIndexMapPtr;

/*
 * Wrapper around an AtomThingMapPtr (or its derivatives) that automatically
 * releases a map on destruction, if one has been acquired.
 */
template <typename AtomThingMapPtrT>
class OwnedAtomThingMapPtr : public AtomThingMapPtrT
{
    ExclusiveContext* cx;

  public:
    explicit OwnedAtomThingMapPtr(ExclusiveContext* cx) : cx(cx) {
        AtomThingMapPtrT::init();
    }

    ~OwnedAtomThingMapPtr() {
        AtomThingMapPtrT::releaseMap(cx);
    }
};

typedef OwnedAtomThingMapPtr<AtomIndexMapPtr> OwnedAtomIndexMapPtr;

/*
 * DefinitionSingle and DefinitionList represent either a single definition
 * or a list of them. The representation of definitions varies between
 * parse handlers, being either a Definition* (FullParseHandler) or a
 * Definition::Kind (SyntaxParseHandler). Methods on the below classes are
 * templated to distinguish the kind of value wrapped by the class.
 */

/* Wrapper for a single definition. */
class DefinitionSingle
{
    uintptr_t bits;

  public:

    template <typename ParseHandler>
    static DefinitionSingle new_(typename ParseHandler::DefinitionNode defn)
    {
        DefinitionSingle res;
        res.bits = ParseHandler::definitionToBits(defn);
        return res;
    }

    template <typename ParseHandler>
    typename ParseHandler::DefinitionNode get() {
        return ParseHandler::definitionFromBits(bits);
    }
};

struct AtomDefnMapPtr : public AtomThingMapPtr<AtomDefnMap>
{
    template <typename ParseHandler>
    MOZ_ALWAYS_INLINE
    typename ParseHandler::DefinitionNode lookupDefn(JSAtom* atom) {
        AtomDefnMap::Ptr p = map_->lookup(atom);
        return p ? p.value().get<ParseHandler>() : ParseHandler::nullDefinition();
    }
};

typedef OwnedAtomThingMapPtr<AtomDefnMapPtr> OwnedAtomDefnMapPtr;

/*
 * A nonempty list containing one or more pointers to Definitions.
 *
 * By far the most common case is that the list contains exactly one
 * Definition, so the implementation is optimized for that case.
 *
 * Nodes for the linked list (if any) are allocated from the tempPool of a
 * context the caller passes into pushFront and pushBack. This means the
 * DefinitionList does not own the memory for the nodes: the JSContext does.
 * As a result, DefinitionList is a POD type; it can be safely and cheaply
 * copied.
 */
class DefinitionList
{
  public:
    class Range;

  private:
    friend class Range;

    /* A node in a linked list of Definitions. */
    struct Node
    {
        uintptr_t bits;
        Node* next;

        Node(uintptr_t bits, Node* next) : bits(bits), next(next) {}
    };

    union {
        uintptr_t bits;
        Node* head;
    } u;

    Node* firstNode() const {
        MOZ_ASSERT(isMultiple());
        return (Node*) (u.bits & ~0x1);
    }

    static Node*
    allocNode(ExclusiveContext* cx, LifoAlloc& alloc, uintptr_t bits, Node* tail);

  public:
    class Range
    {
        friend class DefinitionList;

        Node* node;
        uintptr_t bits;

        explicit Range(const DefinitionList& list) {
            if (list.isMultiple()) {
                node = list.firstNode();
                bits = node->bits;
            } else {
                node = nullptr;
                bits = list.u.bits;
            }
        }

      public:
        /* An empty Range. */
        Range() : node(nullptr), bits(0) {}

        void popFront() {
            MOZ_ASSERT(!empty());
            if (!node) {
                bits = 0;
                return;
            }
            node = node->next;
            bits = node ? node->bits : 0;
        }

        template <typename ParseHandler>
        typename ParseHandler::DefinitionNode front() {
            MOZ_ASSERT(!empty());
            return ParseHandler::definitionFromBits(bits);
        }

        bool empty() const {
            MOZ_ASSERT_IF(!bits, !node);
            return !bits;
        }
    };

    DefinitionList() {
        u.bits = 0;
    }

    explicit DefinitionList(uintptr_t bits) {
        u.bits = bits;
        MOZ_ASSERT(!isMultiple());
    }

    explicit DefinitionList(Node* node) {
        u.head = node;
        u.bits |= 0x1;
        MOZ_ASSERT(isMultiple());
    }

    bool isMultiple() const { return (u.bits & 0x1) != 0; }

    template <typename ParseHandler>
    typename ParseHandler::DefinitionNode front() {
        return ParseHandler::definitionFromBits(isMultiple() ? firstNode()->bits : u.bits);
    }

    /*
     * If there are multiple Definitions in this list, remove the first and
     * return true. Otherwise there is exactly one Definition in the list; do
     * nothing and return false.
     */
    bool popFront() {
        if (!isMultiple())
            return false;

        Node* node = firstNode();
        Node* next = node->next;
        if (next->next)
            *this = DefinitionList(next);
        else
            *this = DefinitionList(next->bits);
        return true;
    }

    /*
     * Add a definition to the front of this list.
     *
     * Return true on success. On OOM, report on cx and return false.
     */
    template <typename ParseHandler>
    bool pushFront(ExclusiveContext* cx, LifoAlloc& alloc,
                   typename ParseHandler::DefinitionNode defn) {
        Node* tail;
        if (isMultiple()) {
            tail = firstNode();
        } else {
            tail = allocNode(cx, alloc, u.bits, nullptr);
            if (!tail)
                return false;
        }

        Node* node = allocNode(cx, alloc, ParseHandler::definitionToBits(defn), tail);
        if (!node)
            return false;
        *this = DefinitionList(node);
        return true;
    }

    /* Overwrite the first Definition in the list. */
    template <typename ParseHandler>
        void setFront(typename ParseHandler::DefinitionNode defn) {
        if (isMultiple())
            firstNode()->bits = ParseHandler::definitionToBits(defn);
        else
            *this = DefinitionList(ParseHandler::definitionToBits(defn));
    }

    Range all() const { return Range(*this); }

#ifdef DEBUG
    void dump();
#endif
};

typedef AtomDefnMap::Range      AtomDefnRange;
typedef AtomDefnMap::AddPtr     AtomDefnAddPtr;
typedef AtomDefnMap::Ptr        AtomDefnPtr;
typedef AtomIndexMap::AddPtr    AtomIndexAddPtr;
typedef AtomIndexMap::Ptr       AtomIndexPtr;
typedef AtomDefnListMap::Ptr    AtomDefnListPtr;
typedef AtomDefnListMap::AddPtr AtomDefnListAddPtr;
typedef AtomDefnListMap::Range  AtomDefnListRange;

/*
 * AtomDecls is a map of atoms to (sequences of) Definitions. It is used by
 * ParseContext to store declarations. A declaration associates a name with a
 * Definition.
 *
 * Declarations with function scope (such as const, var, and function) are
 * unique in the sense that they override any previous declarations with the
 * same name. For such declarations, we only need to store a single Definition,
 * using the method addUnique.
 *
 * Declarations with block scope (such as let) are slightly more complex. They
 * override any previous declarations with the same name, but only do so for
 * the block they are associated with. This is known as shadowing. For such
 * definitions, we need to store a sequence of Definitions, including those
 * introduced by previous declarations (and which are now shadowed), using the
 * method addShadow. When we leave the block associated with the let, the method
 * remove is used to unshadow the declaration immediately preceding it.
 */
template <typename ParseHandler>
class AtomDecls
{
    typedef typename ParseHandler::DefinitionNode DefinitionNode;

    /* AtomDeclsIter needs to get at the DefnListMap directly. */
    friend class AtomDeclsIter;

    ExclusiveContext* cx;
    LifoAlloc& alloc;
    AtomDefnListMap* map;

    AtomDecls(const AtomDecls& other) = delete;
    void operator=(const AtomDecls& other) = delete;

  public:
    explicit AtomDecls(ExclusiveContext* cx, LifoAlloc& alloc) : cx(cx),
                                                                 alloc(alloc),
                                                                 map(nullptr) {}

    ~AtomDecls();

    bool init();

    void clear() {
        map->clear();
    }

    /* Return the definition at the head of the chain for |atom|. */
    DefinitionNode lookupFirst(JSAtom* atom) const {
        MOZ_ASSERT(map);
        AtomDefnListPtr p = map->lookup(atom);
        if (!p)
            return ParseHandler::nullDefinition();
        return p.value().front<ParseHandler>();
    }

    /* Perform a lookup that can iterate over the definitions associated with |atom|. */
    DefinitionList::Range lookupMulti(JSAtom* atom) const {
        MOZ_ASSERT(map);
        if (AtomDefnListPtr p = map->lookup(atom))
            return p.value().all();
        return DefinitionList::Range();
    }

    /* Add-or-update a known-unique definition for |atom|. */
    bool addUnique(JSAtom* atom, DefinitionNode defn) {
        MOZ_ASSERT(map);
        AtomDefnListAddPtr p = map->lookupForAdd(atom);
        if (!p)
            return map->add(p, atom, DefinitionList(ParseHandler::definitionToBits(defn)));
        MOZ_ASSERT(!p.value().isMultiple());
        p.value() = DefinitionList(ParseHandler::definitionToBits(defn));
        return true;
    }

    bool addShadow(JSAtom* atom, DefinitionNode defn);

    /* Updating the definition for an entry that is known to exist is infallible. */
    void updateFirst(JSAtom* atom, DefinitionNode defn) {
        MOZ_ASSERT(map);
        AtomDefnListMap::Ptr p = map->lookup(atom);
        MOZ_ASSERT(p);
        p.value().setFront<ParseHandler>(defn);
    }

    /* Remove the node at the head of the chain for |atom|. */
    void remove(JSAtom* atom) {
        MOZ_ASSERT(map);
        AtomDefnListMap::Ptr p = map->lookup(atom);
        if (!p)
            return;

        DefinitionList& list = p.value();
        if (!list.popFront()) {
            map->remove(p);
            return;
        }
    }

    AtomDefnListMap::Range all() const {
        MOZ_ASSERT(map);
        return map->all();
    }

#ifdef DEBUG
    void dump();
#endif
};

} /* namespace frontend */

} /* namespace js */

namespace mozilla {

template <>
struct IsPod<js::frontend::DefinitionSingle> : TrueType {};

template <>
struct IsPod<js::frontend::DefinitionList> : TrueType {};

} /* namespace mozilla */

#endif /* frontend_ParseMaps_h */
