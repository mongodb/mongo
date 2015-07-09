/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Symbol_h
#define vm_Symbol_h

#include "mozilla/Attributes.h"

#include <stdio.h>

#include "jsalloc.h"
#include "jsapi.h"

#include "gc/Barrier.h"

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

namespace JS {

class Symbol : public js::gc::TenuredCell
{
  private:
    SymbolCode code_;
    JSAtom* description_;

    // The minimum allocation size is sizeof(JSString): 16 bytes on 32-bit
    // architectures and 24 bytes on 64-bit.  8 bytes of padding makes Symbol
    // the minimum size on both.
    uint64_t unused2_;

    Symbol(SymbolCode code, JSAtom* desc)
        : code_(code), description_(desc) {}

    Symbol(const Symbol&) = delete;
    void operator=(const Symbol&) = delete;

    static Symbol*
    newInternal(js::ExclusiveContext* cx, SymbolCode code, JSAtom* description);

  public:
    static Symbol* new_(js::ExclusiveContext* cx, SymbolCode code, JSString* description);
    static Symbol* for_(js::ExclusiveContext* cx, js::HandleString description);

    JSAtom* description() const { return description_; }
    SymbolCode code() const { return code_; }

    bool isWellKnownSymbol() const { return uint32_t(code_) < WellKnownSymbolLimit; }

    static inline js::ThingRootKind rootKind() { return js::THING_ROOT_SYMBOL; }
    inline void markChildren(JSTracer* trc);
    inline void finalize(js::FreeOp*) {}

#ifdef DEBUG
    void dump(FILE* fp = stderr);
#endif
};

} /* namespace JS */

namespace js {

/* Hash policy used by the SymbolRegistry. */
struct HashSymbolsByDescription
{
    typedef JS::Symbol* Key;
    typedef JSAtom* Lookup;

    static HashNumber hash(Lookup l) {
        return HashNumber(reinterpret_cast<uintptr_t>(l));
    }
    static bool match(Key sym, Lookup l) {
        return sym->description() == l;
    }
};

/*
 * Hash table that implements the symbol registry.
 *
 * This must be a typedef for the benefit of GCC 4.4.6 (used to build B2G for Ice
 * Cream Sandwich).
 */
typedef HashSet<ReadBarrieredSymbol, HashSymbolsByDescription, SystemAllocPolicy> SymbolHashSet;

/*
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
class SymbolRegistry : public SymbolHashSet
{
  public:
    SymbolRegistry() : SymbolHashSet() {}
    void sweep();
};

} /* namespace js */

namespace js {

// ES6 rev 27 (2014 Aug 24) 19.4.3.3
bool
SymbolDescriptiveString(JSContext* cx, JS::Symbol* sym, JS::MutableHandleValue result);

bool
IsSymbolOrSymbolWrapper(JS::Value v);

JS::Symbol*
ToSymbolPrimitive(JS::Value v);

} /* namespace js */

#endif /* vm_Symbol_h */
