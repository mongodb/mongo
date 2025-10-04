/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PropertyKey_h
#define vm_PropertyKey_h

#include "mozilla/HashFunctions.h"  // mozilla::HashGeneric

#include "NamespaceImports.h"  // js::PropertyKey

#include "js/HashTable.h"   // js::DefaultHasher
#include "js/Id.h"          // JS::PropertyKey
#include "vm/StringType.h"  // JSAtom::hash
#include "vm/SymbolType.h"  // JS::Symbol::hash

namespace js {

static MOZ_ALWAYS_INLINE HashNumber HashPropertyKey(PropertyKey key) {
  // HashGeneric alone would work, but bits of atom and symbol addresses
  // could then be recovered from the hash code. See bug 1330769.
  if (MOZ_LIKELY(key.isAtom())) {
    return key.toAtom()->hash();
  }
  if (key.isSymbol()) {
    return key.toSymbol()->hash();
  }
  return mozilla::HashGeneric(key.asRawBits());
}

// Like HashPropertyKey but optimized for callers that only use atom or symbol
// keys.
static MOZ_ALWAYS_INLINE HashNumber
HashAtomOrSymbolPropertyKey(PropertyKey key) {
  if (MOZ_LIKELY(key.isAtom())) {
    return key.toAtom()->hash();
  }
  return key.toSymbol()->hash();
}

}  // namespace js

namespace mozilla {

template <>
struct DefaultHasher<JS::PropertyKey> {
  using Lookup = JS::PropertyKey;
  static HashNumber hash(JS::PropertyKey key) {
    return js::HashPropertyKey(key);
  }
  static bool match(JS::PropertyKey key1, JS::PropertyKey key2) {
    return key1 == key2;
  }
};

}  // namespace mozilla

#endif /* vm_PropertyKey_h */
