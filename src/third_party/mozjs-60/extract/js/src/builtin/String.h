/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_String_h
#define builtin_String_h

#include "mozilla/HashFunctions.h"
#include "mozilla/PodOperations.h"

#include <stdio.h>
#include <string.h>

#include "jsutil.h"
#include "NamespaceImports.h"

#include "gc/Rooting.h"
#include "js/RootingAPI.h"
#include "js/UniquePtr.h"
#include "util/Unicode.h"
#include "vm/Printer.h"

namespace js {

/* Initialize the String class, returning its prototype object. */
extern JSObject*
InitStringClass(JSContext* cx, HandleObject obj);

extern bool
str_fromCharCode(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_fromCharCode_one_arg(JSContext* cx, HandleValue code, MutableHandleValue rval);

extern bool
str_fromCodePoint(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_fromCodePoint_one_arg(JSContext* cx, HandleValue code, MutableHandleValue rval);

/* String methods exposed so they can be installed in the self-hosting global. */

extern bool
str_includes(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_indexOf(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_lastIndexOf(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_startsWith(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_toLowerCase(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_toUpperCase(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_toString(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_charAt(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_charCodeAt_impl(JSContext* cx, HandleString string, HandleValue index, MutableHandleValue res);

extern bool
str_charCodeAt(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_contains(JSContext *cx, unsigned argc, Value *vp);

extern bool
str_endsWith(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_trim(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_trimStart(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_trimEnd(JSContext* cx, unsigned argc, Value* vp);

/**
 * Returns the input string converted to lower case based on the language
 * specific case mappings for the input locale.
 *
 * This function only works #if EXPOSE_INTL_API; if not, it will *crash*.
 * Govern yourself accordingly.
 *
 * Usage: lowerCase = intl_toLocaleLowerCase(string, locale)
 */
extern MOZ_MUST_USE bool
intl_toLocaleLowerCase(JSContext* cx, unsigned argc, Value* vp);

/**
 * Returns the input string converted to upper case based on the language
 * specific case mappings for the input locale.
 *
 * This function only works #if EXPOSE_INTL_API; if not, it will *crash*.
 * Govern yourself accordingly.
 *
 * Usage: upperCase = intl_toLocaleUpperCase(string, locale)
 */
extern MOZ_MUST_USE bool
intl_toLocaleUpperCase(JSContext* cx, unsigned argc, Value* vp);

#if EXPOSE_INTL_API

// When the Intl API is exposed, String.prototype.to{Lower,Upper}Case is
// self-hosted.  The core functionality is provided by the intrinsics above.

#else

// When the Intl API is not exposed, String.prototype.to{Lower,Upper}Case are
// implemented in C++.

extern bool
str_toLocaleLowerCase(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_toLocaleUpperCase(JSContext* cx, unsigned argc, Value* vp);

#endif // EXPOSE_INTL_API

#if EXPOSE_INTL_API

// String.prototype.normalize is only implementable if ICU's normalization
// functionality is available.
extern bool
str_normalize(JSContext* cx, unsigned argc, Value* vp);

#endif // EXPOSE_INTL_API

#if EXPOSE_INTL_API

// String.prototype.localeCompare is self-hosted when Intl functionality is
// exposed, and the only intrinsics it requires are provided in the
// implementation of Intl.Collator.

#else

// String.prototype.localeCompare is implemented in C++ (delegating to
// JSLocaleCallbacks) when Intl functionality is not exposed.

extern bool
str_localeCompare(JSContext* cx, unsigned argc, Value* vp);

#endif // EXPOSE_INTL_API

extern bool
str_concat(JSContext* cx, unsigned argc, Value* vp);

ArrayObject*
str_split_string(JSContext* cx, HandleObjectGroup group, HandleString str, HandleString sep,
                 uint32_t limit);

JSString *
str_flat_replace_string(JSContext *cx, HandleString string, HandleString pattern,
                        HandleString replacement);

JSString*
str_replace_string_raw(JSContext* cx, HandleString string, HandleString pattern,
                       HandleString replacement);

extern JSString*
StringToLowerCase(JSContext* cx, HandleString string);

extern JSString*
StringToUpperCase(JSContext* cx, HandleString string);

extern bool
StringConstructor(JSContext* cx, unsigned argc, Value* vp);

extern bool
FlatStringMatch(JSContext* cx, unsigned argc, Value* vp);

extern bool
FlatStringSearch(JSContext* cx, unsigned argc, Value* vp);

} /* namespace js */

#endif /* builtin_String_h */
