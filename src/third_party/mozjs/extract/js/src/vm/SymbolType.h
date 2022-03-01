/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SymbolType_h
#define vm_SymbolType_h

#include <stdio.h>

#include "jsapi.h"

#include "gc/Barrier.h"
#include "gc/Tracer.h"
#include "js/AllocPolicy.h"
#include "js/GCHashTable.h"
#include "js/HeapAPI.h"
#include "js/RootingAPI.h"
#include "js/shadow/Symbol.h"  // JS::shadow::Symbol
#include "js/Symbol.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"
#include "vm/Printer.h"
#include "vm/StringType.h"

namespace js {
class AutoAccessAtomsZone;
}  // namespace js

namespace JS {

class Symbol
    : public js::gc::CellWithTenuredGCPointer<js::gc::TenuredCell, JSAtom> {
 public:
  // User description of symbol, stored in the cell header.
  JSAtom* description() const { return headerPtr(); }

 private:
  SymbolCode code_;

  // Each Symbol gets its own hash code so that we don't have to use
  // addresses as hash codes (a security hazard).
  js::HashNumber hash_;

  Symbol(SymbolCode code, js::HashNumber hash, JSAtom* desc)
      : CellWithTenuredGCPointer(desc), code_(code), hash_(hash) {}

  Symbol(const Symbol&) = delete;
  void operator=(const Symbol&) = delete;

  static Symbol* newInternal(JSContext* cx, SymbolCode code,
                             js::HashNumber hash, js::HandleAtom description);

  static void staticAsserts() {
    static_assert(uint32_t(SymbolCode::WellKnownAPILimit) ==
                      JS::shadow::Symbol::WellKnownAPILimit,
                  "JS::shadow::Symbol::WellKnownAPILimit must match "
                  "SymbolCode::WellKnownAPILimit");
    static_assert(
        offsetof(Symbol, code_) == offsetof(JS::shadow::Symbol, code_),
        "JS::shadow::Symbol::code_ offset must match SymbolCode::code_");
  }

 public:
  static Symbol* new_(JSContext* cx, SymbolCode code,
                      js::HandleString description);
  static Symbol* for_(JSContext* cx, js::HandleString description);

  SymbolCode code() const { return code_; }
  js::HashNumber hash() const { return hash_; }

  bool isWellKnownSymbol() const {
    return uint32_t(code_) < WellKnownSymbolLimit;
  }

  // An "interesting symbol" is a well-known symbol, like @@toStringTag,
  // that's often looked up on random objects but is usually not present. We
  // optimize this by setting a flag on the object's BaseShape when such
  // symbol properties are added, so we can optimize lookups on objects that
  // don't have the BaseShape flag.
  bool isInterestingSymbol() const {
    return code_ == SymbolCode::toStringTag || code_ == SymbolCode::toPrimitive;
  }

  // Symbol created for the #PrivateName syntax.
  bool isPrivateName() const { return code_ == SymbolCode::PrivateNameSymbol; }

  static const JS::TraceKind TraceKind = JS::TraceKind::Symbol;

  inline void traceChildren(JSTracer* trc) {
    js::TraceNullableCellHeaderEdge(trc, this, "symbol description");
  }
  inline void finalize(JSFreeOp*) {}

  // Override base class implementation to tell GC about well-known symbols.
  bool isPermanentAndMayBeShared() const { return isWellKnownSymbol(); }

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this);
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump();  // Debugger-friendly stderr dump.
  void dump(js::GenericPrinter& out);
#endif
};

} /* namespace JS */

namespace js {

/* Hash policy used by the SymbolRegistry. */
struct HashSymbolsByDescription {
  using Key = JS::Symbol*;
  using Lookup = JSAtom*;

  static HashNumber hash(Lookup l) { return HashNumber(l->hash()); }
  static bool match(Key sym, Lookup l) { return sym->description() == l; }
};

/*
 * [SMDOC] Symbol.for() registry (ES6 GlobalSymbolRegistry)
 *
 * The runtime-wide symbol registry, used to implement Symbol.for().
 *
 * ES6 draft rev 25 (2014 May 22) calls this the GlobalSymbolRegistry List. In
 * our implementation, it is not global. There is one per JSRuntime. The
 * symbols in the symbol registry, like all symbols, are allocated in the atoms
 * compartment and can be directly referenced from any compartment. They are
 * never shared across runtimes.
 *
 * The memory management strategy here is modeled after js::AtomSet. It's like
 * a WeakSet. The registry itself does not keep any symbols alive; when a
 * symbol in the registry is collected, the registry entry is removed. No GC
 * nondeterminism is exposed to scripts, because there is no API for
 * enumerating the symbol registry, querying its size, etc.
 */
class SymbolRegistry
    : public GCHashSet<WeakHeapPtrSymbol, HashSymbolsByDescription,
                       SystemAllocPolicy> {
 public:
  SymbolRegistry() = default;
};

// ES6 rev 27 (2014 Aug 24) 19.4.3.3
bool SymbolDescriptiveString(JSContext* cx, JS::Symbol* sym,
                             JS::MutableHandleValue result);

} /* namespace js */

#endif /* vm_SymbolType_h */
