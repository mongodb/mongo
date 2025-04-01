/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_UbiNode_h
#define js_UbiNode_h

#include "mozilla/Alignment.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/Variant.h"
#include "mozilla/Vector.h"

#include <utility>

#include "jspubtd.h"

#include "js/AllocPolicy.h"
#include "js/ColumnNumber.h"  // JS::TaggedColumnNumberOneOrigin
#include "js/HashTable.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Value.h"

// [SMDOC] ubi::Node (Heap Analysis framework)
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
// convert a 'JSContext*' to a ubi::Node, yielding a node with an outgoing edge
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
//
//
// Hostile Graph Structure
//
// Analyses consuming ubi::Node graphs must be robust when presented with graphs
// that are deliberately constructed to exploit their weaknesses. When operating
// on live graphs, web content has control over the object graph, and less
// direct control over shape and string structure, and analyses should be
// prepared to handle extreme cases gracefully. For example, if an analysis were
// to use the C++ stack in a depth-first traversal, carefully constructed
// content could cause the analysis to overflow the stack.
//
// When ubi::Nodes refer to nodes deserialized from a heap snapshot, analyses
// must be even more careful: since snapshots often come from potentially
// compromised e10s content processes, even properties normally guaranteed by
// the platform (the proper linking of DOM nodes, for example) might be
// corrupted. While it is the deserializer's responsibility to check the basic
// structure of the snapshot file, the analyses should be prepared for ubi::Node
// graphs constructed from snapshots to be even more bizarre.

namespace js {
class BaseScript;
}  // namespace js

namespace JS {

class JS_PUBLIC_API AutoCheckCannotGC;

using ZoneSet =
    js::HashSet<Zone*, js::DefaultHasher<Zone*>, js::SystemAllocPolicy>;

using CompartmentSet =
    js::HashSet<Compartment*, js::DefaultHasher<Compartment*>,
                js::SystemAllocPolicy>;

namespace ubi {

class Edge;
class EdgeRange;
class StackFrame;

using mozilla::Maybe;
using mozilla::RangedPtr;
using mozilla::Variant;

template <typename T>
using Vector = mozilla::Vector<T, 0, js::SystemAllocPolicy>;

/*** ubi::StackFrame **********************************************************/

// Concrete JS::ubi::StackFrame instances backed by a live SavedFrame object
// store their strings as JSAtom*, while deserialized stack frames from offline
// heap snapshots store their strings as const char16_t*. In order to provide
// zero-cost accessors to these strings in a single interface that works with
// both cases, we use this variant type.
class JS_PUBLIC_API AtomOrTwoByteChars
    : public Variant<JSAtom*, const char16_t*> {
  using Base = Variant<JSAtom*, const char16_t*>;

 public:
  template <typename T>
  MOZ_IMPLICIT AtomOrTwoByteChars(T&& rhs) : Base(std::forward<T>(rhs)) {}

  template <typename T>
  AtomOrTwoByteChars& operator=(T&& rhs) {
    MOZ_ASSERT(this != &rhs, "self-move disallowed");
    this->~AtomOrTwoByteChars();
    new (this) AtomOrTwoByteChars(std::forward<T>(rhs));
    return *this;
  }

  // Return the length of the given AtomOrTwoByteChars string.
  size_t length();

  // Copy the given AtomOrTwoByteChars string into the destination buffer,
  // inflating if necessary. Does NOT null terminate. Returns the number of
  // characters written to destination.
  size_t copyToBuffer(RangedPtr<char16_t> destination, size_t length);
};

// The base class implemented by each ConcreteStackFrame<T> type. Subclasses
// must not add data members to this class.
class BaseStackFrame {
  friend class StackFrame;

  BaseStackFrame(const StackFrame&) = delete;
  BaseStackFrame& operator=(const StackFrame&) = delete;

 protected:
  void* ptr;
  explicit BaseStackFrame(void* ptr) : ptr(ptr) {}

 public:
  // This is a value type that should not have a virtual destructor. Don't add
  // destructors in subclasses!

  // Get a unique identifier for this StackFrame. The identifier is not valid
  // across garbage collections.
  virtual uint64_t identifier() const { return uint64_t(uintptr_t(ptr)); }

  // Get this frame's parent frame.
  virtual StackFrame parent() const = 0;

  // Get this frame's line number (1-origin).
  virtual uint32_t line() const = 0;

  // Get this frame's column number in UTF-16 code units.
  virtual JS::TaggedColumnNumberOneOrigin column() const = 0;

  // Get this frame's source name. Never null.
  virtual AtomOrTwoByteChars source() const = 0;

  // Get a unique per-process ID for this frame's source. Defaults to zero.
  virtual uint32_t sourceId() const = 0;

  // Return this frame's function name if named, otherwise the inferred
  // display name. Can be null.
  virtual AtomOrTwoByteChars functionDisplayName() const = 0;

  // Returns true if this frame's function is system JavaScript running with
  // trusted principals, false otherwise.
  virtual bool isSystem() const = 0;

  // Return true if this frame's function is a self-hosted JavaScript builtin,
  // false otherwise.
  virtual bool isSelfHosted(JSContext* cx) const = 0;

  // Construct a SavedFrame stack for the stack starting with this frame and
  // containing all of its parents. The SavedFrame objects will be placed into
  // cx's current compartment.
  //
  // Note that the process of
  //
  //     SavedFrame
  //         |
  //         V
  //     JS::ubi::StackFrame
  //         |
  //         V
  //     offline heap snapshot
  //         |
  //         V
  //     JS::ubi::StackFrame
  //         |
  //         V
  //     SavedFrame
  //
  // is lossy because we cannot serialize and deserialize the SavedFrame's
  // principals in the offline heap snapshot, so JS::ubi::StackFrame
  // simplifies the principals check into the boolean isSystem() state. This
  // is fine because we only expose JS::ubi::Stack to devtools and chrome
  // code, and not to the web platform.
  [[nodiscard]] virtual bool constructSavedFrameStack(
      JSContext* cx, MutableHandleObject outSavedFrameStack) const = 0;

  // Trace the concrete implementation of JS::ubi::StackFrame.
  virtual void trace(JSTracer* trc) = 0;
};

// A traits template with a specialization for each backing type that implements
// the ubi::BaseStackFrame interface. Each specialization must be the a subclass
// of ubi::BaseStackFrame.
template <typename T>
class ConcreteStackFrame;

// A JS::ubi::StackFrame represents a frame in a recorded stack. It can be
// backed either by a live SavedFrame object or by a structure deserialized from
// an offline heap snapshot.
//
// It is a value type that may be memcpy'd hither and thither without worrying
// about constructors or destructors, similar to POD types.
//
// Its lifetime is the same as the lifetime of the graph that is being analyzed
// by the JS::ubi::Node that the JS::ubi::StackFrame came from. That is, if the
// graph being analyzed is the live heap graph, the JS::ubi::StackFrame is only
// valid within the scope of an AutoCheckCannotGC; if the graph being analyzed
// is an offline heap snapshot, the JS::ubi::StackFrame is valid as long as the
// offline heap snapshot is alive.
class StackFrame {
  // Storage in which we allocate BaseStackFrame subclasses.
  mozilla::AlignedStorage2<BaseStackFrame> storage;

  BaseStackFrame* base() { return storage.addr(); }
  const BaseStackFrame* base() const { return storage.addr(); }

  template <typename T>
  void construct(T* ptr) {
    static_assert(std::is_base_of_v<BaseStackFrame, ConcreteStackFrame<T>>,
                  "ConcreteStackFrame<T> must inherit from BaseStackFrame");
    static_assert(
        sizeof(ConcreteStackFrame<T>) == sizeof(*base()),
        "ubi::ConcreteStackFrame<T> specializations must be the same size as "
        "ubi::BaseStackFrame");
    ConcreteStackFrame<T>::construct(base(), ptr);
  }
  struct ConstructFunctor;

 public:
  StackFrame() { construct<void>(nullptr); }

  template <typename T>
  MOZ_IMPLICIT StackFrame(T* ptr) {
    construct(ptr);
  }

  template <typename T>
  StackFrame& operator=(T* ptr) {
    construct(ptr);
    return *this;
  }

  // Constructors accepting SpiderMonkey's generic-pointer-ish types.

  template <typename T>
  explicit StackFrame(const JS::Handle<T*>& handle) {
    construct(handle.get());
  }

  template <typename T>
  StackFrame& operator=(const JS::Handle<T*>& handle) {
    construct(handle.get());
    return *this;
  }

  template <typename T>
  explicit StackFrame(const JS::Rooted<T*>& root) {
    construct(root.get());
  }

  template <typename T>
  StackFrame& operator=(const JS::Rooted<T*>& root) {
    construct(root.get());
    return *this;
  }

  // Because StackFrame is just a vtable pointer and an instance pointer, we
  // can memcpy everything around instead of making concrete classes define
  // virtual constructors. See the comment above Node's copy constructor for
  // more details; that comment applies here as well.
  StackFrame(const StackFrame& rhs) {
    memcpy(storage.u.mBytes, rhs.storage.u.mBytes, sizeof(storage.u));
  }

  StackFrame& operator=(const StackFrame& rhs) {
    memcpy(storage.u.mBytes, rhs.storage.u.mBytes, sizeof(storage.u));
    return *this;
  }

  bool operator==(const StackFrame& rhs) const {
    return base()->ptr == rhs.base()->ptr;
  }
  bool operator!=(const StackFrame& rhs) const { return !(*this == rhs); }

  explicit operator bool() const { return base()->ptr != nullptr; }

  // Copy this StackFrame's source name into the given |destination|
  // buffer. Copy no more than |length| characters. The result is *not* null
  // terminated. Returns how many characters were written into the buffer.
  size_t source(RangedPtr<char16_t> destination, size_t length) const;

  // Copy this StackFrame's function display name into the given |destination|
  // buffer. Copy no more than |length| characters. The result is *not* null
  // terminated. Returns how many characters were written into the buffer.
  size_t functionDisplayName(RangedPtr<char16_t> destination,
                             size_t length) const;

  // Get the size of the respective strings. 0 is returned for null strings.
  size_t sourceLength();
  size_t functionDisplayNameLength();

  // Methods that forward to virtual calls through BaseStackFrame.

  void trace(JSTracer* trc) { base()->trace(trc); }
  uint64_t identifier() const {
    auto id = base()->identifier();
    MOZ_ASSERT(JS::Value::isNumberRepresentable(id));
    return id;
  }
  uint32_t line() const { return base()->line(); }
  JS::TaggedColumnNumberOneOrigin column() const { return base()->column(); }
  AtomOrTwoByteChars source() const { return base()->source(); }
  uint32_t sourceId() const { return base()->sourceId(); }
  AtomOrTwoByteChars functionDisplayName() const {
    return base()->functionDisplayName();
  }
  StackFrame parent() const { return base()->parent(); }
  bool isSystem() const { return base()->isSystem(); }
  bool isSelfHosted(JSContext* cx) const { return base()->isSelfHosted(cx); }
  [[nodiscard]] bool constructSavedFrameStack(
      JSContext* cx, MutableHandleObject outSavedFrameStack) const {
    return base()->constructSavedFrameStack(cx, outSavedFrameStack);
  }

  struct HashPolicy {
    using Lookup = JS::ubi::StackFrame;

    static js::HashNumber hash(const Lookup& lookup) {
      return mozilla::HashGeneric(lookup.identifier());
    }

    static bool match(const StackFrame& key, const Lookup& lookup) {
      return key == lookup;
    }

    static void rekey(StackFrame& k, const StackFrame& newKey) { k = newKey; }
  };
};

// The ubi::StackFrame null pointer. Any attempt to operate on a null
// ubi::StackFrame crashes.
template <>
class ConcreteStackFrame<void> : public BaseStackFrame {
  explicit ConcreteStackFrame(void* ptr) : BaseStackFrame(ptr) {}

 public:
  static void construct(void* storage, void*) {
    new (storage) ConcreteStackFrame(nullptr);
  }

  uint64_t identifier() const override { return 0; }
  void trace(JSTracer* trc) override {}
  [[nodiscard]] bool constructSavedFrameStack(
      JSContext* cx, MutableHandleObject out) const override {
    out.set(nullptr);
    return true;
  }

  uint32_t line() const override { MOZ_CRASH("null JS::ubi::StackFrame"); }
  JS::TaggedColumnNumberOneOrigin column() const override {
    MOZ_CRASH("null JS::ubi::StackFrame");
  }
  AtomOrTwoByteChars source() const override {
    MOZ_CRASH("null JS::ubi::StackFrame");
  }
  uint32_t sourceId() const override { MOZ_CRASH("null JS::ubi::StackFrame"); }
  AtomOrTwoByteChars functionDisplayName() const override {
    MOZ_CRASH("null JS::ubi::StackFrame");
  }
  StackFrame parent() const override { MOZ_CRASH("null JS::ubi::StackFrame"); }
  bool isSystem() const override { MOZ_CRASH("null JS::ubi::StackFrame"); }
  bool isSelfHosted(JSContext* cx) const override {
    MOZ_CRASH("null JS::ubi::StackFrame");
  }
};

[[nodiscard]] JS_PUBLIC_API bool ConstructSavedFrameStackSlow(
    JSContext* cx, JS::ubi::StackFrame& frame,
    MutableHandleObject outSavedFrameStack);

/*** ubi::Node
 * ************************************************************************************/

// A concrete node specialization can claim its referent is a member of a
// particular "coarse type" which is less specific than the actual
// implementation type but generally more palatable for web developers. For
// example, JitCode can be considered to have a coarse type of "Script". This is
// used by some analyses for putting nodes into different buckets. The default,
// if a concrete specialization does not provide its own mapping to a CoarseType
// variant, is "Other".
//
// NB: the values associated with a particular enum variant must not change or
// be reused for new variants. Doing so will cause inspecting ubi::Nodes backed
// by an offline heap snapshot from an older SpiderMonkey/Firefox version to
// break. Consider this enum append only.
enum class CoarseType : uint32_t {
  Other = 0,
  Object = 1,
  Script = 2,
  String = 3,
  DOMNode = 4,

  FIRST = Other,
  LAST = DOMNode
};

/**
 * Convert a CoarseType enum into a string. The string is statically allocated.
 */
JS_PUBLIC_API const char* CoarseTypeToString(CoarseType type);

inline uint32_t CoarseTypeToUint32(CoarseType type) {
  return static_cast<uint32_t>(type);
}

inline bool Uint32IsValidCoarseType(uint32_t n) {
  auto first = static_cast<uint32_t>(CoarseType::FIRST);
  auto last = static_cast<uint32_t>(CoarseType::LAST);
  MOZ_ASSERT(first < last);
  return first <= n && n <= last;
}

inline CoarseType Uint32ToCoarseType(uint32_t n) {
  MOZ_ASSERT(Uint32IsValidCoarseType(n));
  return static_cast<CoarseType>(n);
}

// The base class implemented by each ubi::Node referent type. Subclasses must
// not add data members to this class.
class JS_PUBLIC_API Base {
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

  explicit Base(void* ptr) : ptr(ptr) {}

 public:
  bool operator==(const Base& rhs) const {
    // Some compilers will indeed place objects of different types at
    // the same address, so technically, we should include the vtable
    // in this comparison. But it seems unlikely to cause problems in
    // practice.
    return ptr == rhs.ptr;
  }
  bool operator!=(const Base& rhs) const { return !(*this == rhs); }

  // An identifier for this node, guaranteed to be stable and unique for as
  // long as this ubi::Node's referent is alive and at the same address.
  //
  // This is probably suitable for use in serializations, as it is an integral
  // type. It may also help save memory when constructing HashSets of
  // ubi::Nodes: since a uint64_t will always be smaller-or-equal-to the size
  // of a ubi::Node, a HashSet<ubi::Node::Id> may use less space per element
  // than a HashSet<ubi::Node>.
  //
  // (Note that 'unique' only means 'up to equality on ubi::Node'; see the
  // caveats about multiple objects allocated at the same address for
  // 'ubi::Node::operator=='.)
  using Id = uint64_t;
  virtual Id identifier() const { return Id(uintptr_t(ptr)); }

  // Returns true if this node is pointing to something on the live heap, as
  // opposed to something from a deserialized core dump. Returns false,
  // otherwise.
  virtual bool isLive() const { return true; };

  // Return the coarse-grained type-of-thing that this node represents.
  virtual CoarseType coarseType() const { return CoarseType::Other; }

  // Return a human-readable name for the referent's type. The result should
  // be statically allocated. (You can use u"strings" for this.)
  //
  // This must always return Concrete<T>::concreteTypeName; we use that
  // pointer as a tag for this particular referent type.
  virtual const char16_t* typeName() const = 0;

  // Return the size of this node, in bytes. Include any structures that this
  // node owns exclusively that are not exposed as their own ubi::Nodes.
  // |mallocSizeOf| should be a malloc block sizing function; see
  // |mfbt/MemoryReporting.h|.
  //
  // Because we can use |JS::ubi::Node|s backed by a snapshot that was taken
  // on a 64-bit platform when we are currently on a 32-bit platform, we
  // cannot rely on |size_t| for node sizes. Instead, |Size| is uint64_t on
  // all platforms.
  using Size = uint64_t;
  virtual Size size(mozilla::MallocSizeOf mallocSizeof) const { return 1; }

  // Return an EdgeRange that initially contains all the referent's outgoing
  // edges. The caller takes ownership of the EdgeRange.
  //
  // If wantNames is true, compute names for edges. Doing so can be expensive
  // in time and memory.
  virtual js::UniquePtr<EdgeRange> edges(JSContext* cx,
                                         bool wantNames) const = 0;

  // Return the Zone to which this node's referent belongs, or nullptr if the
  // referent is not of a type allocated in SpiderMonkey Zones.
  virtual JS::Zone* zone() const { return nullptr; }

  // Return the compartment for this node. Some ubi::Node referents are not
  // associated with Compartments, such as JSStrings (which are associated
  // with Zones). When the referent is not associated with a compartment,
  // nullptr is returned.
  virtual JS::Compartment* compartment() const { return nullptr; }

  // Return the realm for this node. Some ubi::Node referents are not
  // associated with Realms, such as JSStrings (which are associated
  // with Zones) or cross-compartment wrappers (which are associated with
  // compartments). When the referent is not associated with a realm,
  // nullptr is returned.
  virtual JS::Realm* realm() const { return nullptr; }

  // Return whether this node's referent's allocation stack was captured.
  virtual bool hasAllocationStack() const { return false; }

  // Get the stack recorded at the time this node's referent was
  // allocated. This must only be called when hasAllocationStack() is true.
  virtual StackFrame allocationStack() const {
    MOZ_CRASH(
        "Concrete classes that have an allocation stack must override both "
        "hasAllocationStack and allocationStack.");
  }

  // In some cases, Concrete<T> can return a more descriptive
  // referent type name than simply `T`. This method returns an
  // identifier as specific as is efficiently available.
  // The string returned is borrowed from the ubi::Node's referent.
  // If nothing more specific than typeName() is available, return nullptr.
  virtual const char16_t* descriptiveTypeName() const { return nullptr; }

  // Methods for JSObject Referents
  //
  // These methods are only semantically valid if the referent is either a
  // JSObject in the live heap, or represents a previously existing JSObject
  // from some deserialized heap snapshot.

  // Return the object's [[Class]]'s name.
  virtual const char* jsObjectClassName() const { return nullptr; }

  // Methods for CoarseType::Script referents

  // Return the script's source's filename if available. If unavailable,
  // return nullptr.
  virtual const char* scriptFilename() const { return nullptr; }

 private:
  Base(const Base& rhs) = delete;
  Base& operator=(const Base& rhs) = delete;
};

// A traits template with a specialization for each referent type that
// ubi::Node supports. The specialization must be the concrete subclass of Base
// that represents a pointer to the referent type. It must include these
// members:
//
//    // The specific char16_t array returned by Concrete<T>::typeName().
//    static const char16_t concreteTypeName[];
//
//    // Construct an instance of this concrete class in |storage| referring
//    // to |referent|. Implementations typically use a placement 'new'.
//    //
//    // In some cases, |referent| will contain dynamic type information that
//    // identifies it a some more specific subclass of |Referent|. For
//    // example, when |Referent| is |JSObject|, then |referent->getClass()|
//    // could tell us that it's actually a JSFunction. Similarly, if
//    // |Referent| is |nsISupports|, we would like a ubi::Node that knows its
//    // final implementation type.
//    //
//    // So we delegate the actual construction to this specialization, which
//    // knows Referent's details.
//    static void construct(void* storage, Referent* referent);
template <typename Referent>
class Concrete;

// A container for a Base instance; all members simply forward to the contained
// instance.  This container allows us to pass ubi::Node instances by value.
class Node {
  // Storage in which we allocate Base subclasses.
  mozilla::AlignedStorage2<Base> storage;
  Base* base() { return storage.addr(); }
  const Base* base() const { return storage.addr(); }

  template <typename T>
  void construct(T* ptr) {
    static_assert(
        sizeof(Concrete<T>) == sizeof(*base()),
        "ubi::Base specializations must be the same size as ubi::Base");
    static_assert(std::is_base_of_v<Base, Concrete<T>>,
                  "ubi::Concrete<T> must inherit from ubi::Base");
    Concrete<T>::construct(base(), ptr);
  }
  struct ConstructFunctor;

 public:
  Node() { construct<void>(nullptr); }

  template <typename T>
  MOZ_IMPLICIT Node(T* ptr) {
    construct(ptr);
  }
  template <typename T>
  Node& operator=(T* ptr) {
    construct(ptr);
    return *this;
  }

  // We can construct and assign from rooted forms of pointers.
  template <typename T>
  MOZ_IMPLICIT Node(const Rooted<T*>& root) {
    construct(root.get());
  }
  template <typename T>
  Node& operator=(const Rooted<T*>& root) {
    construct(root.get());
    return *this;
  }

  // Constructors accepting SpiderMonkey's other generic-pointer-ish types.
  // Note that we *do* want an implicit constructor here: JS::Value and
  // JS::ubi::Node are both essentially tagged references to other sorts of
  // objects, so letting conversions happen automatically is appropriate.
  MOZ_IMPLICIT Node(JS::HandleValue value);
  explicit Node(JS::GCCellPtr thing);

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

  explicit operator bool() const { return base()->ptr != nullptr; }

  bool isLive() const { return base()->isLive(); }

  // Get the canonical type name for the given type T.
  template <typename T>
  static const char16_t* canonicalTypeName() {
    return Concrete<T>::concreteTypeName;
  }

  template <typename T>
  bool is() const {
    return base()->typeName() == canonicalTypeName<T>();
  }

  template <typename T>
  T* as() const {
    MOZ_ASSERT(isLive());
    MOZ_ASSERT(this->is<T>());
    return static_cast<T*>(base()->ptr);
  }

  template <typename T>
  T* asOrNull() const {
    MOZ_ASSERT(isLive());
    return this->is<T>() ? static_cast<T*>(base()->ptr) : nullptr;
  }

  // If this node refers to something that can be represented as a JavaScript
  // value that is safe to expose to JavaScript code, return that value.
  // Otherwise return UndefinedValue(). JSStrings, JS::Symbols, and some (but
  // not all!) JSObjects can be exposed.
  JS::Value exposeToJS() const;

  CoarseType coarseType() const { return base()->coarseType(); }
  const char16_t* typeName() const { return base()->typeName(); }
  JS::Zone* zone() const { return base()->zone(); }
  JS::Compartment* compartment() const { return base()->compartment(); }
  JS::Realm* realm() const { return base()->realm(); }
  const char* jsObjectClassName() const { return base()->jsObjectClassName(); }
  const char16_t* descriptiveTypeName() const {
    return base()->descriptiveTypeName();
  }

  const char* scriptFilename() const { return base()->scriptFilename(); }

  using Size = Base::Size;
  Size size(mozilla::MallocSizeOf mallocSizeof) const {
    auto size = base()->size(mallocSizeof);
    MOZ_ASSERT(
        size > 0,
        "C++ does not have zero-sized types! Choose 1 if you just need a "
        "conservative default.");
    return size;
  }

  js::UniquePtr<EdgeRange> edges(JSContext* cx, bool wantNames = true) const {
    return base()->edges(cx, wantNames);
  }

  bool hasAllocationStack() const { return base()->hasAllocationStack(); }
  StackFrame allocationStack() const { return base()->allocationStack(); }

  using Id = Base::Id;
  Id identifier() const {
    auto id = base()->identifier();
    MOZ_ASSERT(JS::Value::isNumberRepresentable(id));
    return id;
  }

  // A hash policy for ubi::Nodes.
  // This simply uses the stock PointerHasher on the ubi::Node's pointer.
  // We specialize DefaultHasher below to make this the default.
  class HashPolicy {
    typedef js::PointerHasher<void*> PtrHash;

   public:
    typedef Node Lookup;

    static js::HashNumber hash(const Lookup& l) {
      return PtrHash::hash(l.base()->ptr);
    }
    static bool match(const Node& k, const Lookup& l) { return k == l; }
    static void rekey(Node& k, const Node& newKey) { k = newKey; }
  };
};

using NodeSet =
    js::HashSet<Node, js::DefaultHasher<Node>, js::SystemAllocPolicy>;
using NodeSetPtr = mozilla::UniquePtr<NodeSet, JS::DeletePolicy<NodeSet>>;

/*** Edge and EdgeRange *******************************************************/

using EdgeName = UniqueTwoByteChars;

// An outgoing edge to a referent node.
class Edge {
 public:
  Edge() = default;

  // Construct an initialized Edge, taking ownership of |name|.
  Edge(char16_t* name, const Node& referent) : name(name), referent(referent) {}

  // Move construction and assignment.
  Edge(Edge&& rhs) : name(std::move(rhs.name)), referent(rhs.referent) {}

  Edge& operator=(Edge&& rhs) {
    MOZ_ASSERT(&rhs != this);
    this->~Edge();
    new (this) Edge(std::move(rhs));
    return *this;
  }

  Edge(const Edge&) = delete;
  Edge& operator=(const Edge&) = delete;

  // This edge's name. This may be nullptr, if Node::edges was called with
  // false as the wantNames parameter.
  //
  // The storage is owned by this Edge, and will be freed when this Edge is
  // destructed. You may take ownership of the name by `std::move`ing it
  // out of the edge; it is just a UniquePtr.
  //
  // (In real life we'll want a better representation for names, to avoid
  // creating tons of strings when the names follow a pattern; and we'll need
  // to think about lifetimes carefully to ensure traversal stays cheap.)
  EdgeName name = nullptr;

  // This edge's referent.
  Node referent;
};

// EdgeRange is an abstract base class for iterating over a node's outgoing
// edges. (This is modeled after js::HashTable<K,V>::Range.)
//
// Concrete instances of this class need not be as lightweight as Node itself,
// since they're usually only instantiated while iterating over a particular
// object's edges. For example, a dumb implementation for JS Cells might use
// JS::TraceChildren to to get the outgoing edges, and then store them in an
// array internal to the EdgeRange.
class EdgeRange {
 protected:
  // The current front edge of this range, or nullptr if this range is empty.
  Edge* front_;

  EdgeRange() : front_(nullptr) {}

 public:
  virtual ~EdgeRange() = default;

  // True if there are no more edges in this range.
  bool empty() const { return !front_; }

  // The front edge of this range. This is owned by the EdgeRange, and is
  // only guaranteed to live until the next call to popFront, or until
  // the EdgeRange is destructed.
  const Edge& front() const { return *front_; }
  Edge& front() { return *front_; }

  // Remove the front edge from this range. This should only be called if
  // !empty().
  virtual void popFront() = 0;

 private:
  EdgeRange(const EdgeRange&) = delete;
  EdgeRange& operator=(const EdgeRange&) = delete;
};

typedef mozilla::Vector<Edge, 8, js::SystemAllocPolicy> EdgeVector;

// An EdgeRange concrete class that holds a pre-existing vector of
// Edges. A PreComputedEdgeRange does not take ownership of its
// EdgeVector; it is up to the PreComputedEdgeRange's consumer to manage
// that lifetime.
class PreComputedEdgeRange : public EdgeRange {
  EdgeVector& edges;
  size_t i;

  void settle() { front_ = i < edges.length() ? &edges[i] : nullptr; }

 public:
  explicit PreComputedEdgeRange(EdgeVector& edges) : edges(edges), i(0) {
    settle();
  }

  void popFront() override {
    MOZ_ASSERT(!empty());
    i++;
    settle();
  }
};

/*** RootList *****************************************************************/

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
// stable across GC. It returns a [[nodiscard]] AutoCheckCannotGC token in order
// to enforce this. The token's lifetime must extend at least as long as the
// RootList itself. Note that the RootList does not itself contain a nogc field,
// which means that it is possible to store it somewhere that it can escape
// the init()'s nogc scope. Don't do that. (Or you could call some function
// and pass in the RootList and GC, but that would be caught.)
//
// Example usage:
//
//    {
//        JS::ubi::RootList rootList(cx);
//        auto [ok, nogc] = rootList.init();
//        if (!ok()) {
//            return false;
//        }
//
//        JS::ubi::Node root(&rootList);
//
//        ...
//    }
class MOZ_STACK_CLASS JS_PUBLIC_API RootList {
 public:
  JSContext* cx;
  EdgeVector edges;
  bool wantNames;
  bool inited;

  explicit RootList(JSContext* cx, bool wantNames = false);

  // Find all GC roots.
  [[nodiscard]] std::pair<bool, JS::AutoCheckCannotGC> init();
  // Find only GC roots in the provided set of |JS::Compartment|s. Note: it's
  // important to take a CompartmentSet and not a RealmSet: objects in
  // same-compartment realms can reference each other directly, without going
  // through CCWs, so if we used a RealmSet here we would miss edges.
  [[nodiscard]] std::pair<bool, JS::AutoCheckCannotGC> init(
      CompartmentSet& debuggees);
  // Find only GC roots in the given Debugger object's set of debuggee
  // compartments.
  [[nodiscard]] std::pair<bool, JS::AutoCheckCannotGC> init(
      HandleObject debuggees);

  // Returns true if the RootList has been initialized successfully, false
  // otherwise.
  bool initialized() { return inited; }

  // Explicitly add the given Node as a root in this RootList. If wantNames is
  // true, you must pass an edgeName. The RootList does not take ownership of
  // edgeName.
  [[nodiscard]] bool addRoot(Node node, const char16_t* edgeName = nullptr);
};

/*** Concrete classes for ubi::Node referent types ****************************/

template <>
class JS_PUBLIC_API Concrete<RootList> : public Base {
 protected:
  explicit Concrete(RootList* ptr) : Base(ptr) {}
  RootList& get() const { return *static_cast<RootList*>(ptr); }

 public:
  static void construct(void* storage, RootList* ptr) {
    new (storage) Concrete(ptr);
  }

  js::UniquePtr<EdgeRange> edges(JSContext* cx, bool wantNames) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

// A reusable ubi::Concrete specialization base class for types supported by
// JS::TraceChildren.
template <typename Referent>
class JS_PUBLIC_API TracerConcrete : public Base {
  JS::Zone* zone() const override;

 public:
  js::UniquePtr<EdgeRange> edges(JSContext* cx, bool wantNames) const override;

 protected:
  explicit TracerConcrete(Referent* ptr) : Base(ptr) {}
  Referent& get() const { return *static_cast<Referent*>(ptr); }
};

// For JS::TraceChildren-based types that have 'realm' and 'compartment'
// methods.
template <typename Referent>
class JS_PUBLIC_API TracerConcreteWithRealm : public TracerConcrete<Referent> {
  typedef TracerConcrete<Referent> TracerBase;
  JS::Compartment* compartment() const override;
  JS::Realm* realm() const override;

 protected:
  explicit TracerConcreteWithRealm(Referent* ptr) : TracerBase(ptr) {}
};

// Define specializations for some commonly-used public JSAPI types.
// These can use the generic templates above.
template <>
class JS_PUBLIC_API Concrete<JS::Symbol> : TracerConcrete<JS::Symbol> {
 protected:
  explicit Concrete(JS::Symbol* ptr) : TracerConcrete(ptr) {}

 public:
  static void construct(void* storage, JS::Symbol* ptr) {
    new (storage) Concrete(ptr);
  }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

template <>
class JS_PUBLIC_API Concrete<JS::BigInt> : TracerConcrete<JS::BigInt> {
 protected:
  explicit Concrete(JS::BigInt* ptr) : TracerConcrete(ptr) {}

 public:
  static void construct(void* storage, JS::BigInt* ptr) {
    new (storage) Concrete(ptr);
  }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

template <>
class JS_PUBLIC_API Concrete<js::BaseScript>
    : TracerConcreteWithRealm<js::BaseScript> {
 protected:
  explicit Concrete(js::BaseScript* ptr)
      : TracerConcreteWithRealm<js::BaseScript>(ptr) {}

 public:
  static void construct(void* storage, js::BaseScript* ptr) {
    new (storage) Concrete(ptr);
  }

  CoarseType coarseType() const final { return CoarseType::Script; }
  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;
  const char* scriptFilename() const final;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

// The JSObject specialization.
template <>
class JS_PUBLIC_API Concrete<JSObject> : public TracerConcrete<JSObject> {
 protected:
  explicit Concrete(JSObject* ptr) : TracerConcrete<JSObject>(ptr) {}

 public:
  static void construct(void* storage, JSObject* ptr);

  JS::Compartment* compartment() const override;
  JS::Realm* realm() const override;

  const char* jsObjectClassName() const override;
  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  bool hasAllocationStack() const override;
  StackFrame allocationStack() const override;

  CoarseType coarseType() const final { return CoarseType::Object; }

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

// For JSString, we extend the generic template with a 'size' implementation.
template <>
class JS_PUBLIC_API Concrete<JSString> : TracerConcrete<JSString> {
 protected:
  explicit Concrete(JSString* ptr) : TracerConcrete<JSString>(ptr) {}

 public:
  static void construct(void* storage, JSString* ptr) {
    new (storage) Concrete(ptr);
  }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  CoarseType coarseType() const final { return CoarseType::String; }

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

// The ubi::Node null pointer. Any attempt to operate on a null ubi::Node
// asserts.
template <>
class JS_PUBLIC_API Concrete<void> : public Base {
  const char16_t* typeName() const override;
  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;
  js::UniquePtr<EdgeRange> edges(JSContext* cx, bool wantNames) const override;
  JS::Zone* zone() const override;
  JS::Compartment* compartment() const override;
  JS::Realm* realm() const override;
  CoarseType coarseType() const final;

  explicit Concrete(void* ptr) : Base(ptr) {}

 public:
  static void construct(void* storage, void* ptr) {
    new (storage) Concrete(ptr);
  }
};

// The |callback| callback is much like the |Concrete<T>::construct| method: a
// call to |callback| should construct an instance of the most appropriate
// JS::ubi::Base subclass for |obj| in |storage|. The callback may assume that
// |obj->getClass()->isDOMClass()|, and that |storage| refers to the
// sizeof(JS::ubi::Base) bytes of space that all ubi::Base implementations
// should require.

// Set |cx|'s runtime hook for constructing ubi::Nodes for DOM classes to
// |callback|.
void SetConstructUbiNodeForDOMObjectCallback(JSContext* cx,
                                             void (*callback)(void*,
                                                              JSObject*));

}  // namespace ubi
}  // namespace JS

namespace mozilla {

// Make ubi::Node::HashPolicy the default hash policy for ubi::Node.
template <>
struct DefaultHasher<JS::ubi::Node> : JS::ubi::Node::HashPolicy {};
template <>
struct DefaultHasher<JS::ubi::StackFrame> : JS::ubi::StackFrame::HashPolicy {};

}  // namespace mozilla

#endif  // js_UbiNode_h
