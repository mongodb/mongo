/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Scope_h
#define vm_Scope_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_ASSERT_IF
#include "mozilla/Attributes.h"  // MOZ_IMPLICIT, MOZ_INIT_OUTSIDE_CTOR, MOZ_STACK_CLASS
#include "mozilla/Casting.h"          // mozilla::AssertedCast
#include "mozilla/Maybe.h"            // mozilla::Maybe
#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf
#include "mozilla/Span.h"             // mozilla::Span

#include <algorithm>    // std::fill_n
#include <stddef.h>     // size_t
#include <stdint.h>     // uint8_t, uint16_t, uint32_t, uintptr_t
#include <type_traits>  // std::is_same_v, std::is_base_of_v

#include "builtin/ModuleObject.h"  // ModuleObject, Handle<ModuleObject*>
#include "frontend/ParserAtom.h"   // frontend::TaggedParserAtomIndex
#include "gc/Barrier.h"            // GCPtr
#include "gc/Cell.h"               // TenuredCellWithNonGCPointer
#include "js/GCPolicyAPI.h"        // GCPolicy, IgnoreGCPolicy
#include "js/HeapAPI.h"            // CellFlagBitsReservedForGC
#include "js/RootingAPI.h"         // Handle, MutableHandle
#include "js/TraceKind.h"          // JS::TraceKind
#include "js/TypeDecls.h"          // HandleFunction
#include "js/UbiNode.h"            // ubi::*
#include "js/UniquePtr.h"          // UniquePtr
#include "util/Poison.h"  // AlwaysPoison, JS_SCOPE_DATA_TRAILING_NAMES_PATTERN, MemCheckKind
#include "vm/JSFunction.h"  // JSFunction
#include "vm/ScopeKind.h"   // ScopeKind
#include "vm/Shape.h"       // Shape
#include "wasm/WasmJS.h"    // WasmInstanceObject

class JSAtom;
class JSScript;
class JSTracer;
struct JSContext;

namespace js {

class JS_PUBLIC_API GenericPrinter;

namespace frontend {
class ScopeStencil;
struct ScopeStencilRef;
class RuntimeScopeBindingCache;
}  // namespace frontend

template <typename NameT>
class AbstractBaseScopeData;

template <typename NameT>
class BaseAbstractBindingIter;

template <typename NameT>
class AbstractBindingIter;

template <typename NameT>
class AbstractPositionalFormalParameterIter;

using BindingIter = AbstractBindingIter<JSAtom>;

class AbstractScopePtr;

static inline bool ScopeKindIsCatch(ScopeKind kind) {
  return kind == ScopeKind::SimpleCatch || kind == ScopeKind::Catch;
}

static inline bool ScopeKindIsInBody(ScopeKind kind) {
  return kind == ScopeKind::Lexical || kind == ScopeKind::SimpleCatch ||
         kind == ScopeKind::Catch || kind == ScopeKind::With ||
         kind == ScopeKind::FunctionLexical ||
         kind == ScopeKind::FunctionBodyVar || kind == ScopeKind::ClassBody;
}

const char* BindingKindString(BindingKind kind);
const char* ScopeKindString(ScopeKind kind);

template <typename NameT>
class AbstractBindingName;

template <>
class AbstractBindingName<JSAtom> {
 public:
  using NameT = JSAtom;
  using NamePointerT = NameT*;

 private:
  // A JSAtom* with its low bit used as a tag for the:
  //  * whether it is closed over (i.e., exists in the environment shape)
  //  * whether it is a top-level function binding in global or eval scope,
  //    instead of var binding (both are in the same range in Scope data)
  uintptr_t bits_;

  static constexpr uintptr_t ClosedOverFlag = 0x1;
  // TODO: We should reuse this bit for let vs class distinction to
  //       show the better redeclaration error message (bug 1428672).
  static constexpr uintptr_t TopLevelFunctionFlag = 0x2;
  static constexpr uintptr_t FlagMask = 0x3;

 public:
  AbstractBindingName() : bits_(0) {}

  AbstractBindingName(NameT* name, bool closedOver,
                      bool isTopLevelFunction = false)
      : bits_(uintptr_t(name) | (closedOver ? ClosedOverFlag : 0x0) |
              (isTopLevelFunction ? TopLevelFunctionFlag : 0x0)) {}

  NamePointerT name() const {
    return reinterpret_cast<NameT*>(bits_ & ~FlagMask);
  }

  bool closedOver() const { return bits_ & ClosedOverFlag; }

 private:
  friend class BaseAbstractBindingIter<NameT>;

  // This method should be called only for binding names in `vars` range in
  // BindingIter.
  bool isTopLevelFunction() const { return bits_ & TopLevelFunctionFlag; }

 public:
  void trace(JSTracer* trc) {
    if (JSAtom* atom = name()) {
      TraceManuallyBarrieredEdge(trc, &atom, "binding name");
    }
  }
};

template <>
class AbstractBindingName<frontend::TaggedParserAtomIndex> {
  uint32_t bits_;

  using TaggedParserAtomIndex = frontend::TaggedParserAtomIndex;

 public:
  using NameT = TaggedParserAtomIndex;
  using NamePointerT = NameT;

 private:
  static constexpr size_t TaggedIndexBit = TaggedParserAtomIndex::IndexBit + 2;

  static constexpr size_t FlagShift = TaggedIndexBit;
  static constexpr size_t FlagBit = 2;
  static constexpr uint32_t FlagMask = BitMask(FlagBit) << FlagShift;

  static constexpr uint32_t ClosedOverFlag = 1 << FlagShift;
  static constexpr uint32_t TopLevelFunctionFlag = 2 << FlagShift;

 public:
  AbstractBindingName() : bits_(TaggedParserAtomIndex::NullTag) {
    // TaggedParserAtomIndex's tags shouldn't overlap with flags.
    static_assert((TaggedParserAtomIndex::NullTag & FlagMask) == 0);
    static_assert((TaggedParserAtomIndex::ParserAtomIndexTag & FlagMask) == 0);
    static_assert((TaggedParserAtomIndex::WellKnownTag & FlagMask) == 0);
  }

  AbstractBindingName(TaggedParserAtomIndex name, bool closedOver,
                      bool isTopLevelFunction = false)
      : bits_(name.rawData() | (closedOver ? ClosedOverFlag : 0x0) |
              (isTopLevelFunction ? TopLevelFunctionFlag : 0x0)) {}

 public:
  NamePointerT name() const {
    return TaggedParserAtomIndex::fromRaw(bits_ & ~FlagMask);
  }

  bool closedOver() const { return bits_ & ClosedOverFlag; }

  AbstractBindingName<JSAtom> copyWithNewAtom(JSAtom* newName) const {
    return AbstractBindingName<JSAtom>(newName, closedOver(),
                                       isTopLevelFunction());
  }

  void updateNameAfterStencilMerge(TaggedParserAtomIndex name) {
    bits_ = (bits_ & FlagMask) | name.rawData();
  }

 private:
  friend class BaseAbstractBindingIter<TaggedParserAtomIndex>;
  friend class frontend::ScopeStencil;

  // This method should be called only for binding names in `vars` range in
  // BindingIter.
  bool isTopLevelFunction() const { return bits_ & TopLevelFunctionFlag; }
};

using BindingName = AbstractBindingName<JSAtom>;

static inline void TraceBindingNames(JSTracer* trc, BindingName* names,
                                     uint32_t length) {
  for (uint32_t i = 0; i < length; i++) {
    JSAtom* name = names[i].name();
    MOZ_ASSERT(name);
    TraceManuallyBarrieredEdge(trc, &name, "scope name");
  }
};
static inline void TraceNullableBindingNames(JSTracer* trc, BindingName* names,
                                             uint32_t length) {
  for (uint32_t i = 0; i < length; i++) {
    if (JSAtom* name = names[i].name()) {
      TraceManuallyBarrieredEdge(trc, &name, "scope name");
    }
  }
};

const size_t ScopeDataAlignBytes = size_t(1) << gc::CellFlagBitsReservedForGC;

/**
 * Base class for scope {Runtime,Parser}Data classes to inherit from.
 *
 * `js::Scope` stores a pointer to RuntimeData classes in their first word, so
 * they must be suitably aligned to allow storing GC flags in the low bits.
 */
template <typename NameT>
class AbstractBaseScopeData {
 public:
  using NameType = NameT;

  // The length of names after specialized ScopeData subclasses.
  uint32_t length = 0;
};

template <typename ScopeDataT>
static inline void AssertDerivedScopeData() {
  static_assert(
      !std::is_same_v<ScopeDataT,
                      AbstractBaseScopeData<typename ScopeDataT::NameType>>,
      "ScopeDataT shouldn't be AbstractBaseScopeData");
  static_assert(
      std::is_base_of_v<AbstractBaseScopeData<typename ScopeDataT::NameType>,
                        ScopeDataT>,
      "ScopeDataT should be subclass of AbstractBaseScopeData");
}

template <typename ScopeDataT>
static inline size_t GetOffsetOfScopeDataTrailingNames() {
  AssertDerivedScopeData<ScopeDataT>();
  return sizeof(ScopeDataT);
}

template <typename ScopeDataT>
static inline AbstractBindingName<typename ScopeDataT::NameType>*
GetScopeDataTrailingNamesPointer(ScopeDataT* data) {
  AssertDerivedScopeData<ScopeDataT>();
  return reinterpret_cast<AbstractBindingName<typename ScopeDataT::NameType>*>(
      data + 1);
}

template <typename ScopeDataT>
static inline const AbstractBindingName<typename ScopeDataT::NameType>*
GetScopeDataTrailingNamesPointer(const ScopeDataT* data) {
  AssertDerivedScopeData<ScopeDataT>();
  return reinterpret_cast<
      const AbstractBindingName<typename ScopeDataT::NameType>*>(data + 1);
}

template <typename ScopeDataT>
static inline mozilla::Span<AbstractBindingName<typename ScopeDataT::NameType>>
GetScopeDataTrailingNames(ScopeDataT* data) {
  return mozilla::Span(GetScopeDataTrailingNamesPointer(data), data->length);
}

template <typename ScopeDataT>
static inline mozilla::Span<
    const AbstractBindingName<typename ScopeDataT::NameType>>
GetScopeDataTrailingNames(const ScopeDataT* data) {
  return mozilla::Span(GetScopeDataTrailingNamesPointer(data), data->length);
}

using BaseScopeData = AbstractBaseScopeData<JSAtom>;

inline void PoisonNames(AbstractBindingName<JSAtom>* data, uint32_t length) {
  AlwaysPoison(data, JS_SCOPE_DATA_TRAILING_NAMES_PATTERN,
               sizeof(AbstractBindingName<JSAtom>) * length,
               MemCheckKind::MakeUndefined);
}

// frontend::TaggedParserAtomIndex doesn't require poison value.
// Fill with null value instead.
inline void PoisonNames(
    AbstractBindingName<frontend::TaggedParserAtomIndex>* data,
    uint32_t length) {
  std::fill_n(data, length,
              AbstractBindingName<frontend::TaggedParserAtomIndex>());
}

template <typename ScopeDataT>
static inline void PoisonNames(ScopeDataT* data, uint32_t length) {
  if (length) {
    PoisonNames(GetScopeDataTrailingNamesPointer(data), length);
  }
}

//
// Allow using is<T> and as<T> on Rooted<Scope*> and Handle<Scope*>.
//
template <typename Wrapper>
class WrappedPtrOperations<Scope*, Wrapper> {
 public:
  template <class U>
  JS::Handle<U*> as() const {
    const Wrapper& self = *static_cast<const Wrapper*>(this);
    MOZ_ASSERT_IF(self, self->template is<U>());
    return Handle<U*>::fromMarkedLocation(
        reinterpret_cast<U* const*>(self.address()));
  }
};

//
// The base class of all Scopes.
//
class Scope : public gc::TenuredCellWithNonGCPointer<BaseScopeData> {
  friend class GCMarker;
  friend class frontend::ScopeStencil;
  friend class js::AbstractBindingIter<JSAtom>;
  friend class js::frontend::RuntimeScopeBindingCache;
  friend class gc::CellAllocator;

 protected:
  // The raw data pointer, stored in the cell header.
  BaseScopeData* rawData() { return headerPtr(); }
  const BaseScopeData* rawData() const { return headerPtr(); }

  // The kind determines data_.
  const ScopeKind kind_;

  // If there are any aliased bindings, the shape for the
  // EnvironmentObject. Otherwise nullptr.
  const GCPtr<SharedShape*> environmentShape_;

  // The enclosing scope or nullptr.
  GCPtr<Scope*> enclosingScope_;

  Scope(ScopeKind kind, Scope* enclosing, SharedShape* environmentShape)
      : TenuredCellWithNonGCPointer(nullptr),
        kind_(kind),
        environmentShape_(environmentShape),
        enclosingScope_(enclosing) {}

  static Scope* create(JSContext* cx, ScopeKind kind, Handle<Scope*> enclosing,
                       Handle<SharedShape*> envShape);

  template <typename ConcreteScope>
  void initData(
      MutableHandle<UniquePtr<typename ConcreteScope::RuntimeData>> data);

  template <typename F>
  void applyScopeDataTyped(F&& f);

  static void updateEnvShapeIfRequired(mozilla::Maybe<uint32_t>* envShape,
                                       bool needsEnvironment);

 public:
  template <typename ConcreteScope>
  static ConcreteScope* create(
      JSContext* cx, ScopeKind kind, Handle<Scope*> enclosing,
      Handle<SharedShape*> envShape,
      MutableHandle<UniquePtr<typename ConcreteScope::RuntimeData>> data);

  static const JS::TraceKind TraceKind = JS::TraceKind::Scope;

  template <typename T>
  bool is() const {
    return kind_ == T::classScopeKind_;
  }

  template <typename T>
  T& as() {
    MOZ_ASSERT(this->is<T>());
    return *static_cast<T*>(this);
  }

  template <typename T>
  const T& as() const {
    MOZ_ASSERT(this->is<T>());
    return *static_cast<const T*>(this);
  }

  ScopeKind kind() const { return kind_; }

  bool isNamedLambda() const {
    return kind() == ScopeKind::NamedLambda ||
           kind() == ScopeKind::StrictNamedLambda;
  }

  SharedShape* environmentShape() const { return environmentShape_; }

  Scope* enclosing() const { return enclosingScope_; }

  static bool hasEnvironment(ScopeKind kind, bool hasEnvironmentShape = false) {
    switch (kind) {
      case ScopeKind::With:
      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
        return true;
      default:
        // If there's a shape, an environment must be created for this scope.
        return hasEnvironmentShape;
    }
  }

  bool hasEnvironment() const {
    return hasEnvironment(kind_, !!environmentShape());
  }

  uint32_t firstFrameSlot() const;

  uint32_t chainLength() const;
  uint32_t environmentChainLength() const;

  template <typename T>
  bool hasOnChain() const {
    for (const Scope* it = this; it; it = it->enclosing()) {
      if (it->is<T>()) {
        return true;
      }
    }
    return false;
  }

  bool hasOnChain(ScopeKind kind) const {
    for (const Scope* it = this; it; it = it->enclosing()) {
      if (it->kind() == kind) {
        return true;
      }
    }
    return false;
  }

  void traceChildren(JSTracer* trc);
  void finalize(JS::GCContext* gcx);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  void dump();
#if defined(DEBUG) || defined(JS_JITSPEW)
  static bool dumpForDisassemble(JSContext* cx, JS::Handle<Scope*> scope,
                                 GenericPrinter& out, const char* indent);
#endif /* defined(DEBUG) || defined(JS_JITSPEW) */
};

template <class DataT>
inline size_t SizeOfScopeData(uint32_t length) {
  using BindingT = AbstractBindingName<typename DataT::NameType>;
  return GetOffsetOfScopeDataTrailingNames<DataT>() + length * sizeof(BindingT);
}

//
// A useful typedef for selecting between a gc-aware wrappers
// around pointers to BaseScopeData-derived types, and around raw
// pointer wrappers around BaseParserScopeData-derived types.
//
template <typename ScopeT, typename AtomT>
using AbstractScopeData = typename ScopeT::template AbstractData<AtomT>;

// Binding names are stored from `this+1`.
// Make sure the class aligns the binding name size.
template <typename SlotInfo>
struct alignas(alignof(AbstractBindingName<frontend::TaggedParserAtomIndex>))
    ParserScopeData
    : public AbstractBaseScopeData<frontend::TaggedParserAtomIndex> {
  SlotInfo slotInfo;

  explicit ParserScopeData(size_t length) { PoisonNames(this, length); }
  ParserScopeData() = delete;
};

// RuntimeScopeData has 2 requirements:
//   * It aligns with `BindingName`, that is stored after `this+1`
//   * It aligns with ScopeDataAlignBytes, in order to put it in the first
//     word of `js::Scope`
static_assert(alignof(BindingName) <= ScopeDataAlignBytes);
template <typename SlotInfo>
struct alignas(ScopeDataAlignBytes) RuntimeScopeData
    : public AbstractBaseScopeData<JSAtom> {
  SlotInfo slotInfo;

  explicit RuntimeScopeData(size_t length) { PoisonNames(this, length); }
  RuntimeScopeData() = delete;

  void trace(JSTracer* trc);
};

//
// A lexical scope that holds let and const bindings. There are 4 kinds of
// LexicalScopes.
//
// Lexical
//   A plain lexical scope.
//
// SimpleCatch
//   Holds the single catch parameter of a catch block.
//
// Catch
//   Holds the catch parameters (and only the catch parameters) of a catch
//   block.
//
// NamedLambda
// StrictNamedLambda
//   Holds the single name of the callee for a named lambda expression.
//
// All kinds of LexicalScopes correspond to LexicalEnvironmentObjects on the
// environment chain.
//
class LexicalScope : public Scope {
  friend class Scope;
  friend class AbstractBindingIter<JSAtom>;
  friend class GCMarker;
  friend class frontend::ScopeStencil;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Bindings are sorted by kind in both frames and environments.
    //
    //   lets - [0, constStart)
    // consts - [constStart, length)
    uint32_t constStart = 0;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    // consts - [constStart, usingStart)
    // usings - [usingStart, length)
    uint32_t usingStart = 0;
#endif
  };

  using RuntimeData = RuntimeScopeData<SlotInfo>;
  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

 private:
  static void prepareForScopeCreation(ScopeKind kind, uint32_t firstFrameSlot,
                                      LexicalScope::ParserData* data,
                                      mozilla::Maybe<uint32_t>* envShape);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }
  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  static uint32_t nextFrameSlot(Scope* scope);

  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }

  // Returns an empty shape for extensible global and non-syntactic lexical
  // scopes.
  static SharedShape* getEmptyExtensibleEnvironmentShape(JSContext* cx);
};

template <>
inline bool Scope::is<LexicalScope>() const {
  return kind_ == ScopeKind::Lexical || kind_ == ScopeKind::SimpleCatch ||
         kind_ == ScopeKind::Catch || kind_ == ScopeKind::NamedLambda ||
         kind_ == ScopeKind::StrictNamedLambda ||
         kind_ == ScopeKind::FunctionLexical;
}

// The body scope of a JS class, containing only synthetic bindings for private
// class members. (The binding for the class name, `C` in the example below, is
// in another scope, a `LexicalScope`, that encloses the `ClassBodyScope`.)
// Example:
//
//     class C {
//       #f = 0;
//       #m() {
//         return this.#f++;
//       }
//     }
//
// This class has a ClassBodyScope with four synthetic bindings:
// - `#f` (private name)
// - `#m` (private name)
// - `#m.method` (function object)
// - `.privateBrand` (the class's private brand)
class ClassBodyScope : public Scope {
  friend class Scope;
  friend class AbstractBindingIter<JSAtom>;
  friend class GCMarker;
  friend class frontend::ScopeStencil;
  friend class AbstractScopePtr;

  static const ScopeKind classScopeKind_ = ScopeKind::ClassBody;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Bindings are sorted by kind in both frames and environments.
    //
    //     synthetic - [0, privateMethodStart)
    // privateMethod - [privateMethodStart, length)
    uint32_t privateMethodStart = 0;
  };

  using RuntimeData = RuntimeScopeData<SlotInfo>;
  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

 private:
  static void prepareForScopeCreation(ScopeKind kind, uint32_t firstFrameSlot,
                                      ClassBodyScope::ParserData* data,
                                      mozilla::Maybe<uint32_t>* envShape);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }
  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  static uint32_t nextFrameSlot(Scope* scope);

  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }

  // Returns an empty shape for extensible global and non-syntactic lexical
  // scopes.
  static SharedShape* getEmptyExtensibleEnvironmentShape(JSContext* cx);
};

//
// Scope corresponding to a function. Holds formal parameter names, special
// internal names (see FunctionScope::isSpecialName), and, if the function
// parameters contain no expressions that might possibly be evaluated, the
// function's var bindings. For example, in these functions, the FunctionScope
// will store a/b/c bindings but not d/e/f bindings:
//
//   function f1(a, b) {
//     var c;
//     let e;
//     const f = 3;
//   }
//   function f2([a], b = 4, ...c) {
//     var d, e, f; // stored in VarScope
//   }
//
// Corresponds to CallObject on environment chain.
//
class FunctionScope : public Scope {
  friend class GCMarker;
  friend class AbstractBindingIter<JSAtom>;
  friend class AbstractPositionalFormalParameterIter<JSAtom>;
  friend class Scope;
  friend class AbstractScopePtr;
  static const ScopeKind classScopeKind_ = ScopeKind::Function;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Flag bits.
    // This uses uint32_t in order to make this struct packed.
    uint32_t flags = 0;

    // If parameter expressions are present, parameters act like lexical
    // bindings.
    static constexpr uint32_t HasParameterExprsFlag = 1;

    // Bindings are sorted by kind in both frames and environments.
    //
    // Positional formal parameter names are those that are not
    // destructured. They may be referred to by argument slots if
    // !script()->hasParameterExprs().
    //
    // An argument slot that needs to be skipped due to being destructured
    // or having defaults will have a nullptr name in the name array to
    // advance the argument slot.
    //
    // Rest parameter binding is also included in positional formals.
    // This also becomes nullptr if destructuring.
    //
    // The number of positional formals is equal to function.length if
    // there's no rest, function.length+1 otherwise.
    //
    // Destructuring parameters and destructuring rest are included in
    // "other formals" below.
    //
    // "vars" contains the following:
    //   * function's top level vars if !script()->hasParameterExprs()
    //   * special internal names (arguments, .this, .generator) if
    //     they're used.
    //
    // positional formals - [0, nonPositionalFormalStart)
    //      other formals - [nonPositionalParamStart, varStart)
    //               vars - [varStart, length)
    uint16_t nonPositionalFormalStart = 0;
    uint16_t varStart = 0;

    bool hasParameterExprs() const { return flags & HasParameterExprsFlag; }
    void setHasParameterExprs() { flags |= HasParameterExprsFlag; }
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    SlotInfo slotInfo;
    // The canonical function of the scope, as during a scope walk we
    // often query properties of the JSFunction (e.g., is the function an
    // arrow).
    GCPtr<JSFunction*> canonicalFunction = {};

    explicit RuntimeData(size_t length) { PoisonNames(this, length); }
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  static void prepareForScopeCreation(FunctionScope::ParserData* data,
                                      bool hasParameterExprs,
                                      bool needsEnvironment,
                                      mozilla::Maybe<uint32_t>* envShape);

 private:
  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }

  JSFunction* canonicalFunction() const { return data().canonicalFunction; }
  void initCanonicalFunction(JSFunction* fun) {
    data().canonicalFunction.init(fun);
  }

  JSScript* script() const;

  bool hasParameterExprs() const { return data().slotInfo.hasParameterExprs(); }

  uint32_t numPositionalFormalParameters() const {
    return data().slotInfo.nonPositionalFormalStart;
  }

  static bool isSpecialName(frontend::TaggedParserAtomIndex name);
};

//
// Scope holding only vars. There is a single kind of VarScopes.
//
// FunctionBodyVar
//   Corresponds to the extra var scope present in functions with parameter
//   expressions. See examples in comment above FunctionScope.
//
// Corresponds to VarEnvironmentObject on environment chain.
//
class VarScope : public Scope {
  friend class GCMarker;
  friend class AbstractBindingIter<JSAtom>;
  friend class Scope;
  friend class frontend::ScopeStencil;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // All bindings are vars.
    //
    //            vars - [0, length)
  };

  using RuntimeData = RuntimeScopeData<SlotInfo>;
  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

 private:
  static void prepareForScopeCreation(ScopeKind kind,
                                      VarScope::ParserData* data,
                                      uint32_t firstFrameSlot,
                                      bool needsEnvironment,
                                      mozilla::Maybe<uint32_t>* envShape);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }
};

template <>
inline bool Scope::is<VarScope>() const {
  return kind_ == ScopeKind::FunctionBodyVar;
}

//
// Scope corresponding to both the global object scope and the global lexical
// scope.
//
// Both are extensible and are singletons across <script> tags, so these
// scopes are a fragment of the names in global scope. In other words, two
// global scripts may have two different GlobalScopes despite having the same
// GlobalObject.
//
// There are 2 kinds of GlobalScopes.
//
// Global
//   Corresponds to a GlobalObject and its GlobalLexicalEnvironmentObject on
//   the environment chain.
//
// NonSyntactic
//   Corresponds to a non-GlobalObject created by the embedding on the
//   environment chain. This distinction is important for optimizations.
//
class GlobalScope : public Scope {
  friend class Scope;
  friend class AbstractBindingIter<JSAtom>;
  friend class GCMarker;

 public:
  struct SlotInfo {
    // Bindings are sorted by kind.
    // `vars` includes top-level functions which is distinguished by a bit
    // on the BindingName.
    //
    //            vars - [0, letStart)
    //            lets - [letStart, constStart)
    //          consts - [constStart, length)
    uint32_t letStart = 0;
    uint32_t constStart = 0;
  };

  using RuntimeData = RuntimeScopeData<SlotInfo>;
  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  static GlobalScope* createEmpty(JSContext* cx, ScopeKind kind);

 private:
  static GlobalScope* createWithData(
      JSContext* cx, ScopeKind kind,
      MutableHandle<UniquePtr<RuntimeData>> data);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  bool isSyntactic() const { return kind() != ScopeKind::NonSyntactic; }

  bool hasBindings() const { return data().length > 0; }
};

template <>
inline bool Scope::is<GlobalScope>() const {
  return kind_ == ScopeKind::Global || kind_ == ScopeKind::NonSyntactic;
}

//
// Scope of a 'with' statement. Has no bindings.
//
// Corresponds to a WithEnvironmentObject on the environment chain.
class WithScope : public Scope {
  friend class Scope;
  friend class AbstractScopePtr;
  static const ScopeKind classScopeKind_ = ScopeKind::With;

 public:
  static WithScope* create(JSContext* cx, Handle<Scope*> enclosing);
};

//
// Scope of an eval. Holds var bindings. There are 2 kinds of EvalScopes.
//
// StrictEval
//   A strict eval. Corresponds to a VarEnvironmentObject, where its var
//   bindings lives.
//
// Eval
//   A sloppy eval. This is an empty scope, used only in the frontend, to
//   detect redeclaration errors. It has no Environment. Any `var`s declared
//   in the eval code are bound on the nearest enclosing var environment.
//
class EvalScope : public Scope {
  friend class Scope;
  friend class AbstractBindingIter<JSAtom>;
  friend class GCMarker;
  friend class frontend::ScopeStencil;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // All bindings in an eval script are 'var' bindings. The implicit
    // lexical scope around the eval is present regardless of strictness
    // and is its own LexicalScope.
    // `vars` includes top-level functions which is distinguished by a bit
    // on the BindingName.
    //
    //            vars - [0, length)
  };

  using RuntimeData = RuntimeScopeData<SlotInfo>;
  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

 private:
  static void prepareForScopeCreation(ScopeKind scopeKind,
                                      EvalScope::ParserData* data,
                                      mozilla::Maybe<uint32_t>* envShape);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  // Starting a scope, the nearest var scope that a direct eval can
  // introduce vars on.
  static Scope* nearestVarScopeForDirectEval(Scope* scope);

  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }

  bool strict() const { return kind() == ScopeKind::StrictEval; }

  bool hasBindings() const { return data().length > 0; }

  bool isNonGlobal() const {
    if (strict()) {
      return true;
    }
    return !nearestVarScopeForDirectEval(enclosing())->is<GlobalScope>();
  }
};

template <>
inline bool Scope::is<EvalScope>() const {
  return kind_ == ScopeKind::Eval || kind_ == ScopeKind::StrictEval;
}

//
// Scope corresponding to the toplevel script in an ES module.
//
// Like GlobalScopes, these scopes contain both vars and lexical bindings, as
// the treating of imports and exports requires putting them in one scope.
//
// Corresponds to a ModuleEnvironmentObject on the environment chain.
//
class ModuleScope : public Scope {
  friend class GCMarker;
  friend class AbstractBindingIter<JSAtom>;
  friend class Scope;
  friend class AbstractScopePtr;
  friend class frontend::ScopeStencil;
  static const ScopeKind classScopeKind_ = ScopeKind::Module;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Bindings are sorted by kind.
    //
    // imports - [0, varStart)
    //    vars - [varStart, letStart)
    //    lets - [letStart, constStart)
    //  consts - [constStart, length)
    uint32_t varStart = 0;
    uint32_t letStart = 0;
    uint32_t constStart = 0;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    // consts - [constStart, usingStart)
    // usings - [usingStart, length)
    uint32_t usingStart = 0;
#endif
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    SlotInfo slotInfo;
    // The module of the scope.
    GCPtr<ModuleObject*> module = {};

    explicit RuntimeData(size_t length);
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

 private:
  static void prepareForScopeCreation(ModuleScope::ParserData* data,
                                      mozilla::Maybe<uint32_t>* envShape);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }

  ModuleObject* module() const { return data().module; }
  void initModule(ModuleObject* mod) { return data().module.init(mod); }

  // Off-thread compilation needs to calculate environmentChainLength for
  // an emptyGlobalScope where the global may not be available.
  static const size_t EnclosingEnvironmentChainLength = 1;
};

class WasmInstanceScope : public Scope {
  friend class AbstractBindingIter<JSAtom>;
  friend class Scope;
  friend class GCMarker;
  friend class AbstractScopePtr;
  static const ScopeKind classScopeKind_ = ScopeKind::WasmInstance;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Bindings list the WASM memories and globals.
    //
    // memories - [0, globalsStart)
    //  globals - [globalsStart, length)
    uint32_t memoriesStart = 0;
    uint32_t globalsStart = 0;
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    SlotInfo slotInfo;
    // The wasm instance of the scope.
    GCPtr<WasmInstanceObject*> instance = {};

    explicit RuntimeData(size_t length);
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  static WasmInstanceScope* create(JSContext* cx, WasmInstanceObject* instance);

 private:
  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  WasmInstanceObject* instance() const { return data().instance; }

  uint32_t memoriesStart() const { return data().slotInfo.memoriesStart; }

  uint32_t globalsStart() const { return data().slotInfo.globalsStart; }

  uint32_t namesCount() const { return data().length; }
};

// Scope corresponding to the wasm function. A WasmFunctionScope is used by
// Debugger only, and not for wasm execution.
//
class WasmFunctionScope : public Scope {
  friend class AbstractBindingIter<JSAtom>;
  friend class Scope;
  friend class GCMarker;
  friend class AbstractScopePtr;
  static const ScopeKind classScopeKind_ = ScopeKind::WasmFunction;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Bindings are the local variable names.
    //
    //    vars - [0, length)
  };

  using RuntimeData = RuntimeScopeData<SlotInfo>;
  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  static WasmFunctionScope* create(JSContext* cx, Handle<Scope*> enclosing,
                                   uint32_t funcIndex);

 private:
  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }
};

template <typename F>
void Scope::applyScopeDataTyped(F&& f) {
  switch (kind()) {
    case ScopeKind::Function: {
      f(&as<FunctionScope>().data());
      break;
      case ScopeKind::FunctionBodyVar:
        f(&as<VarScope>().data());
        break;
      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
      case ScopeKind::FunctionLexical:
        f(&as<LexicalScope>().data());
        break;
      case ScopeKind::ClassBody:
        f(&as<ClassBodyScope>().data());
        break;
      case ScopeKind::With:
        // With scopes do not have data.
        break;
      case ScopeKind::Eval:
      case ScopeKind::StrictEval:
        f(&as<EvalScope>().data());
        break;
      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
        f(&as<GlobalScope>().data());
        break;
      case ScopeKind::Module:
        f(&as<ModuleScope>().data());
        break;
      case ScopeKind::WasmInstance:
        f(&as<WasmInstanceScope>().data());
        break;
      case ScopeKind::WasmFunction:
        f(&as<WasmFunctionScope>().data());
        break;
    }
  }
}

//
// An iterator for a Scope's bindings. This is the source of truth for frame
// and environment object layout.
//
// It may be placed in GC containers; for example:
//
//   for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++) {
//     use(bi);
//     SomeMayGCOperation();
//     use(bi);
//   }
//
template <typename NameT>
class BaseAbstractBindingIter {
 protected:
  // Bindings are sorted by kind. Because different Scopes have differently
  // laid out {Runtime,Parser}Data for packing, BindingIter must handle all
  // binding kinds.
  //
  // Kind ranges:
  //
  //            imports - [0, positionalFormalStart)
  // positional formals - [positionalFormalStart, nonPositionalFormalStart)
  //      other formals - [nonPositionalParamStart, varStart)
  //               vars - [varStart, letStart)
  //               lets - [letStart, constStart)
  //             consts - [constStart, syntheticStart)
  //          synthetic - [syntheticStart, privateMethodStart)
  //    private methods = [privateMethodStart, length)
  //
  // If ENABLE_EXPLICIT_RESOURCE_MANAGEMENT is set, the consts range is split
  // into the following:
  //             consts - [constStart, usingStart)
  //             usings - [usingStart, syntheticStart)
  //
  // Access method when not closed over:
  //
  //            imports - name
  // positional formals - argument slot
  //      other formals - frame slot
  //               vars - frame slot
  //               lets - frame slot
  //             consts - frame slot
  //          synthetic - frame slot
  //    private methods - frame slot
  //
  // Access method when closed over:
  //
  //            imports - name
  // positional formals - environment slot or name
  //      other formals - environment slot or name
  //               vars - environment slot or name
  //               lets - environment slot or name
  //             consts - environment slot or name
  //          synthetic - environment slot or name
  //    private methods - environment slot or name
  MOZ_INIT_OUTSIDE_CTOR uint32_t positionalFormalStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t nonPositionalFormalStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t varStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t letStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t constStart_;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  MOZ_INIT_OUTSIDE_CTOR uint32_t usingStart_;
#endif
  MOZ_INIT_OUTSIDE_CTOR uint32_t syntheticStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t privateMethodStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t length_;

  MOZ_INIT_OUTSIDE_CTOR uint32_t index_;

  enum Flags : uint8_t {
    CannotHaveSlots = 0,
    CanHaveArgumentSlots = 1 << 0,
    CanHaveFrameSlots = 1 << 1,
    CanHaveEnvironmentSlots = 1 << 2,

    // See comment in settle below.
    HasFormalParameterExprs = 1 << 3,
    IgnoreDestructuredFormalParameters = 1 << 4,

    // Truly I hate named lambdas.
    IsNamedLambda = 1 << 5
  };

  static const uint8_t CanHaveSlotsMask = 0x7;

  MOZ_INIT_OUTSIDE_CTOR uint8_t flags_;
  MOZ_INIT_OUTSIDE_CTOR uint16_t argumentSlot_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t frameSlot_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t environmentSlot_;

  MOZ_INIT_OUTSIDE_CTOR AbstractBindingName<NameT>* names_;

  void init(uint32_t positionalFormalStart, uint32_t nonPositionalFormalStart,
            uint32_t varStart, uint32_t letStart, uint32_t constStart,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
            uint32_t usingStart,
#endif
            uint32_t syntheticStart, uint32_t privateMethodStart, uint8_t flags,
            uint32_t firstFrameSlot, uint32_t firstEnvironmentSlot,
            mozilla::Span<AbstractBindingName<NameT>> names) {
    positionalFormalStart_ = positionalFormalStart;
    nonPositionalFormalStart_ = nonPositionalFormalStart;
    varStart_ = varStart;
    letStart_ = letStart;
    constStart_ = constStart;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    usingStart_ = usingStart;
#endif
    syntheticStart_ = syntheticStart;
    privateMethodStart_ = privateMethodStart;
    length_ = names.size();

    index_ = 0;
    flags_ = flags;
    argumentSlot_ = 0;
    frameSlot_ = firstFrameSlot;
    environmentSlot_ = firstEnvironmentSlot;
    names_ = names.data();

    settle();
  }

  void init(LexicalScope::AbstractData<NameT>& data, uint32_t firstFrameSlot,
            uint8_t flags);

  void init(ClassBodyScope::AbstractData<NameT>& data, uint32_t firstFrameSlot);
  void init(FunctionScope::AbstractData<NameT>& data, uint8_t flags);

  void init(VarScope::AbstractData<NameT>& data, uint32_t firstFrameSlot);
  void init(GlobalScope::AbstractData<NameT>& data);
  void init(EvalScope::AbstractData<NameT>& data, bool strict);
  void init(ModuleScope::AbstractData<NameT>& data);
  void init(WasmInstanceScope::AbstractData<NameT>& data);
  void init(WasmFunctionScope::AbstractData<NameT>& data);

  bool hasFormalParameterExprs() const {
    return flags_ & HasFormalParameterExprs;
  }

  bool ignoreDestructuredFormalParameters() const {
    return flags_ & IgnoreDestructuredFormalParameters;
  }

  bool isNamedLambda() const { return flags_ & IsNamedLambda; }

  void increment() {
    MOZ_ASSERT(!done());
    if (flags_ & CanHaveSlotsMask) {
      if (canHaveArgumentSlots()) {
        if (index_ < nonPositionalFormalStart_) {
          MOZ_ASSERT(index_ >= positionalFormalStart_);
          argumentSlot_++;
        }
      }
      if (closedOver()) {
        // Imports must not be given known slots. They are
        // indirect bindings.
        MOZ_ASSERT(kind() != BindingKind::Import);
        MOZ_ASSERT(canHaveEnvironmentSlots());
        environmentSlot_++;
      } else if (canHaveFrameSlots()) {
        // Usually positional formal parameters don't have frame
        // slots, except when there are parameter expressions, in
        // which case they act like lets.
        if (index_ >= nonPositionalFormalStart_ ||
            (hasFormalParameterExprs() && name())) {
          frameSlot_++;
        }
      }
    }
    index_++;
  }

  void settle() {
    if (ignoreDestructuredFormalParameters()) {
      while (!done() && !name()) {
        increment();
      }
    }
  }

  BaseAbstractBindingIter() = default;

 public:
  BaseAbstractBindingIter(LexicalScope::AbstractData<NameT>& data,
                          uint32_t firstFrameSlot, bool isNamedLambda) {
    init(data, firstFrameSlot, isNamedLambda ? IsNamedLambda : 0);
  }

  BaseAbstractBindingIter(ClassBodyScope::AbstractData<NameT>& data,
                          uint32_t firstFrameSlot) {
    init(data, firstFrameSlot);
  }

  BaseAbstractBindingIter(FunctionScope::AbstractData<NameT>& data,
                          bool hasParameterExprs) {
    init(data, IgnoreDestructuredFormalParameters |
                   (hasParameterExprs ? HasFormalParameterExprs : 0));
  }

  BaseAbstractBindingIter(VarScope::AbstractData<NameT>& data,
                          uint32_t firstFrameSlot) {
    init(data, firstFrameSlot);
  }

  explicit BaseAbstractBindingIter(GlobalScope::AbstractData<NameT>& data) {
    init(data);
  }

  explicit BaseAbstractBindingIter(ModuleScope::AbstractData<NameT>& data) {
    init(data);
  }

  explicit BaseAbstractBindingIter(
      WasmFunctionScope::AbstractData<NameT>& data) {
    init(data);
  }

  BaseAbstractBindingIter(EvalScope::AbstractData<NameT>& data, bool strict) {
    init(data, strict);
  }

  MOZ_IMPLICIT BaseAbstractBindingIter(
      const BaseAbstractBindingIter<NameT>& bi) = default;

  bool done() const { return index_ == length_; }

  explicit operator bool() const { return !done(); }

  void operator++(int) {
    increment();
    settle();
  }

  bool isLast() const {
    MOZ_ASSERT(!done());
    return index_ + 1 == length_;
  }

  bool canHaveArgumentSlots() const { return flags_ & CanHaveArgumentSlots; }

  bool canHaveFrameSlots() const { return flags_ & CanHaveFrameSlots; }

  bool canHaveEnvironmentSlots() const {
    return flags_ & CanHaveEnvironmentSlots;
  }

  typename AbstractBindingName<NameT>::NamePointerT name() const {
    MOZ_ASSERT(!done());
    return names_[index_].name();
  }

  bool closedOver() const {
    MOZ_ASSERT(!done());
    return names_[index_].closedOver();
  }

  BindingLocation location() const {
    MOZ_ASSERT(!done());
    if (!(flags_ & CanHaveSlotsMask)) {
      return BindingLocation::Global();
    }
    if (index_ < positionalFormalStart_) {
      return BindingLocation::Import();
    }
    if (closedOver()) {
      MOZ_ASSERT(canHaveEnvironmentSlots());
      return BindingLocation::Environment(environmentSlot_);
    }
    if (index_ < nonPositionalFormalStart_ && canHaveArgumentSlots()) {
      return BindingLocation::Argument(argumentSlot_);
    }
    if (canHaveFrameSlots()) {
      return BindingLocation::Frame(frameSlot_);
    }
    MOZ_ASSERT(isNamedLambda());
    return BindingLocation::NamedLambdaCallee();
  }

  BindingKind kind() const {
    MOZ_ASSERT(!done());
    if (index_ < positionalFormalStart_) {
      return BindingKind::Import;
    }
    if (index_ < varStart_) {
      // When the parameter list has expressions, the parameters act
      // like lexical bindings and have TDZ.
      if (hasFormalParameterExprs()) {
        return BindingKind::Let;
      }
      return BindingKind::FormalParameter;
    }
    if (index_ < letStart_) {
      return BindingKind::Var;
    }
    if (index_ < constStart_) {
      return BindingKind::Let;
    }
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    if (index_ < usingStart_) {
      return isNamedLambda() ? BindingKind::NamedLambdaCallee
                             : BindingKind::Const;
    }
    if (index_ < syntheticStart_) {
      return BindingKind::Using;
    }
#else
    if (index_ < syntheticStart_) {
      return isNamedLambda() ? BindingKind::NamedLambdaCallee
                             : BindingKind::Const;
    }
#endif
    if (index_ < privateMethodStart_) {
      return BindingKind::Synthetic;
    }
    return BindingKind::PrivateMethod;
  }

  js::frontend::NameLocation nameLocation() const {
    using js::frontend::NameLocation;

    BindingKind bindKind = kind();
    BindingLocation bl = location();
    switch (bl.kind()) {
      case BindingLocation::Kind::Global:
        return NameLocation::Global(bindKind);
      case BindingLocation::Kind::Argument:
        return NameLocation::ArgumentSlot(bl.argumentSlot());
      case BindingLocation::Kind::Frame:
        return NameLocation::FrameSlot(bindKind, bl.slot());
      case BindingLocation::Kind::Environment:
        return NameLocation::EnvironmentCoordinate(bindKind, 0, bl.slot());
      case BindingLocation::Kind::Import:
        return NameLocation::Import();
      case BindingLocation::Kind::NamedLambdaCallee:
        return NameLocation::NamedLambdaCallee();
    }
    MOZ_CRASH("Bad BindingKind");
  }

  bool isTopLevelFunction() const {
    MOZ_ASSERT(!done());
    bool result = names_[index_].isTopLevelFunction();
    MOZ_ASSERT_IF(result, kind() == BindingKind::Var);
    return result;
  }

  bool hasArgumentSlot() const {
    MOZ_ASSERT(!done());
    if (hasFormalParameterExprs()) {
      return false;
    }
    return index_ >= positionalFormalStart_ &&
           index_ < nonPositionalFormalStart_;
  }

  uint16_t argumentSlot() const {
    MOZ_ASSERT(canHaveArgumentSlots());
    return mozilla::AssertedCast<uint16_t>(index_);
  }

  uint32_t nextFrameSlot() const {
    MOZ_ASSERT(canHaveFrameSlots());
    return frameSlot_;
  }

  uint32_t nextEnvironmentSlot() const {
    MOZ_ASSERT(canHaveEnvironmentSlots());
    return environmentSlot_;
  }
};

template <typename NameT>
class AbstractBindingIter;

template <>
class AbstractBindingIter<JSAtom> : public BaseAbstractBindingIter<JSAtom> {
  using Base = BaseAbstractBindingIter<JSAtom>;

 public:
  AbstractBindingIter(ScopeKind kind, BaseScopeData* data,
                      uint32_t firstFrameSlot);

  explicit AbstractBindingIter(Scope* scope);
  explicit AbstractBindingIter(JSScript* script);

  using Base::Base;

  inline void trace(JSTracer* trc) {
    TraceNullableBindingNames(trc, names_, length_);
  }
};

template <>
class AbstractBindingIter<frontend::TaggedParserAtomIndex>
    : public BaseAbstractBindingIter<frontend::TaggedParserAtomIndex> {
  using Base = BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>;

 public:
  explicit AbstractBindingIter(const frontend::ScopeStencilRef& ref);

  using Base::Base;
};

void DumpBindings(JSContext* cx, Scope* scope);
JSAtom* FrameSlotName(JSScript* script, jsbytecode* pc);

SharedShape* EmptyEnvironmentShape(JSContext* cx, const JSClass* cls,
                                   uint32_t numSlots, ObjectFlags objectFlags);

template <class T>
SharedShape* EmptyEnvironmentShape(JSContext* cx) {
  return EmptyEnvironmentShape(cx, &T::class_, T::RESERVED_SLOTS,
                               T::OBJECT_FLAGS);
}

//
// PositionalFormalParameterIter is a refinement BindingIter that only iterates
// over positional formal parameters of a function.
//
template <typename NameT>
class BasePositionalFormalParamterIter : public AbstractBindingIter<NameT> {
  using Base = AbstractBindingIter<NameT>;

 protected:
  void settle() {
    if (this->index_ >= this->nonPositionalFormalStart_) {
      this->index_ = this->length_;
    }
  }

 public:
  using Base::Base;

  void operator++(int) {
    Base::operator++(1);
    settle();
  }

  bool isDestructured() const { return !this->name(); }
};

template <typename NameT>
class AbstractPositionalFormalParameterIter;

template <>
class AbstractPositionalFormalParameterIter<JSAtom>
    : public BasePositionalFormalParamterIter<JSAtom> {
  using Base = BasePositionalFormalParamterIter<JSAtom>;

 public:
  explicit AbstractPositionalFormalParameterIter(Scope* scope);
  explicit AbstractPositionalFormalParameterIter(JSScript* script);

  using Base::Base;
};

template <>
class AbstractPositionalFormalParameterIter<frontend::TaggedParserAtomIndex>
    : public BasePositionalFormalParamterIter<frontend::TaggedParserAtomIndex> {
  using Base =
      BasePositionalFormalParamterIter<frontend::TaggedParserAtomIndex>;

 public:
  AbstractPositionalFormalParameterIter(
      FunctionScope::AbstractData<frontend::TaggedParserAtomIndex>& data,
      bool hasParameterExprs)
      : Base(data, hasParameterExprs) {
    settle();
  }

  using Base::Base;
};

using PositionalFormalParameterIter =
    AbstractPositionalFormalParameterIter<JSAtom>;

//
// Iterator for walking the scope chain.
//
// It may be placed in GC containers; for example:
//
//   for (Rooted<ScopeIter> si(cx, ScopeIter(scope)); si; si++) {
//     use(si);
//     SomeMayGCOperation();
//     use(si);
//   }
//
class MOZ_STACK_CLASS ScopeIter {
  Scope* scope_;

 public:
  explicit ScopeIter(Scope* scope) : scope_(scope) {}

  explicit ScopeIter(JSScript* script);

  explicit ScopeIter(const ScopeIter& si) = default;

  bool done() const { return !scope_; }

  explicit operator bool() const { return !done(); }

  void operator++(int) {
    MOZ_ASSERT(!done());
    scope_ = scope_->enclosing();
  }

  Scope* scope() const {
    MOZ_ASSERT(!done());
    return scope_;
  }

  ScopeKind kind() const {
    MOZ_ASSERT(!done());
    return scope_->kind();
  }

  // Returns the shape of the environment if it is known. It is possible to
  // hasSyntacticEnvironment and to have no known shape, e.g., eval.
  SharedShape* environmentShape() const { return scope()->environmentShape(); }

  // Returns whether this scope has a syntactic environment (i.e., an
  // Environment that isn't a non-syntactic With or NonSyntacticVariables)
  // on the environment chain.
  bool hasSyntacticEnvironment() const;

  void trace(JSTracer* trc) {
    if (scope_) {
      TraceRoot(trc, &scope_, "scope iter scope");
    }
  }
};

//
// Specializations of Rooted containers for the iterators.
//

template <typename Wrapper>
class WrappedPtrOperations<BindingIter, Wrapper> {
  const BindingIter& iter() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  bool done() const { return iter().done(); }
  explicit operator bool() const { return !done(); }
  bool isLast() const { return iter().isLast(); }
  bool canHaveArgumentSlots() const { return iter().canHaveArgumentSlots(); }
  bool canHaveFrameSlots() const { return iter().canHaveFrameSlots(); }
  bool canHaveEnvironmentSlots() const {
    return iter().canHaveEnvironmentSlots();
  }
  JSAtom* name() const { return iter().name(); }
  bool closedOver() const { return iter().closedOver(); }
  BindingLocation location() const { return iter().location(); }
  BindingKind kind() const { return iter().kind(); }
  bool isTopLevelFunction() const { return iter().isTopLevelFunction(); }
  bool hasArgumentSlot() const { return iter().hasArgumentSlot(); }
  uint16_t argumentSlot() const { return iter().argumentSlot(); }
  uint32_t nextFrameSlot() const { return iter().nextFrameSlot(); }
  uint32_t nextEnvironmentSlot() const { return iter().nextEnvironmentSlot(); }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<BindingIter, Wrapper>
    : public WrappedPtrOperations<BindingIter, Wrapper> {
  BindingIter& iter() { return static_cast<Wrapper*>(this)->get(); }

 public:
  void operator++(int) { iter().operator++(1); }
};

template <typename Wrapper>
class WrappedPtrOperations<ScopeIter, Wrapper> {
  const ScopeIter& iter() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  bool done() const { return iter().done(); }
  explicit operator bool() const { return !done(); }
  Scope* scope() const { return iter().scope(); }
  ScopeKind kind() const { return iter().kind(); }
  SharedShape* environmentShape() const { return iter().environmentShape(); }
  bool hasSyntacticEnvironment() const {
    return iter().hasSyntacticEnvironment();
  }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<ScopeIter, Wrapper>
    : public WrappedPtrOperations<ScopeIter, Wrapper> {
  ScopeIter& iter() { return static_cast<Wrapper*>(this)->get(); }

 public:
  void operator++(int) { iter().operator++(1); }
};

SharedShape* CreateEnvironmentShape(JSContext* cx, BindingIter& bi,
                                    const JSClass* cls, uint32_t numSlots,
                                    ObjectFlags objectFlags);

SharedShape* CreateEnvironmentShapeForSyntheticModule(
    JSContext* cx, const JSClass* cls, uint32_t numSlots,
    Handle<ModuleObject*> module);

SharedShape* EmptyEnvironmentShape(JSContext* cx, const JSClass* cls,
                                   uint32_t numSlots, ObjectFlags objectFlags);

static inline size_t GetOffsetOfParserScopeDataTrailingNames(ScopeKind kind) {
  switch (kind) {
    // FunctionScope
    case ScopeKind::Function:
      return GetOffsetOfScopeDataTrailingNames<FunctionScope::ParserData>();

    // VarScope
    case ScopeKind::FunctionBodyVar:
      return GetOffsetOfScopeDataTrailingNames<VarScope::ParserData>();

    // LexicalScope
    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
    case ScopeKind::FunctionLexical:
      return GetOffsetOfScopeDataTrailingNames<LexicalScope::ParserData>();

    // ClassBodyScope
    case ScopeKind::ClassBody:
      return GetOffsetOfScopeDataTrailingNames<ClassBodyScope::ParserData>();

    // EvalScope
    case ScopeKind::Eval:
    case ScopeKind::StrictEval:
      return GetOffsetOfScopeDataTrailingNames<EvalScope::ParserData>();

    // GlobalScope
    case ScopeKind::Global:
    case ScopeKind::NonSyntactic:
      return GetOffsetOfScopeDataTrailingNames<GlobalScope::ParserData>();

    // ModuleScope
    case ScopeKind::Module:
      return GetOffsetOfScopeDataTrailingNames<ModuleScope::ParserData>();

    // WasmInstanceScope
    case ScopeKind::WasmInstance:
      return GetOffsetOfScopeDataTrailingNames<WasmInstanceScope::ParserData>();

    // WasmFunctionScope
    case ScopeKind::WasmFunction:
      return GetOffsetOfScopeDataTrailingNames<WasmFunctionScope::ParserData>();

    // WithScope doesn't have ScopeData.
    case ScopeKind::With:
    default:
      MOZ_CRASH("Unexpected ScopeKind");
  }

  return 0;
}

inline size_t SizeOfParserScopeData(ScopeKind kind, uint32_t length) {
  return GetOffsetOfParserScopeDataTrailingNames(kind) +
         sizeof(AbstractBindingName<frontend::TaggedParserAtomIndex>) * length;
}

inline mozilla::Span<AbstractBindingName<frontend::TaggedParserAtomIndex>>
GetParserScopeDataTrailingNames(
    ScopeKind kind,
    AbstractBaseScopeData<frontend::TaggedParserAtomIndex>* data) {
  return mozilla::Span(
      reinterpret_cast<AbstractBindingName<frontend::TaggedParserAtomIndex>*>(
          uintptr_t(data) + GetOffsetOfParserScopeDataTrailingNames(kind)),
      data->length);
}

}  // namespace js

namespace JS {

template <>
struct GCPolicy<js::ScopeKind> : public IgnoreGCPolicy<js::ScopeKind> {};

template <typename T>
struct ScopeDataGCPolicy : public NonGCPointerPolicy<T> {};

#define DEFINE_SCOPE_DATA_GCPOLICY(Data)              \
  template <>                                         \
  struct MapTypeToRootKind<Data*> {                   \
    static const RootKind kind = RootKind::Traceable; \
  };                                                  \
  template <>                                         \
  struct GCPolicy<Data*> : public ScopeDataGCPolicy<Data*> {}

DEFINE_SCOPE_DATA_GCPOLICY(js::LexicalScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::ClassBodyScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::FunctionScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::VarScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::GlobalScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::EvalScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::ModuleScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::WasmFunctionScope::RuntimeData);

#undef DEFINE_SCOPE_DATA_GCPOLICY

namespace ubi {

template <>
class Concrete<js::Scope> : TracerConcrete<js::Scope> {
 protected:
  explicit Concrete(js::Scope* ptr) : TracerConcrete<js::Scope>(ptr) {}

 public:
  static void construct(void* storage, js::Scope* ptr) {
    new (storage) Concrete(ptr);
  }

  CoarseType coarseType() const final { return CoarseType::Script; }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

}  // namespace ubi
}  // namespace JS

#endif  // vm_Scope_h
