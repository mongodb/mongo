/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SymbolType.h"

#include "gc/Allocator.h"
#include "gc/HashUtil.h"
#include "util/StringBuffer.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"

#include "vm/Realm-inl.h"

using JS::Symbol;
using namespace js;

Symbol* Symbol::newInternal(JSContext* cx, JS::SymbolCode code, uint32_t hash,
                            Handle<JSAtom*> description) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
  AutoAllocInAtomsZone az(cx);
  return cx->newCell<Symbol>(code, hash, description);
}

Symbol* Symbol::new_(JSContext* cx, JS::SymbolCode code,
                     HandleString description) {
  Rooted<JSAtom*> atom(cx);
  if (description) {
    atom = AtomizeString(cx, description);
    if (!atom) {
      return nullptr;
    }
  }

  Symbol* sym = newInternal(cx, code, cx->runtime()->randomHashCode(), atom);
  if (sym) {
    cx->markAtom(sym);
  }
  return sym;
}

Symbol* Symbol::newWellKnown(JSContext* cx, JS::SymbolCode code,
                             Handle<PropertyName*> description) {
  return newInternal(cx, code, cx->runtime()->randomHashCode(), description);
}

Symbol* Symbol::for_(JSContext* cx, HandleString description) {
  Rooted<JSAtom*> atom(cx, AtomizeString(cx, description));
  if (!atom) {
    return nullptr;
  }

  SymbolRegistry& registry = cx->symbolRegistry();
  DependentAddPtr<SymbolRegistry> p(cx, registry, atom);
  if (p) {
    cx->markAtom(*p);
    return *p;
  }

  // Rehash the hash of the atom to give the corresponding symbol a hash
  // that is different than the hash of the corresponding atom.
  HashNumber hash = mozilla::HashGeneric(atom->hash());
  Symbol* sym = newInternal(cx, SymbolCode::InSymbolRegistry, hash, atom);
  if (!sym) {
    return nullptr;
  }

  if (!p.add(cx, registry, atom, sym)) {
    return nullptr;
  }

  cx->markAtom(sym);
  return sym;
}

#if defined(DEBUG) || defined(JS_JITSPEW)
void Symbol::dump() {
  js::Fprinter out(stderr);
  dump(out);
}

void Symbol::dump(js::GenericPrinter& out) {
  if (isWellKnownSymbol()) {
    // All the well-known symbol names are ASCII.
    description()->dumpCharsNoNewline(out);
  } else if (code_ == SymbolCode::InSymbolRegistry ||
             code_ == SymbolCode::UniqueSymbol) {
    out.printf(code_ == SymbolCode::InSymbolRegistry ? "Symbol.for("
                                                     : "Symbol(");

    if (description()) {
      description()->dumpCharsNoNewline(out);
    } else {
      out.printf("undefined");
    }

    out.putChar(')');

    if (code_ == SymbolCode::UniqueSymbol) {
      out.printf("@%p", (void*)this);
    }
  } else if (code_ == SymbolCode::PrivateNameSymbol) {
    MOZ_ASSERT(description());
    out.putChar('#');
    description()->dumpCharsNoNewline(out);
    out.printf("@%p", (void*)this);
  } else {
    out.printf("<Invalid Symbol code=%u>", unsigned(code_));
  }
}
#endif  // defined(DEBUG) || defined(JS_JITSPEW)

bool js::SymbolDescriptiveString(JSContext* cx, Symbol* sym,
                                 MutableHandleValue result) {
  // steps 2-5
  JSStringBuilder sb(cx);
  if (!sb.append("Symbol(")) {
    return false;
  }
  if (JSAtom* desc = sym->description()) {
    if (!sb.append(desc)) {
      return false;
    }
  }
  if (!sb.append(')')) {
    return false;
  }

  // step 6
  JSString* str = sb.finishString();
  if (!str) {
    return false;
  }
  result.setString(str);
  return true;
}

JS::ubi::Node::Size JS::ubi::Concrete<JS::Symbol>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  // If we start allocating symbols in the nursery, we will need to update
  // this method.
  MOZ_ASSERT(get().isTenured());
  return js::gc::Arena::thingSize(get().asTenured().getAllocKind());
}
