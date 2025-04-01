/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ScopeBindingCache_h
#define frontend_ScopeBindingCache_h

#include "mozilla/Assertions.h"  // mozilla::MakeCompilerAssumeUnreachableFakeValue
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/HashTable.h"   // mozilla::HashMap

#include "jstypes.h"  // JS_PUBLIC_API

#include "frontend/NameAnalysisTypes.h"  // NameLocation
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex, ParserAtomsTable

#include "js/Utility.h"  // AutoEnterOOMUnsafeRegion

#include "vm/Scope.h"       // AbstractBaseScopeData
#include "vm/StringType.h"  // JSAtom

namespace js {
namespace frontend {

struct CompilationAtomCache;
struct CompilationStencil;
struct ScopeStencilRef;
struct FakeStencilGlobalScope;
struct CompilationStencilMerger;

// Generic atom wrapper which provides a way to interpret any Atom given
// contextual information. Thus, this structure offers the ability to compare
// Atom from different domains.
//
// This structure provides a `hash` field which is universal across all Atom
// representations. Thus, Atoms from different contexes can be registered in a
// hash table and looked up with a different Atom kind.
struct GenericAtom {
  // Emitter names are TaggedParserAtomIndex which are registered in the
  // ParserAtomsTable of an extensible compilation stencil, frequently related
  // to bytecode emitter, which lookup names in the scope chain to replace names
  // by variable locations.
  struct EmitterName {
    FrontendContext* fc;
    ParserAtomsTable& parserAtoms;
    CompilationAtomCache& atomCache;
    TaggedParserAtomIndex index;

    EmitterName(FrontendContext* fc, ParserAtomsTable& parserAtoms,
                CompilationAtomCache& atomCache, TaggedParserAtomIndex index)
        : fc(fc),
          parserAtoms(parserAtoms),
          atomCache(atomCache),
          index(index) {}
  };

  // Stencil names are TaggedParserAtomIndex which are registered in a
  // ParserAtomVector of a compilation stencil, frequently related to the result
  // of a compilation. It can be seen while manipulating names of a scope chain
  // while delazifying functions using a stencil for context.
  struct StencilName {
    const CompilationStencil& stencil;
    TaggedParserAtomIndex index;
  };

  // Any names are references to different Atom representation, including some
  // which are interpretable given some contexts such as EmitterName and
  // StencilName.
  using AnyName = mozilla::Variant<EmitterName, StencilName, JSAtom*>;

  HashNumber hash;
  AnyName ref;

  // Constructor for atoms managed by an ExtensibleCompilationState, while
  // compiling a script.
  GenericAtom(FrontendContext* fc, ParserAtomsTable& parserAtoms,
              CompilationAtomCache& atomCache, TaggedParserAtomIndex index);

  // Constructors for atoms managed by a CompilationStencil or a
  // BorrowingCompilationStencil, which provide contextual information from an
  // already compiled script.
  GenericAtom(const CompilationStencil& context, TaggedParserAtomIndex index);
  GenericAtom(ScopeStencilRef& scope, TaggedParserAtomIndex index);
  GenericAtom(const FakeStencilGlobalScope& scope, TaggedParserAtomIndex index)
      : ref((JSAtom*)nullptr) {
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE();
  }

  // Constructor for atoms managed by the Garbage Collector, while providing
  // contextual scope information when delazifying functions on the main thread.
  GenericAtom(const Scope*, JSAtom* ptr) : GenericAtom(ptr) {}
  explicit GenericAtom(JSAtom* ptr) : ref(ptr) { hash = ptr->hash(); }

  bool operator==(const GenericAtom& other) const;
};

template <typename NameT>
struct BindingHasher;

template <>
struct BindingHasher<TaggedParserAtomIndex> {
  // This is a GenericAtom::StencilName stripped from its context which is the
  // same for every key.
  using Key = TaggedParserAtomIndex;
  struct Lookup {
    // When building a BindingMap, we assume that the TaggedParserAtomIndex is
    // coming from an existing Stencil, and is not an EmitterName.
    const CompilationStencil& keyStencil;
    GenericAtom other;

    Lookup(ScopeStencilRef& scope_ref, const GenericAtom& other);
    Lookup(const FakeStencilGlobalScope& scope_ref, const GenericAtom& other)
        : keyStencil(mozilla::MakeCompilerAssumeUnreachableFakeValue<
                     const CompilationStencil&>()),
          other(other) {
      MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE();
    }
  };

  static HashNumber hash(const Lookup& aLookup) { return aLookup.other.hash; }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    GenericAtom key(aLookup.keyStencil, aKey);
    return key == aLookup.other;
  }
};

template <>
struct BindingHasher<JSAtom*> {
  using Key = JSAtom*;
  struct Lookup {
    GenericAtom other;

    template <typename Any>
    Lookup(const Any&, const GenericAtom& other) : other(other) {}
  };

  static HashNumber hash(const Lookup& aLookup) { return aLookup.other.hash; }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    GenericAtom key(aKey);
    return key == aLookup.other;
  }
};

// Map the bound names to their respective name location. This is used to avoid
// doing a linear lookup over the list of bindings each time we are looking for
// a single name.
//
// The names given used as a key are either JSAtom in the case of a on-demand
// delazification, or a TaggedParserAtomIndex in case of a
// concurrent-delazification. In both case Lookup arguments are not trivially
// created out of a key, as in the case of a TaggedParserAtomIndex, the
// CompilationStencil should be provided to interpret the TaggedParserAtomIndex
// which are stored in this hash table.
template <typename NameT>
struct BindingMap {
  using Lookup = typename BindingHasher<NameT>::Lookup;
  using Map =
      HashMap<NameT, NameLocation, BindingHasher<NameT>, js::SystemAllocPolicy>;

  Map hashMap;
  mozilla::Maybe<NameLocation> catchAll;
};

// For each list of bound names, map the list of bound names to the hash table
// which is used to reduce the time needed per lookup.
//
// The name parameter are either JSAtom in the case of a on-demand
// delazification, or a TaggedParserAtomIndex in case of a
// concurrent-delazification.
template <typename NameT, typename ScopeT = NameT>
using ScopeBindingMap =
    HashMap<AbstractBaseScopeData<ScopeT>*, BindingMap<NameT>,
            DefaultHasher<AbstractBaseScopeData<ScopeT>*>,
            js::SystemAllocPolicy>;

// Common interface for a cache holding the mapping of Scope to a hash table
// which mirror the binding mapping stored in the scope.
class ScopeBindingCache {
 public:
  using CacheGeneration = size_t;

  virtual CacheGeneration getCurrentGeneration() const = 0;

  // Check whether the cache provided as argument is capable of storing the type
  // of scope given as arguments.
  virtual bool canCacheFor(Scope* ptr);
  virtual bool canCacheFor(ScopeStencilRef ref);
  virtual bool canCacheFor(const FakeStencilGlobalScope& ref);

  // Create a new BindingMap cache for a given scope. This cache should then be
  // filled with all names which might be looked up.
  virtual BindingMap<JSAtom*>* createCacheFor(Scope* ptr);
  virtual BindingMap<TaggedParserAtomIndex>* createCacheFor(
      ScopeStencilRef ref);
  virtual BindingMap<TaggedParserAtomIndex>* createCacheFor(
      const FakeStencilGlobalScope& ref);

  // Return the BindingMap created for the associated scope, unless the
  // generation value does not match the one stored internally, in which case a
  // null pointer is always returned.
  virtual BindingMap<JSAtom*>* lookupScope(Scope* ptr, CacheGeneration gen);
  virtual BindingMap<TaggedParserAtomIndex>* lookupScope(ScopeStencilRef ref,
                                                         CacheGeneration gen);
  virtual BindingMap<TaggedParserAtomIndex>* lookupScope(
      const FakeStencilGlobalScope& ref, CacheGeneration gen);
};

// NoScopeBindingCache is a no-op which does not implement a ScopeBindingCache.
//
// This is useful when compiling a global script or module, where we are not
// interested in looking up anything from the enclosing scope chain.
class NoScopeBindingCache final : public ScopeBindingCache {
 public:
  CacheGeneration getCurrentGeneration() const override { return 1; };

  bool canCacheFor(Scope* ptr) override;
  bool canCacheFor(ScopeStencilRef ref) override;
  bool canCacheFor(const FakeStencilGlobalScope& ref) override;
};

// StencilScopeBindingCache provides an interface to cache the bindings provided
// by a CompilationStencilMerger.
//
// This cache lives on the stack and its content would be invalidated once going
// out of scope. The constructor expects a reference to a
// CompilationStencilMerger, that is expected to:
//   - out-live this class.
//   - contain the enclosing scope which are manipulated by this class.
//   - be the receiver of delazified functions.
class MOZ_STACK_CLASS StencilScopeBindingCache final
    : public ScopeBindingCache {
  ScopeBindingMap<TaggedParserAtomIndex> scopeMap;
#ifdef DEBUG
  const CompilationStencilMerger& merger_;
#endif

 public:
  explicit StencilScopeBindingCache(const CompilationStencilMerger& merger)
#ifdef DEBUG
      : merger_(merger)
#endif
  {
  }

  // The cache content is always valid as long as it does not out-live the
  // CompilationStencilMerger. No need for a generation number.
  CacheGeneration getCurrentGeneration() const override { return 1; }

  bool canCacheFor(ScopeStencilRef ref) override;
  bool canCacheFor(const FakeStencilGlobalScope& ref) override;

  BindingMap<TaggedParserAtomIndex>* createCacheFor(
      ScopeStencilRef ref) override;
  BindingMap<TaggedParserAtomIndex>* createCacheFor(
      const FakeStencilGlobalScope& ref) override;

  BindingMap<TaggedParserAtomIndex>* lookupScope(ScopeStencilRef ref,
                                                 CacheGeneration gen) override;
  BindingMap<TaggedParserAtomIndex>* lookupScope(
      const FakeStencilGlobalScope& ref, CacheGeneration gen) override;
};

// RuntimeScopeBindingCache is used to hold the binding map for each scope which
// is hold by a Scope managed by the garbage collector.
//
// This cache is not thread safe.
//
// The generation number is used to assert the validity of the cached content.
// During a GC, the cached content is thrown away and getCurrentGeneration
// returns a different number. When the generation number differs from the
// initialization of the cached content, the cache content might be renewed or
// ignored.
class RuntimeScopeBindingCache final : public ScopeBindingCache {
  ScopeBindingMap<JSAtom*, JSAtom> scopeMap;

  // This value is initialized to 1, such that we can differentiate it from the
  // typical 0-init of size_t values, when non-initialized.
  size_t cacheGeneration = 1;

 public:
  CacheGeneration getCurrentGeneration() const override {
    return cacheGeneration;
  }

  bool canCacheFor(Scope* ptr) override;
  BindingMap<JSAtom*>* createCacheFor(Scope* ptr) override;
  BindingMap<JSAtom*>* lookupScope(Scope* ptr, CacheGeneration gen) override;

  // The ScopeBindingCache is not instrumented for tracing weakly the keys used
  // for mapping to the NameLocation. Instead, we always purge during compaction
  // or collection, and increment the cacheGeneration to notify all consumers
  // that the cache can no longer be used without being re-populated.
  void purge() {
    cacheGeneration++;
    scopeMap.clearAndCompact();
  }
};

}  // namespace frontend
}  // namespace js

#endif  // frontend_ScopeBindingCache_h
