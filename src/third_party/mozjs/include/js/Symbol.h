/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Symbols. */

#ifndef js_Symbol_h
#define js_Symbol_h

#include "js/shadow/Symbol.h"  // JS::shadow::Symbol::WellKnownAPILimit

#include <stddef.h>  // size_t
#include <stdint.h>  // uintptr_t, uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

namespace JS {

class JS_PUBLIC_API Symbol;

/**
 * Create a new Symbol with the given description. This function never returns
 * a Symbol that is in the Runtime-wide symbol registry.
 *
 * If description is null, the new Symbol's [[Description]] attribute is
 * undefined.
 */
extern JS_PUBLIC_API Symbol* NewSymbol(JSContext* cx,
                                       Handle<JSString*> description);

/**
 * Symbol.for as specified in ES6.
 *
 * Get a Symbol with the description 'key' from the Runtime-wide symbol
 * registry. If there is not already a Symbol with that description in the
 * registry, a new Symbol is created and registered. 'key' must not be null.
 */
extern JS_PUBLIC_API Symbol* GetSymbolFor(JSContext* cx, Handle<JSString*> key);

/**
 * Get the [[Description]] attribute of the given symbol.
 *
 * This function is infallible. If it returns null, that means the symbol's
 * [[Description]] is undefined.
 */
extern JS_PUBLIC_API JSString* GetSymbolDescription(Handle<Symbol*> symbol);

/* Well-known symbols. */
#define JS_FOR_EACH_WELL_KNOWN_SYMBOL(MACRO) \
  MACRO(isConcatSpreadable)                  \
  MACRO(iterator)                            \
  MACRO(match)                               \
  MACRO(replace)                             \
  MACRO(search)                              \
  MACRO(species)                             \
  MACRO(hasInstance)                         \
  MACRO(split)                               \
  MACRO(toPrimitive)                         \
  MACRO(toStringTag)                         \
  MACRO(unscopables)                         \
  MACRO(asyncIterator)                       \
  MACRO(matchAll)

enum class SymbolCode : uint32_t {
// There is one SymbolCode for each well-known symbol.
#define JS_DEFINE_SYMBOL_ENUM(name) name,
  JS_FOR_EACH_WELL_KNOWN_SYMBOL(
      JS_DEFINE_SYMBOL_ENUM)  // SymbolCode::iterator, etc.
#undef JS_DEFINE_SYMBOL_ENUM
  Limit,
  WellKnownAPILimit = JS::shadow::Symbol::WellKnownAPILimit,
  PrivateNameSymbol = 0xfffffffd,  // created by the #PrivateName syntax.
  InSymbolRegistry =
      0xfffffffe,            // created by Symbol.for() or JS::GetSymbolFor()
  UniqueSymbol = 0xffffffff  // created by Symbol() or JS::NewSymbol()
};

/* For use in loops that iterate over the well-known symbols. */
const size_t WellKnownSymbolLimit = size_t(SymbolCode::Limit);

/**
 * Return the SymbolCode telling what sort of symbol `symbol` is.
 *
 * A symbol's SymbolCode never changes once it is created.
 */
extern JS_PUBLIC_API SymbolCode GetSymbolCode(Handle<Symbol*> symbol);

/**
 * Get one of the well-known symbols defined by ES6. A single set of well-known
 * symbols is shared by all compartments in a JSRuntime.
 *
 * `which` must be in the range [0, WellKnownSymbolLimit).
 */
extern JS_PUBLIC_API Symbol* GetWellKnownSymbol(JSContext* cx,
                                                SymbolCode which);

/**
 * Return true if the given JSPropertySpec::name or JSFunctionSpec::name value
 * is actually a symbol code and not a string. See JS_SYM_FN.
 */
inline bool PropertySpecNameIsSymbol(uintptr_t name) {
  return name != 0 && name - 1 < WellKnownSymbolLimit;
}

}  // namespace JS

#endif /* js_Symbol_h */
