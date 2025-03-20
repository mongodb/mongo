/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_StencilCache_h
#define vm_StencilCache_h

#include "mozilla/Atomics.h"        // mozilla::Atomic
#include "mozilla/HashFunctions.h"  // mozilla::HashGeneric
#include "mozilla/RefPtr.h"         // mozilla::RefPtr

#include "js/HashTable.h"  // js::HashTable

#include "threading/ExclusiveData.h"  // js::ExclusiveData

#include "vm/JSScript.h"       // js::ScriptSource
#include "vm/SharedStencil.h"  // js::SourceExtent

struct JS_PUBLIC_API JSContext;  // vm/JSContext.h

namespace js {

namespace frontend {
struct CompilationStencil;            // frontend/CompilationStencil.h
struct ExtensibleCompilationStencil;  // frontend/CompilationStencil.h
} /* namespace frontend */

// Note, the key is a RefPtr<ScriptSource>, but neither the PointerHasher nor
// DefaultHasher seems to handle these correctly.
struct SourceCachePolicy {
  using Lookup = const ScriptSource*;

  static HashNumber hash(const Lookup& l) { return mozilla::HashGeneric(l); }
  static bool match(const Lookup& entry, const Lookup& l) { return entry == l; }
};

// Immutable information to identify a unique function, as well as the
// compilation context which will result in a unique Stencil once compiled.
struct StencilContext {
  // This pointer is used to isolate a single source. Note the uniqueness of the
  // ReadOnlyCompileOptions is implied by the fact that we allocate a new
  // ScriptSource for each CompilationInput, which are initialized with a set of
  // CompileOptions.
  RefPtr<ScriptSource> source;

  SourceExtent::FunctionKey funKey;

  StencilContext(RefPtr<ScriptSource>& source, SourceExtent extent)
      : source(source), funKey(extent.toFunctionKey()) {}
};

struct StencilCachePolicy {
  using Lookup = StencilContext;

  static HashNumber hash(const Lookup& l) {
    const ScriptSource* raw = l.source;
    return mozilla::HashGeneric(raw, l.funKey);
  }
  static bool match(const Lookup& entry, const Lookup& l) {
    return entry.source == l.source && entry.funKey == l.funKey;
  }
};

// Cache stencils which are parsed from the same source, and with identical
// compilation options.
//
// This cache does not check the principals, as the source should not be
// aliased across different principals.
//
// The content provided by this cache is computed by delazification tasks. The
// delazification task needs contextual information to generate Stencils, which
// are then registered in this cache.
//
// To reclaim memory from this cache, the producers should be shutdown and the
// cache should be cleared. As the delazification process needs contextual
// information to generate the Stencils, it is impossible to resume without
// either keeping memory or recomputing from the beginning.
//
// Therefore, this cache cannot be cleared without disabling it at the same
// time. Disabling this cache ends all threads which attempts to add new
// stencils.
//
// The cache can be in multiple states:
//
// - Cache disabled:
//
//     The cache can be disabled when running in a steady state for some time,
//     or after reaching a shrinking GC, which reclaims the memory from this
//     cache.
//
//     This state is expected for the steady state of web pages, once the
//     pages are loaded for a while and that we do not expect a huge load of
//     delazification.
//
// - Cache enabled, Source is not cached:
//
//     This is a rare case which can happen either when testing the
//     StencilCache, or after some dynamic load of JavaScript code in a page
//     which already reached as steady state. In which case, we want to prevent
//     locking for any functions which are not part of the newly loaded source.
//
//     This might also happen for eval-ed or inline JavaScript, which is
//     parsed on the main thread, and also delazified on the main thread.
//
// - Cache enabled, Source is cached:
//
//     This case is expected to be frequent for any new document. A new
//     document will register many sources for parsing off-thread, and
//     triggering off-thread delazification.
//
//     All newly parse sources will be registered here until a steady state is
//     reached, or a shrinking GC is called.
class StencilCache {
  using SourceSet =
      js::HashSet<RefPtr<ScriptSource>, SourceCachePolicy, SystemAllocPolicy>;
  using StencilMap =
      js::HashMap<StencilContext, RefPtr<frontend::CompilationStencil>,
                  StencilCachePolicy, SystemAllocPolicy>;

  struct CacheData {
    // Sources which are recorded in this cache.
    SourceSet watched;
    // Stencils of functions which are recorded in this cache.
    StencilMap functions;
  };

  // Map a function to its CompilationStencil.
  ExclusiveData<CacheData> cache;

  // This flag is mostly read, and changes rarely. We use this Atomic to avoid
  // locking a Mutex when the cache is disabled.
  //
  // The cache can be disabled when running in a steady state for some time, or
  // after reaching a shrinking GC, which reclaims the memory from this cache
  // and indirectly ends the threads which are producing stencils for this
  // cache.
  mozilla::Atomic<bool, mozilla::ReleaseAcquire> enabled;

 public:
  StencilCache();

  // An access key is returned when checking whether the cache is enabled. It
  // should be used in an if statement and provided to all follow-up functions
  // if it is true-ish.
  using AccessKey = ExclusiveData<CacheData>::NullableGuard;

  // Not all stencils should be cached, we use the ScriptSource pointer to
  // identify whether we should look further or not in the cache.
  AccessKey isSourceCached(ScriptSource* src);

  // Register a source in the cache in order to cache any stencil associated
  // with this source in the future. To stop caching, the function
  // clearAndDisable can be used.
  //
  // Note: This function should be called once per source. Which is usualy after
  // creating it.
  [[nodiscard]] bool startCaching(RefPtr<ScriptSource>&& src);

  // Checks if the cache contains a specific stencil and returns a pointer to
  // it if it does. Otherwise, returns nullptr.
  frontend::CompilationStencil* lookup(AccessKey& guard,
                                       const StencilContext& key);

  // Adds a newly compiled stencil to the cache. The cache should not contain
  // any entry for this function before calling this function.
  [[nodiscard]] bool putNew(AccessKey& guard, const StencilContext& key,
                            frontend::CompilationStencil* value);

  // Prevent any further stencil from being cached, and clear and reclaim the
  // memory of all stencil held by the cache.
  //
  // WARNING: This function should not be called within a scope checking for
  // isSourceCached, as this would cause a dead-lock.
  void clearAndDisable();
};

// This cache is used to store the result of delazification compilations which
// might be happening off-thread. The main-thread will concurrently read the
// content of this cache to avoid delazification, or fallback on running the
// delazification on the main-thread.
//
// Main-thread results are not stored in the DelazificationCache as there is no
// other consumer.
class DelazificationCache : public StencilCache {
  static DelazificationCache singleton;

 public:
  DelazificationCache() = default;

  static DelazificationCache& getSingleton() { return singleton; }
};

} /* namespace js */

#endif /* vm_StencilCache_h */
