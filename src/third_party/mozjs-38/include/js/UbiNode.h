/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_UbiNode_h
#define js_UbiNode_h

#include "mozilla/Alignment.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"

#include "jspubtd.h"

#include "js/GCAPI.h"
#include "js/HashTable.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"
#include "js/Vector.h"

// JS::ubi::Node
//
// JS::ubi::Node is a pointer-like type designed for internal use by heap
// analysis tools. A ubi::Node can refer to:
//
// - a JS value, like a string, object, or symbol;
// - an internal SpiderMonkey structure, like a shape or a scope chain object
// - an instance of some embedding-provided type: in Firefox, an XPCOM
//   object, or an internal DOM node class instance
//
// A ubi::Node instance provides metadata about its referent, and can
// enumerate its referent's outgoing edges, so you can implement heap analysis
// algorithms that walk the graph - finding paths between objects, or
// computing heap dominator trees, say - using ubi::Node, while remaining
// ignorant of the details of the types you're operating on.
//
// Of course, when it comes to presenting the results in a developer-facing
// tool, you'll need to stop being ignorant of those details, because you have
// to discuss the ubi::Nodes' referents with the developer. Here, ubi::Node
// can hand you dynamically checked, properly typed pointers to the original
// objects via the as<T> method, or generate descriptions of the referent
// itself.
//
// ubi::Node instances are lightweight (two-word) value types. Instances:
// - compare equal if and only if they refer to the same object;
// - have hash values that respect their equality relation; and
// - have serializations that are only equal if the ubi::Nodes are equal.
//
// A ubi::Node is only valid for as long as its referent is alive; if its
// referent goes away, the ubi::Node becomes a dangling pointer. A ubi::Node
// that refers to a GC-managed object is not automatically a GC root; if the
// GC frees or relocates its referent, the ubi::Node becomes invalid. A
// ubi::Node that refers to a reference-counted object does not bump the
// reference count.
//
// ubi::Node values require no supporting data structures, making them
// feasible for use in memory-constrained devices --- ideally, the memory
// requirements of the algorithm which uses them will be the limiting factor,
// not the demands of ubi::Node itself.
//
// One can construct a ubi::Node value given a pointer to a type that ubi::Node
// supports. In the other direction, one can convert a ubi::Node back to a
// pointer; these downcasts are checked dynamically. In particular, one can
// convert a 'JSRuntime*' to a ubi::Node, yielding a node with an outgoing edge
// for every root registered with the runtime; starting from this, one can walk
// the entire heap. (Of course, one could also start traversal at any other kind
// of type to which one has a pointer.)
//
//
// Extending ubi::Node To Handle Your Embedding's Types
//
// To add support for a new ubi::Node referent type R, you must define a
// specialization of the ubi::Concrete template, ubi::Concrete<R>, which
// inherits from ubi::Base. ubi::Node itself uses the specialization for
// compile-time information (i.e. the checked conversions between R * and
// ubi::Node), and the inheritance for run-time dispatching.
//
//
// ubi::Node Exposes Implementation Details
//
// In many cases, a JavaScript developer's view of their data differs
// substantially from its actual implementation. For example, while the
// ECMAScript specification describes objects as maps from property names to
// sets of attributes (like ECMAScript's [[Value]]), in practice many objects
// have only a pointer to a shape, shared with other similar objects, and
// indexed slots that contain the [[Value]] attributes. As another example, a
// string produced by concatenating two other strings may sometimes be
// represented by a "rope", a structure that points to the two original
// strings.
//

// We intend to use ubi::Node to write tools that report memory usage, so it's
// important that ubi::Node accurately portray how much memory nodes consume.
// Thus, for example, when data that apparently belongs to multiple nodes is
// in fact shared in a common structure, ubi::Node's graph uses a separate
// node for that shared structure, and presents edges to it from the data's
// apparent owners. For example, ubi::Node exposes SpiderMonkey objects'
// shapes and base shapes, and exposes rope string and substring structure,
// because these optimizations become visible when a tool reports how much
// memory a structure consumes.
//
// However, fine granularity is not a goal. When a particular object is the
// exclusive owner of a separate block of memory, ubi::Node may present the
// object and its block as a single node, and add their sizes together when
// reporting the node's size, as there is no meaningful loss of data in this
// case. Thus, for example, a ubi::Node referring to a JavaScript object, when
// asked for the object's size in bytes, includes the object's slot and
// element arrays' sizes in the total. There is no separate ubi::Node value
// representing the slot and element arrays, since they are owned exclusively
// by the object.
//
//
// Presenting Analysis Results To JavaScript Developers
//
// If an analysis provides its results in terms of ubi::Node values, a user
// interface presenting those results will generally need to clean them up
// before they can be understood by JavaScript developers. For example,
// JavaScript developers should not need to understand shapes, only JavaScript
// objects. Similarly, they should not need to understand the distinction
// between DOM nodes and the JavaScript shadow objects that represent them.
//
//
// Rooting Restrictions
//
// At present there is no way to root ubi::Node instances, so instances can't be
// live across any operation that might GC. Analyses using ubi::Node must either
// run to completion and convert their results to some other rootable type, or
// save their intermediate state in some rooted structure if they must GC before
// they complete. (For algorithms like path-finding and dominator tree
// computation, we implement the algorithm avoiding any operation that could
// cause a GC --- and use AutoCheckCannotGC to verify this.)
//
// If this restriction prevents us from implementing interesting tools, we may
// teach the GC how to root ubi::Nodes, fix up hash tables that use them as
// keys, etc.

namespace JS {
namespace ubi {

using mozilla::Maybe;

class Edge;
class EdgeRange;

// The base class implemented by each ubi::Node referent type. Subclasses must
// not add data members to this class.
class Base {
    friend class Node;

    // For performance's sake, we'd prefer to avoid a virtual destructor; and
    // an empty constructor seems consistent with the 'lightweight value type'
    // visible behavior we're trying to achieve. But if the destructor isn't
    // virtual, and a subclass overrides it, the subclass's destructor will be
    // ignored. Is there a way to make the compiler catch that error?

  protected:
    // Space for the actual pointer. Concrete subclasses should define a
    // properly typed 'get' member function to access this.
    void* ptr;

    explicit Base(void* ptr) : ptr(ptr) { }

  public:
    bool operator==(const Base& rhs) const {
        // Some compilers will indeed place objects of different types at
        // the same address, so technically, we should include the vtable
        // in this comparison. But it seems unlikely to cause problems in
        // practice.
        return ptr == rhs.ptr;
    }
    bool operator!=(const Base& rhs) const { return !(*this == rhs); }

    // Return a human-readable name for the referent's type. The result should
    // be statically allocated. (You can use MOZ_UTF16("strings") for this.)
    //
    // This must always return Concrete<T>::concreteTypeName; we use that
    // pointer as a tag for this particular referent type.
    virtual const char16_t* typeName() const = 0;

    // Return the size of this node, in bytes. Include any structures that this
    // node owns exclusively that are not exposed as their own ubi::Nodes.
    // |mallocSizeOf| should be a malloc block sizing function; see
    // |mfbt/MemoryReporting.h.
    virtual size_t size(mozilla::MallocSizeOf mallocSizeof) const { return 0; }

    // Return an EdgeRange that initially contains all the referent's outgoing
    // edges. The EdgeRange should be freed with 'js_delete'. (You could use
    // ScopedDJSeletePtr<EdgeRange> to manage it.) On OOM, report an exception
    // on |cx| and return nullptr.
    //
    // If wantNames is true, compute names for edges. Doing so can be expensive
    // in time and memory.
    virtual EdgeRange* edges(JSContext* cx, bool wantNames) const = 0;

    // Return the Zone to which this node's referent belongs, or nullptr if the
    // referent is not of a type allocated in SpiderMonkey Zones.
    virtual JS::Zone* zone() const { return nullptr; }

    // Return the compartment for this node. Some ubi::Node referents are not
    // associated with JSCompartments, such as JSStrings (which are associated
    // with Zones). When the referent is not associated with a compartment,
    // nullptr is returned.
    virtual JSCompartment* compartment() const { return nullptr; }

  private:
    Base(const Base& rhs) = delete;
    Base& operator=(const Base& rhs) = delete;
};

// A traits template with a specialization for each referent type that
// ubi::Node supports. The specialization must be the concrete subclass of
// Base that represents a pointer to the referent type. It must also
// include the members described here.
template<typename Referent>
struct Concrete {
    // The specific char16_t array returned by Concrete<T>::typeName.
    static const char16_t concreteTypeName[];

    // Construct an instance of this concrete class in |storage| referring
    // to |referent|. Implementations typically use a placement 'new'.
    //
    // In some cases, |referent| will contain dynamic type information that
    // identifies it a some more specific subclass of |Referent|. For example,
    // when |Referent| is |JSObject|, then |referent->getClass()| could tell us
    // that it's actually a JSFunction. Similarly, if |Referent| is
    // |nsISupports|, we would like a ubi::Node that knows its final
    // implementation type.
    //
    // So, we delegate the actual construction to this specialization, which
    // knows Referent's details.
    static void construct(void* storage, Referent* referent);
};

// A container for a Base instance; all members simply forward to the contained instance.
// This container allows us to pass ubi::Node instances by value.
class Node {
    // Storage in which we allocate Base subclasses.
    mozilla::AlignedStorage2<Base> storage;
    Base* base() { return storage.addr(); }
    const Base* base() const { return storage.addr(); }

    template<typename T>
    void construct(T* ptr) {
        static_assert(sizeof(Concrete<T>) == sizeof(*base()),
                      "ubi::Base specializations must be the same size as ubi::Base");
        Concrete<T>::construct(base(), ptr);
    }

  public:
    Node() { construct<void>(nullptr); }

    template<typename T>
    Node(T* ptr) {
        construct(ptr);
    }
    template<typename T>
    Node& operator=(T* ptr) {
        construct(ptr);
        return *this;
    }

    // We can construct and assign from rooted forms of pointers.
    template<typename T>
    Node(const Rooted<T*>& root) {
        construct(root.get());
    }
    template<typename T>
    Node& operator=(const Rooted<T*>& root) {
        construct(root.get());
        return *this;
    }

    // Constructors accepting SpiderMonkey's other generic-pointer-ish types.
    // Note that we *do* want an implicit constructor here: JS::Value and
    // JS::ubi::Node are both essentially tagged references to other sorts of
    // objects, so letting conversions happen automatically is appropriate.
    MOZ_IMPLICIT Node(JS::HandleValue value);
    Node(JSGCTraceKind kind, void* ptr);

    // copy construction and copy assignment just use memcpy, since we know
    // instances contain nothing but a vtable pointer and a data pointer.
    //
    // To be completely correct, concrete classes could provide a virtual
    // 'construct' member function, which we could invoke on rhs to construct an
    // instance in our storage. But this is good enough; there's no need to jump
    // through vtables for copying and assignment that are just going to move
    // two words around. The compiler knows how to optimize memcpy.
    Node(const Node& rhs) {
        memcpy(storage.u.mBytes, rhs.storage.u.mBytes, sizeof(storage.u));
    }

    Node& operator=(const Node& rhs) {
        memcpy(storage.u.mBytes, rhs.storage.u.mBytes, sizeof(storage.u));
        return *this;
    }

    bool operator==(const Node& rhs) const { return *base() == *rhs.base(); }
    bool operator!=(const Node& rhs) const { return *base() != *rhs.base(); }

    explicit operator bool() const {
        return base()->ptr != nullptr;
    }

    template<typename T>
    bool is() const {
        return base()->typeName() == Concrete<T>::concreteTypeName;
    }

    template<typename T>
    T* as() const {
        MOZ_ASSERT(is<T>());
        return static_cast<T*>(base()->ptr);
    }

    template<typename T>
    T* asOrNull() const {
        return is<T>() ? static_cast<T*>(base()->ptr) : nullptr;
    }

    // If this node refers to something that can be represented as a JavaScript
    // value that is safe to expose to JavaScript code, return that value.
    // Otherwise return UndefinedValue(). JSStrings, JS::Symbols, and some (but
    // not all!) JSObjects can be exposed.
    JS::Value exposeToJS() const;

    const char16_t* typeName()      const { return base()->typeName(); }
    JS::Zone* zone()                const { return base()->zone(); }
    JSCompartment* compartment()    const { return base()->compartment(); }

    size_t size(mozilla::MallocSizeOf mallocSizeof) const {
        return base()->size(mallocSizeof);
    }

    EdgeRange* edges(JSContext* cx, bool wantNames = true) const {
        return base()->edges(cx, wantNames);
    }

    // A hash policy for ubi::Nodes.
    // This simply uses the stock PointerHasher on the ubi::Node's pointer.
    // We specialize DefaultHasher below to make this the default.
    class HashPolicy {
        typedef js::PointerHasher<void*, mozilla::tl::FloorLog2<sizeof(void*)>::value> PtrHash;

      public:
        typedef Node Lookup;

        static js::HashNumber hash(const Lookup& l) { return PtrHash::hash(l.base()->ptr); }
        static bool match(const Node& k, const Lookup& l) { return k == l; }
        static void rekey(Node& k, const Node& newKey) { k = newKey; }
    };
};


// Edge is the abstract base class representing an outgoing edge of a node.
// Edges are owned by EdgeRanges, and need not have assignment operators or copy
// constructors.
//
// Each Edge class should inherit from this base class, overriding as
// appropriate.
class Edge {
  protected:
    Edge() : name(nullptr), referent() { }
    virtual ~Edge() { }

  public:
    // This edge's name. This may be nullptr, if Node::edges was called with
    // false as the wantNames parameter.
    //
    // The storage is owned by this Edge, and will be freed when this Edge is
    // destructed.
    //
    // (In real life we'll want a better representation for names, to avoid
    // creating tons of strings when the names follow a pattern; and we'll need
    // to think about lifetimes carefully to ensure traversal stays cheap.)
    const char16_t* name;

    // This edge's referent.
    Node referent;

  private:
    Edge(const Edge&) = delete;
    Edge& operator=(const Edge&) = delete;
};


// EdgeRange is an abstract base class for iterating over a node's outgoing
// edges. (This is modeled after js::HashTable<K,V>::Range.)
//
// Concrete instances of this class need not be as lightweight as Node itself,
// since they're usually only instantiated while iterating over a particular
// object's edges. For example, a dumb implementation for JS Cells might use
// JS_TraceChildren to to get the outgoing edges, and then store them in an
// array internal to the EdgeRange.
class EdgeRange {
  protected:
    // The current front edge of this range, or nullptr if this range is empty.
    Edge* front_;

    EdgeRange() : front_(nullptr) { }

  public:
    virtual ~EdgeRange() { }

    // True if there are no more edges in this range.
    bool empty() const { return !front_; }

    // The front edge of this range. This is owned by the EdgeRange, and is
    // only guaranteed to live until the next call to popFront, or until
    // the EdgeRange is destructed.
    const Edge& front() { return *front_; }

    // Remove the front edge from this range. This should only be called if
    // !empty().
    virtual void popFront() = 0;

  private:
    EdgeRange(const EdgeRange&) = delete;
    EdgeRange& operator=(const EdgeRange&) = delete;
};


// A dumb Edge concrete class. All but the most essential members have the
// default behavior.
class SimpleEdge : public Edge {
    SimpleEdge(SimpleEdge&) = delete;
    SimpleEdge& operator=(const SimpleEdge&) = delete;

  public:
    SimpleEdge() : Edge() { }

    // Construct an initialized SimpleEdge, taking ownership of |name|.
    SimpleEdge(char16_t* name, const Node& referent) {
        this->name = name;
        this->referent = referent;
    }
    ~SimpleEdge() {
        js_free(const_cast<char16_t*>(name));
    }

    // Move construction and assignment.
    SimpleEdge(SimpleEdge&& rhs) {
        name = rhs.name;
        referent = rhs.referent;

        rhs.name = nullptr;
    }
    SimpleEdge& operator=(SimpleEdge&& rhs) {
        MOZ_ASSERT(&rhs != this);
        this->~SimpleEdge();
        new(this) SimpleEdge(mozilla::Move(rhs));
        return *this;
    }
};

typedef mozilla::Vector<SimpleEdge, 8, js::TempAllocPolicy> SimpleEdgeVector;


// RootList is a class that can be pointed to by a |ubi::Node|, creating a
// fictional root-of-roots which has edges to every GC root in the JS
// runtime. Having a single root |ubi::Node| is useful for algorithms written
// with the assumption that there aren't multiple roots (such as computing
// dominator trees) and you want a single point of entry. It also ensures that
// the roots themselves get visited by |ubi::BreadthFirst| (they would otherwise
// only be used as starting points).
//
// RootList::init itself causes a minor collection, but once the list of roots
// has been created, GC must not occur, as the referent ubi::Nodes are not
// stable across GC. The init calls emplace on |noGC|'s AutoCheckCannotGC, whose
// lifetime must extend at least as long as the RootList itself.
//
// Example usage:
//
//    {
//        mozilla::Maybe<JS::AutoCheckCannotGC> maybeNoGC;
//        JS::ubi::RootList rootList(cx, maybeNoGC);
//        if (!rootList.init())
//            return false;
//
//        // The AutoCheckCannotGC is guaranteed to exist if init returned true.
//        MOZ_ASSERT(maybeNoGC.isSome());
//
//        JS::ubi::Node root(&rootList);
//
//        ...
//    }
class MOZ_STACK_CLASS RootList {
    Maybe<AutoCheckCannotGC>& noGC;
    JSContext*               cx;

  public:
    SimpleEdgeVector edges;
    bool             wantNames;

    RootList(JSContext* cx, Maybe<AutoCheckCannotGC>& noGC, bool wantNames = false);

    // Find all GC roots.
    bool init();
    // Find only GC roots in the provided set of |Zone|s.
    bool init(ZoneSet& debuggees);
    // Find only GC roots in the given Debugger object's set of debuggee zones.
    bool init(HandleObject debuggees);

    // Explicitly add the given Node as a root in this RootList. If wantNames is
    // true, you must pass an edgeName. The RootList does not take ownership of
    // edgeName.
    bool addRoot(Node node, const char16_t* edgeName = nullptr);
};


// Concrete classes for ubi::Node referent types.

template<>
struct Concrete<RootList> : public Base {
    EdgeRange* edges(JSContext* cx, bool wantNames) const override;
    const char16_t* typeName() const override { return concreteTypeName; }

  protected:
    explicit Concrete(RootList* ptr) : Base(ptr) { }
    RootList& get() const { return *static_cast<RootList*>(ptr); }

  public:
    static const char16_t concreteTypeName[];
    static void construct(void* storage, RootList* ptr) { new (storage) Concrete(ptr); }
};

// A reusable ubi::Concrete specialization base class for types supported by
// JS_TraceChildren.
template<typename Referent>
class TracerConcrete : public Base {
    const char16_t* typeName() const override { return concreteTypeName; }
    EdgeRange* edges(JSContext*, bool wantNames) const override;
    JS::Zone* zone() const override;

  protected:
    explicit TracerConcrete(Referent* ptr) : Base(ptr) { }
    Referent& get() const { return *static_cast<Referent*>(ptr); }

  public:
    static const char16_t concreteTypeName[];
    static void construct(void* storage, Referent* ptr) { new (storage) TracerConcrete(ptr); }
};

// For JS_TraceChildren-based types that have a 'compartment' method.
template<typename Referent>
class TracerConcreteWithCompartment : public TracerConcrete<Referent> {
    typedef TracerConcrete<Referent> TracerBase;
    JSCompartment* compartment() const override;

    explicit TracerConcreteWithCompartment(Referent* ptr) : TracerBase(ptr) { }

  public:
    static void construct(void* storage, Referent* ptr) {
        new (storage) TracerConcreteWithCompartment(ptr);
    }
};

// Define specializations for some commonly-used public JSAPI types.
template<> struct Concrete<JSObject> : TracerConcreteWithCompartment<JSObject> { };
template<> struct Concrete<JSString> : TracerConcrete<JSString> { };
template<> struct Concrete<JS::Symbol> : TracerConcrete<JS::Symbol> { };
template<> struct Concrete<JSScript> : TracerConcreteWithCompartment<JSScript> { };

// The ubi::Node null pointer. Any attempt to operate on a null ubi::Node asserts.
template<>
class Concrete<void> : public Base {
    const char16_t* typeName() const override;
    size_t size(mozilla::MallocSizeOf mallocSizeOf) const override;
    EdgeRange* edges(JSContext* cx, bool wantNames) const override;
    JS::Zone* zone() const override;
    JSCompartment* compartment() const override;

    explicit Concrete(void* ptr) : Base(ptr) { }

  public:
    static void construct(void* storage, void* ptr) { new (storage) Concrete(ptr); }
    static const char16_t concreteTypeName[];
};


} // namespace ubi
} // namespace JS

namespace js {

// Make ubi::Node::HashPolicy the default hash policy for ubi::Node.
template<> struct DefaultHasher<JS::ubi::Node> : JS::ubi::Node::HashPolicy { };

} // namespace js

#endif // js_UbiNode_h
