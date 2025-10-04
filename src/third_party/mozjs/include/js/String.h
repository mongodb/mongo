/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript string operations. */

#ifndef js_String_h
#define js_String_h

#include "js/shadow/String.h"  // JS::shadow::String

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_ALWAYS_INLINE
#include "mozilla/Likely.h"      // MOZ_LIKELY
#include "mozilla/Maybe.h"       // mozilla::Maybe
#include "mozilla/Range.h"       // mozilla::Range
#include "mozilla/Span.h"        // mozilla::Span
                                 // std::tuple

#include <algorithm>  // std::copy_n
#include <stddef.h>   // size_t
#include <stdint.h>   // uint32_t, uint64_t, INT32_MAX

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/CharacterEncoding.h"  // JS::UTF8Chars, JS::ConstUTF8CharsZ
#include "js/RootingAPI.h"         // JS::Handle
#include "js/TypeDecls.h"          // JS::Latin1Char
#include "js/UniquePtr.h"          // JS::UniquePtr
#include "js/Utility.h"            // JS::FreePolicy, JS::UniqueTwoByteChars
#include "js/Value.h"              // JS::Value

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSAtom;
class JSLinearString;
class JS_PUBLIC_API JSString;

namespace JS {

class JS_PUBLIC_API AutoRequireNoGC;

}  // namespace JS

extern JS_PUBLIC_API JSString* JS_GetEmptyString(JSContext* cx);

// Don't want to export data, so provide accessors for non-inline Values.
extern JS_PUBLIC_API JS::Value JS_GetEmptyStringValue(JSContext* cx);

/*
 * String creation.
 *
 * NB: JS_NewUCString takes ownership of bytes on success, avoiding a copy;
 * but on error (signified by null return), it leaves chars owned by the
 * caller. So the caller must free bytes in the error case, if it has no use
 * for them. In contrast, all the JS_New*StringCopy* functions do not take
 * ownership of the character memory passed to them -- they copy it.
 */

extern JS_PUBLIC_API JSString* JS_NewStringCopyN(JSContext* cx, const char* s,
                                                 size_t n);

extern JS_PUBLIC_API JSString* JS_NewStringCopyZ(JSContext* cx, const char* s);

extern JS_PUBLIC_API JSString* JS_NewStringCopyUTF8Z(
    JSContext* cx, const JS::ConstUTF8CharsZ s);

extern JS_PUBLIC_API JSString* JS_NewStringCopyUTF8N(JSContext* cx,
                                                     const JS::UTF8Chars& s);

extern JS_PUBLIC_API JSString* JS_AtomizeStringN(JSContext* cx, const char* s,
                                                 size_t length);

extern JS_PUBLIC_API JSString* JS_AtomizeString(JSContext* cx, const char* s);

// Note: unlike the non-pinning JS_Atomize* functions, this can be called
// without entering a realm/zone.
extern JS_PUBLIC_API JSString* JS_AtomizeAndPinStringN(JSContext* cx,
                                                       const char* s,
                                                       size_t length);

// Note: unlike the non-pinning JS_Atomize* functions, this can be called
// without entering a realm/zone.
extern JS_PUBLIC_API JSString* JS_AtomizeAndPinString(JSContext* cx,
                                                      const char* s);

extern JS_PUBLIC_API JSString* JS_NewLatin1String(
    JSContext* cx, js::UniquePtr<JS::Latin1Char[], JS::FreePolicy> chars,
    size_t length);

extern JS_PUBLIC_API JSString* JS_NewUCString(JSContext* cx,
                                              JS::UniqueTwoByteChars chars,
                                              size_t length);

extern JS_PUBLIC_API JSString* JS_NewUCStringDontDeflate(
    JSContext* cx, JS::UniqueTwoByteChars chars, size_t length);

extern JS_PUBLIC_API JSString* JS_NewUCStringCopyN(JSContext* cx,
                                                   const char16_t* s, size_t n);

extern JS_PUBLIC_API JSString* JS_NewUCStringCopyZ(JSContext* cx,
                                                   const char16_t* s);

extern JS_PUBLIC_API JSString* JS_AtomizeUCStringN(JSContext* cx,
                                                   const char16_t* s,
                                                   size_t length);

extern JS_PUBLIC_API JSString* JS_AtomizeUCString(JSContext* cx,
                                                  const char16_t* s);

extern JS_PUBLIC_API bool JS_CompareStrings(JSContext* cx, JSString* str1,
                                            JSString* str2, int32_t* result);

[[nodiscard]] extern JS_PUBLIC_API bool JS_StringEqualsAscii(
    JSContext* cx, JSString* str, const char* asciiBytes, bool* match);

// Same as above, but when the length of asciiBytes (excluding the
// trailing null, if any) is known.
[[nodiscard]] extern JS_PUBLIC_API bool JS_StringEqualsAscii(
    JSContext* cx, JSString* str, const char* asciiBytes, size_t length,
    bool* match);

template <size_t N>
[[nodiscard]] bool JS_StringEqualsLiteral(JSContext* cx, JSString* str,
                                          const char (&asciiBytes)[N],
                                          bool* match) {
  MOZ_ASSERT(asciiBytes[N - 1] == '\0');
  return JS_StringEqualsAscii(cx, str, asciiBytes, N - 1, match);
}

extern JS_PUBLIC_API size_t JS_PutEscapedString(JSContext* cx, char* buffer,
                                                size_t size, JSString* str,
                                                char quote);

/*
 * Extracting string characters and length.
 *
 * While getting the length of a string is infallible, getting the chars can
 * fail. As indicated by the lack of a JSContext parameter, there are two
 * special cases where getting the chars is infallible:
 *
 * The first case is for strings that have been atomized, e.g. directly by
 * JS_AtomizeAndPinString or implicitly because it is stored in a jsid.
 *
 * The second case is "linear" strings that have been explicitly prepared in a
 * fallible context by JS_EnsureLinearString. To catch errors, a separate opaque
 * JSLinearString type is returned by JS_EnsureLinearString and expected by
 * JS_Get{Latin1,TwoByte}StringCharsAndLength. Note, though, that this is purely
 * a syntactic distinction: the input and output of JS_EnsureLinearString are
 * the same actual GC-thing. If a JSString is known to be linear,
 * JS_ASSERT_STRING_IS_LINEAR can be used to make a debug-checked cast. Example:
 *
 *   // In a fallible context.
 *   JSLinearString* lstr = JS_EnsureLinearString(cx, str);
 *   if (!lstr) {
 *     return false;
 *   }
 *   MOZ_ASSERT(lstr == JS_ASSERT_STRING_IS_LINEAR(str));
 *
 *   // In an infallible context, for the same 'str'.
 *   AutoCheckCannotGC nogc;
 *   const char16_t* chars = JS::GetTwoByteLinearStringChars(nogc, lstr)
 *   MOZ_ASSERT(chars);
 *
 * Note: JS strings (including linear strings and atoms) are not
 * null-terminated!
 *
 * Additionally, string characters are stored as either Latin1Char (8-bit)
 * or char16_t (16-bit). Clients can use JS::StringHasLatin1Chars and can then
 * call either the Latin1* or TwoByte* functions. Some functions like
 * JS_CopyStringChars and JS_GetStringCharAt accept both Latin1 and TwoByte
 * strings.
 */

extern JS_PUBLIC_API size_t JS_GetStringLength(JSString* str);

extern JS_PUBLIC_API bool JS_StringIsLinear(JSString* str);

extern JS_PUBLIC_API const JS::Latin1Char* JS_GetLatin1StringCharsAndLength(
    JSContext* cx, const JS::AutoRequireNoGC& nogc, JSString* str,
    size_t* length);

extern JS_PUBLIC_API const char16_t* JS_GetTwoByteStringCharsAndLength(
    JSContext* cx, const JS::AutoRequireNoGC& nogc, JSString* str,
    size_t* length);

extern JS_PUBLIC_API bool JS_GetStringCharAt(JSContext* cx, JSString* str,
                                             size_t index, char16_t* res);

extern JS_PUBLIC_API const char16_t* JS_GetTwoByteExternalStringChars(
    JSString* str);

extern JS_PUBLIC_API bool JS_CopyStringChars(
    JSContext* cx, const mozilla::Range<char16_t>& dest, JSString* str);

/**
 * Copies the string's characters to a null-terminated char16_t buffer.
 *
 * Returns nullptr on OOM.
 */
extern JS_PUBLIC_API JS::UniqueTwoByteChars JS_CopyStringCharsZ(JSContext* cx,
                                                                JSString* str);

extern JS_PUBLIC_API JSLinearString* JS_EnsureLinearString(JSContext* cx,
                                                           JSString* str);

static MOZ_ALWAYS_INLINE JSLinearString* JS_ASSERT_STRING_IS_LINEAR(
    JSString* str) {
  MOZ_ASSERT(JS_StringIsLinear(str));
  return reinterpret_cast<JSLinearString*>(str);
}

static MOZ_ALWAYS_INLINE JSString* JS_FORGET_STRING_LINEARNESS(
    JSLinearString* str) {
  return reinterpret_cast<JSString*>(str);
}

/*
 * Additional APIs that avoid fallibility when given a linear string.
 */

extern JS_PUBLIC_API bool JS_LinearStringEqualsAscii(JSLinearString* str,
                                                     const char* asciiBytes);
extern JS_PUBLIC_API bool JS_LinearStringEqualsAscii(JSLinearString* str,
                                                     const char* asciiBytes,
                                                     size_t length);

template <size_t N>
bool JS_LinearStringEqualsLiteral(JSLinearString* str,
                                  const char (&asciiBytes)[N]) {
  MOZ_ASSERT(asciiBytes[N - 1] == '\0');
  return JS_LinearStringEqualsAscii(str, asciiBytes, N - 1);
}

extern JS_PUBLIC_API size_t JS_PutEscapedLinearString(char* buffer, size_t size,
                                                      JSLinearString* str,
                                                      char quote);

/**
 * Create a dependent string, i.e., a string that owns no character storage,
 * but that refers to a slice of another string's chars.  Dependent strings
 * are mutable by definition, so the thread safety comments above apply.
 */
extern JS_PUBLIC_API JSString* JS_NewDependentString(JSContext* cx,
                                                     JS::Handle<JSString*> str,
                                                     size_t start,
                                                     size_t length);

/**
 * Concatenate two strings, possibly resulting in a rope.
 * See above for thread safety comments.
 */
extern JS_PUBLIC_API JSString* JS_ConcatStrings(JSContext* cx,
                                                JS::Handle<JSString*> left,
                                                JS::Handle<JSString*> right);

/**
 * For JS_DecodeBytes, set *dstlenp to the size of the destination buffer before
 * the call; on return, *dstlenp contains the number of characters actually
 * stored. To determine the necessary destination buffer size, make a sizing
 * call that passes nullptr for dst.
 *
 * On errors, the functions report the error. In that case, *dstlenp contains
 * the number of characters or bytes transferred so far.  If cx is nullptr, no
 * error is reported on failure, and the functions simply return false.
 *
 * NB: This function does not store an additional zero byte or char16_t after
 * the transcoded string.
 */
JS_PUBLIC_API bool JS_DecodeBytes(JSContext* cx, const char* src, size_t srclen,
                                  char16_t* dst, size_t* dstlenp);

/**
 * Get number of bytes in the string encoding (without accounting for a
 * terminating zero bytes. The function returns (size_t) -1 if the string
 * can not be encoded into bytes and reports an error using cx accordingly.
 */
JS_PUBLIC_API size_t JS_GetStringEncodingLength(JSContext* cx, JSString* str);

/**
 * Encode string into a buffer. The function does not stores an additional
 * zero byte. The function returns (size_t) -1 if the string can not be
 * encoded into bytes with no error reported. Otherwise it returns the number
 * of bytes that are necessary to encode the string. If that exceeds the
 * length parameter, the string will be cut and only length bytes will be
 * written into the buffer.
 */
[[nodiscard]] JS_PUBLIC_API bool JS_EncodeStringToBuffer(JSContext* cx,
                                                         JSString* str,
                                                         char* buffer,
                                                         size_t length);

/**
 * Encode as many scalar values of the string as UTF-8 as can fit
 * into the caller-provided buffer replacing unpaired surrogates
 * with the REPLACEMENT CHARACTER.
 *
 * If JS::StringHasLatin1Chars(str) returns true, the function
 * is guaranteed to convert the entire string if
 * buffer.Length() >= 2 * JS_GetStringLength(str). Otherwise,
 * the function is guaranteed to convert the entire string if
 * buffer.Length() >= 3 * JS_GetStringLength(str).
 *
 * This function does not alter the representation of |str| or
 * any |JSString*| substring that is a constituent part of it.
 * Returns mozilla::Nothing() on OOM, without reporting an error;
 * some data may have been written to |buffer| when this happens.
 *
 * If there's no OOM, returns the number of code units read and
 * the number of code units written.
 *
 * The semantics of this method match the semantics of
 * TextEncoder.encodeInto().
 *
 * The function does not store an additional zero byte.
 */
JS_PUBLIC_API mozilla::Maybe<std::tuple<size_t, size_t>>
JS_EncodeStringToUTF8BufferPartial(JSContext* cx, JSString* str,
                                   mozilla::Span<char> buffer);

namespace JS {

/**
 * Maximum length of a JS string. This is chosen so that the number of bytes
 * allocated for a null-terminated TwoByte string still fits in int32_t.
 */
static constexpr uint32_t MaxStringLength = (1 << 30) - 2;

static_assert((uint64_t(MaxStringLength) + 1) * sizeof(char16_t) <= INT32_MAX,
              "size of null-terminated JSString char buffer must fit in "
              "INT32_MAX");

/** Compute the length of a string. */
MOZ_ALWAYS_INLINE size_t GetStringLength(JSString* s) {
  return shadow::AsShadowString(s)->length();
}

/** Compute the length of a linear string. */
MOZ_ALWAYS_INLINE size_t GetLinearStringLength(JSLinearString* s) {
  return shadow::AsShadowString(s)->length();
}

/** Return true iff the given linear string uses Latin-1 storage. */
MOZ_ALWAYS_INLINE bool LinearStringHasLatin1Chars(JSLinearString* s) {
  return shadow::AsShadowString(s)->hasLatin1Chars();
}

/** Return true iff the given string uses Latin-1 storage. */
MOZ_ALWAYS_INLINE bool StringHasLatin1Chars(JSString* s) {
  return shadow::AsShadowString(s)->hasLatin1Chars();
}

/**
 * Given a linear string known to use Latin-1 storage, return a pointer to that
 * storage.  This pointer remains valid only as long as no GC occurs.
 */
MOZ_ALWAYS_INLINE const Latin1Char* GetLatin1LinearStringChars(
    const AutoRequireNoGC& nogc, JSLinearString* linear) {
  return shadow::AsShadowString(linear)->latin1LinearChars();
}

/**
 * Given a linear string known to use two-byte storage, return a pointer to that
 * storage.  This pointer remains valid only as long as no GC occurs.
 */
MOZ_ALWAYS_INLINE const char16_t* GetTwoByteLinearStringChars(
    const AutoRequireNoGC& nogc, JSLinearString* linear) {
  return shadow::AsShadowString(linear)->twoByteLinearChars();
}

/**
 * Given an in-range index into the provided string, return the character at
 * that index.
 */
MOZ_ALWAYS_INLINE char16_t GetLinearStringCharAt(JSLinearString* linear,
                                                 size_t index) {
  shadow::String* s = shadow::AsShadowString(linear);
  MOZ_ASSERT(index < s->length());

  return s->hasLatin1Chars() ? s->latin1LinearChars()[index]
                             : s->twoByteLinearChars()[index];
}

/**
 * Convert an atom to a linear string.  All atoms are linear, so this
 * operation is infallible.
 */
MOZ_ALWAYS_INLINE JSLinearString* AtomToLinearString(JSAtom* atom) {
  return reinterpret_cast<JSLinearString*>(atom);
}

/**
 * If the provided string uses externally-managed latin-1 storage, return true
 * and set |*callbacks| to the external-string callbacks used to create it and
 * |*chars| to a pointer to its latin1 storage.  (These pointers remain valid
 * as long as the provided string is kept alive.)
 */
MOZ_ALWAYS_INLINE bool IsExternalStringLatin1(
    JSString* str, const JSExternalStringCallbacks** callbacks,
    const JS::Latin1Char** chars) {
  shadow::String* s = shadow::AsShadowString(str);

  if (!s->isExternal() || !s->hasLatin1Chars()) {
    return false;
  }

  *callbacks = s->externalCallbacks;
  *chars = s->nonInlineCharsLatin1;
  return true;
}

/**
 * If the provided string uses externally-managed two-byte storage, return true
 * and set |*callbacks| to the external-string callbacks used to create it and
 * |*chars| to a pointer to its two-byte storage.  (These pointers remain valid
 * as long as the provided string is kept alive.)
 */
MOZ_ALWAYS_INLINE bool IsExternalUCString(
    JSString* str, const JSExternalStringCallbacks** callbacks,
    const char16_t** chars) {
  shadow::String* s = shadow::AsShadowString(str);

  if (!s->isExternal() || s->hasLatin1Chars()) {
    return false;
  }

  *callbacks = s->externalCallbacks;
  *chars = s->nonInlineCharsTwoByte;
  return true;
}

namespace detail {

extern JS_PUBLIC_API JSLinearString* StringToLinearStringSlow(JSContext* cx,
                                                              JSString* str);

}  // namespace detail

/** Convert a string to a linear string. */
MOZ_ALWAYS_INLINE JSLinearString* StringToLinearString(JSContext* cx,
                                                       JSString* str) {
  if (MOZ_LIKELY(shadow::AsShadowString(str)->isLinear())) {
    return reinterpret_cast<JSLinearString*>(str);
  }

  return detail::StringToLinearStringSlow(cx, str);
}

/** Copy characters in |s[start..start + len]| to |dest[0..len]|. */
MOZ_ALWAYS_INLINE void CopyLinearStringChars(char16_t* dest, JSLinearString* s,
                                             size_t len, size_t start = 0) {
#ifdef DEBUG
  size_t stringLen = GetLinearStringLength(s);
  MOZ_ASSERT(start <= stringLen);
  MOZ_ASSERT(len <= stringLen - start);
#endif

  shadow::String* str = shadow::AsShadowString(s);

  if (str->hasLatin1Chars()) {
    const Latin1Char* src = str->latin1LinearChars();
    for (size_t i = 0; i < len; i++) {
      dest[i] = src[start + i];
    }
  } else {
    const char16_t* src = str->twoByteLinearChars();
    std::copy_n(src + start, len, dest);
  }
}

/**
 * Copy characters in |s[start..start + len]| to |dest[0..len]|, lossily
 * truncating 16-bit values to |char| if necessary.
 */
MOZ_ALWAYS_INLINE void LossyCopyLinearStringChars(char* dest, JSLinearString* s,
                                                  size_t len,
                                                  size_t start = 0) {
#ifdef DEBUG
  size_t stringLen = GetLinearStringLength(s);
  MOZ_ASSERT(start <= stringLen);
  MOZ_ASSERT(len <= stringLen - start);
#endif

  shadow::String* str = shadow::AsShadowString(s);

  if (LinearStringHasLatin1Chars(s)) {
    const Latin1Char* src = str->latin1LinearChars();
    for (size_t i = 0; i < len; i++) {
      dest[i] = char(src[start + i]);
    }
  } else {
    const char16_t* src = str->twoByteLinearChars();
    for (size_t i = 0; i < len; i++) {
      dest[i] = char(src[start + i]);
    }
  }
}

/**
 * Copy characters in |s[start..start + len]| to |dest[0..len]|.
 *
 * This function is fallible.  If you already have a linear string, use the
 * infallible |JS::CopyLinearStringChars| above instead.
 */
[[nodiscard]] inline bool CopyStringChars(JSContext* cx, char16_t* dest,
                                          JSString* s, size_t len,
                                          size_t start = 0) {
  JSLinearString* linear = StringToLinearString(cx, s);
  if (!linear) {
    return false;
  }

  CopyLinearStringChars(dest, linear, len, start);
  return true;
}

/**
 * Copy characters in |s[start..start + len]| to |dest[0..len]|, lossily
 * truncating 16-bit values to |char| if necessary.
 *
 * This function is fallible.  If you already have a linear string, use the
 * infallible |JS::LossyCopyLinearStringChars| above instead.
 */
[[nodiscard]] inline bool LossyCopyStringChars(JSContext* cx, char* dest,
                                               JSString* s, size_t len,
                                               size_t start = 0) {
  JSLinearString* linear = StringToLinearString(cx, s);
  if (!linear) {
    return false;
  }

  LossyCopyLinearStringChars(dest, linear, len, start);
  return true;
}

}  // namespace JS

/** DO NOT USE, only present for Rust bindings as a temporary hack */
[[deprecated]] extern JS_PUBLIC_API bool JS_DeprecatedStringHasLatin1Chars(
    JSString* str);

// JSString* is an aligned pointer, but this information isn't available in the
// public header. We specialize HasFreeLSB here so that JS::Result<JSString*>
// compiles.

namespace mozilla {
namespace detail {
template <>
struct HasFreeLSB<JSString*> {
  static constexpr bool value = true;
};
}  // namespace detail
}  // namespace mozilla

#endif  // js_String_h
