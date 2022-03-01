/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_CharacterEncoding_h
#define js_CharacterEncoding_h

#include "mozilla/Range.h"
#include "mozilla/Span.h"

#include "js/TypeDecls.h"
#include "js/Utility.h"

class JSLinearString;

namespace mozilla {
union Utf8Unit;
}

namespace JS {

/*
 * By default, all C/C++ 1-byte-per-character strings passed into the JSAPI
 * are treated as ISO/IEC 8859-1, also known as Latin-1. That is, each
 * byte is treated as a 2-byte character, and there is no way to pass in a
 * string containing characters beyond U+00FF.
 */
class Latin1Chars : public mozilla::Range<Latin1Char> {
  typedef mozilla::Range<Latin1Char> Base;

 public:
  using CharT = Latin1Char;

  Latin1Chars() = default;
  Latin1Chars(char* aBytes, size_t aLength)
      : Base(reinterpret_cast<Latin1Char*>(aBytes), aLength) {}
  Latin1Chars(const Latin1Char* aBytes, size_t aLength)
      : Base(const_cast<Latin1Char*>(aBytes), aLength) {}
  Latin1Chars(const char* aBytes, size_t aLength)
      : Base(reinterpret_cast<Latin1Char*>(const_cast<char*>(aBytes)),
             aLength) {}
};

/*
 * Like Latin1Chars, but the chars are const.
 */
class ConstLatin1Chars : public mozilla::Range<const Latin1Char> {
  typedef mozilla::Range<const Latin1Char> Base;

 public:
  using CharT = Latin1Char;

  ConstLatin1Chars() = default;
  ConstLatin1Chars(const Latin1Char* aChars, size_t aLength)
      : Base(aChars, aLength) {}
};

/*
 * A Latin1Chars, but with \0 termination for C compatibility.
 */
class Latin1CharsZ : public mozilla::RangedPtr<Latin1Char> {
  typedef mozilla::RangedPtr<Latin1Char> Base;

 public:
  using CharT = Latin1Char;

  Latin1CharsZ() : Base(nullptr, 0) {}  // NOLINT

  Latin1CharsZ(char* aBytes, size_t aLength)
      : Base(reinterpret_cast<Latin1Char*>(aBytes), aLength) {
    MOZ_ASSERT(aBytes[aLength] == '\0');
  }

  Latin1CharsZ(Latin1Char* aBytes, size_t aLength) : Base(aBytes, aLength) {
    MOZ_ASSERT(aBytes[aLength] == '\0');
  }

  using Base::operator=;

  char* c_str() { return reinterpret_cast<char*>(get()); }
};

class UTF8Chars : public mozilla::Range<unsigned char> {
  typedef mozilla::Range<unsigned char> Base;

 public:
  using CharT = unsigned char;

  UTF8Chars() = default;
  UTF8Chars(char* aBytes, size_t aLength)
      : Base(reinterpret_cast<unsigned char*>(aBytes), aLength) {}
  UTF8Chars(const char* aBytes, size_t aLength)
      : Base(reinterpret_cast<unsigned char*>(const_cast<char*>(aBytes)),
             aLength) {}
  UTF8Chars(mozilla::Utf8Unit* aUnits, size_t aLength)
      : UTF8Chars(reinterpret_cast<char*>(aUnits), aLength) {}
  UTF8Chars(const mozilla::Utf8Unit* aUnits, size_t aLength)
      : UTF8Chars(reinterpret_cast<const char*>(aUnits), aLength) {}
};

/*
 * SpiderMonkey also deals directly with UTF-8 encoded text in some places.
 */
class UTF8CharsZ : public mozilla::RangedPtr<unsigned char> {
  typedef mozilla::RangedPtr<unsigned char> Base;

 public:
  using CharT = unsigned char;

  UTF8CharsZ() : Base(nullptr, 0) {}  // NOLINT

  UTF8CharsZ(char* aBytes, size_t aLength)
      : Base(reinterpret_cast<unsigned char*>(aBytes), aLength) {
    MOZ_ASSERT(aBytes[aLength] == '\0');
  }

  UTF8CharsZ(unsigned char* aBytes, size_t aLength) : Base(aBytes, aLength) {
    MOZ_ASSERT(aBytes[aLength] == '\0');
  }

  UTF8CharsZ(mozilla::Utf8Unit* aUnits, size_t aLength)
      : UTF8CharsZ(reinterpret_cast<char*>(aUnits), aLength) {}

  using Base::operator=;

  char* c_str() { return reinterpret_cast<char*>(get()); }
};

/*
 * A wrapper for a "const char*" that is encoded using UTF-8.
 * This class does not manage ownership of the data; that is left
 * to others.  This differs from UTF8CharsZ in that the chars are
 * const and it disallows assignment.
 */
class JS_PUBLIC_API ConstUTF8CharsZ {
  const char* data_;

 public:
  using CharT = unsigned char;

  ConstUTF8CharsZ() : data_(nullptr) {}

  ConstUTF8CharsZ(const char* aBytes, size_t aLength) : data_(aBytes) {
    MOZ_ASSERT(aBytes[aLength] == '\0');
#ifdef DEBUG
    validate(aLength);
#endif
  }

  const void* get() const { return data_; }

  const char* c_str() const { return data_; }

  explicit operator bool() const { return data_ != nullptr; }

 private:
#ifdef DEBUG
  void validate(size_t aLength);
#endif
};

/*
 * SpiderMonkey uses a 2-byte character representation: it is a
 * 2-byte-at-a-time view of a UTF-16 byte stream. This is similar to UCS-2,
 * but unlike UCS-2, we do not strip UTF-16 extension bytes. This allows a
 * sufficiently dedicated JavaScript program to be fully unicode-aware by
 * manually interpreting UTF-16 extension characters embedded in the JS
 * string.
 */
class TwoByteChars : public mozilla::Range<char16_t> {
  typedef mozilla::Range<char16_t> Base;

 public:
  using CharT = char16_t;

  TwoByteChars() = default;
  TwoByteChars(char16_t* aChars, size_t aLength) : Base(aChars, aLength) {}
  TwoByteChars(const char16_t* aChars, size_t aLength)
      : Base(const_cast<char16_t*>(aChars), aLength) {}
};

/*
 * A TwoByteChars, but \0 terminated for compatibility with JSFlatString.
 */
class TwoByteCharsZ : public mozilla::RangedPtr<char16_t> {
  typedef mozilla::RangedPtr<char16_t> Base;

 public:
  using CharT = char16_t;

  TwoByteCharsZ() : Base(nullptr, 0) {}  // NOLINT

  TwoByteCharsZ(char16_t* chars, size_t length) : Base(chars, length) {
    MOZ_ASSERT(chars[length] == '\0');
  }

  using Base::operator=;
};

typedef mozilla::RangedPtr<const char16_t> ConstCharPtr;

/*
 * Like TwoByteChars, but the chars are const.
 */
class ConstTwoByteChars : public mozilla::Range<const char16_t> {
  typedef mozilla::Range<const char16_t> Base;

 public:
  using CharT = char16_t;

  ConstTwoByteChars() = default;
  ConstTwoByteChars(const char16_t* aChars, size_t aLength)
      : Base(aChars, aLength) {}
};

/*
 * Convert a 2-byte character sequence to "ISO-Latin-1". This works by
 * truncating each 2-byte pair in the sequence to a 1-byte pair. If the source
 * contains any UTF-16 extension characters, then this may give invalid Latin1
 * output. The returned string is zero terminated. The returned string or the
 * returned string's |start()| must be freed with JS_free or js_free,
 * respectively. If allocation fails, an OOM error will be set and the method
 * will return a nullptr chars (which can be tested for with the ! operator).
 * This method cannot trigger GC.
 */
extern Latin1CharsZ LossyTwoByteCharsToNewLatin1CharsZ(
    JSContext* cx, const mozilla::Range<const char16_t> tbchars);

inline Latin1CharsZ LossyTwoByteCharsToNewLatin1CharsZ(JSContext* cx,
                                                       const char16_t* begin,
                                                       size_t length) {
  const mozilla::Range<const char16_t> tbchars(begin, length);
  return JS::LossyTwoByteCharsToNewLatin1CharsZ(cx, tbchars);
}

template <typename CharT>
extern UTF8CharsZ CharsToNewUTF8CharsZ(JSContext* cx,
                                       const mozilla::Range<CharT> chars);

JS_PUBLIC_API uint32_t Utf8ToOneUcs4Char(const uint8_t* utf8Buffer,
                                         int utf8Length);

/*
 * Inflate bytes in UTF-8 encoding to char16_t.
 * - On error, returns an empty TwoByteCharsZ.
 * - On success, returns a malloc'd TwoByteCharsZ, and updates |outlen| to hold
 *   its length;  the length value excludes the trailing null.
 */
extern JS_PUBLIC_API TwoByteCharsZ
UTF8CharsToNewTwoByteCharsZ(JSContext* cx, const UTF8Chars utf8, size_t* outlen,
                            arena_id_t destArenaId);

/*
 * Like UTF8CharsToNewTwoByteCharsZ, but for ConstUTF8CharsZ.
 */
extern JS_PUBLIC_API TwoByteCharsZ
UTF8CharsToNewTwoByteCharsZ(JSContext* cx, const ConstUTF8CharsZ& utf8,
                            size_t* outlen, arena_id_t destArenaId);

/*
 * The same as UTF8CharsToNewTwoByteCharsZ(), except that any malformed UTF-8
 * characters will be replaced by \uFFFD. No exception will be thrown for
 * malformed UTF-8 input.
 */
extern JS_PUBLIC_API TwoByteCharsZ
LossyUTF8CharsToNewTwoByteCharsZ(JSContext* cx, const UTF8Chars utf8,
                                 size_t* outlen, arena_id_t destArenaId);

extern JS_PUBLIC_API TwoByteCharsZ
LossyUTF8CharsToNewTwoByteCharsZ(JSContext* cx, const ConstUTF8CharsZ& utf8,
                                 size_t* outlen, arena_id_t destArenaId);

/*
 * Returns the length of the char buffer required to encode |s| as UTF8.
 * Does not include the null-terminator.
 */
JS_PUBLIC_API size_t GetDeflatedUTF8StringLength(JSLinearString* s);

/*
 * Encode whole scalar values of |src| into |dst| as UTF-8 until |src| is
 * exhausted or too little space is available in |dst| to fit the scalar
 * value. Lone surrogates are converted to REPLACEMENT CHARACTER. Return
 * the number of bytes of |dst| that were filled.
 *
 * Use |JS_EncodeStringToUTF8BufferPartial| if your string isn't already
 * linear.
 *
 * Given |JSString* str = JS_FORGET_STRING_LINEARNESS(src)|,
 * if |JS::StringHasLatin1Chars(str)|, then |src| is always fully converted
 * if |dst.Length() >= JS_GetStringLength(str) * 2|. Otherwise |src| is
 * always fully converted if |dst.Length() >= JS_GetStringLength(str) * 3|.
 *
 * The exact space required is always |GetDeflatedUTF8StringLength(str)|.
 */
JS_PUBLIC_API size_t DeflateStringToUTF8Buffer(JSLinearString* src,
                                               mozilla::Span<char> dst);

/*
 * The smallest character encoding capable of fully representing a particular
 * string.
 */
enum class SmallestEncoding { ASCII, Latin1, UTF16 };

/*
 * Returns the smallest encoding possible for the given string: if all
 * codepoints are <128 then ASCII, otherwise if all codepoints are <256
 * Latin-1, else UTF16.
 */
JS_PUBLIC_API SmallestEncoding FindSmallestEncoding(UTF8Chars utf8);

/*
 * Return a null-terminated Latin-1 string copied from the input string,
 * storing its length (excluding null terminator) in |*outlen|.  Fail and
 * report an error if the string contains non-Latin-1 codepoints.  Returns
 * Latin1CharsZ() on failure.
 */
extern JS_PUBLIC_API Latin1CharsZ
UTF8CharsToNewLatin1CharsZ(JSContext* cx, const UTF8Chars utf8, size_t* outlen,
                           arena_id_t destArenaId);

/*
 * Return a null-terminated Latin-1 string copied from the input string,
 * storing its length (excluding null terminator) in |*outlen|.  Non-Latin-1
 * codepoints are replaced by '?'.  Returns Latin1CharsZ() on failure.
 */
extern JS_PUBLIC_API Latin1CharsZ
LossyUTF8CharsToNewLatin1CharsZ(JSContext* cx, const UTF8Chars utf8,
                                size_t* outlen, arena_id_t destArenaId);

/*
 * Returns true if all characters in the given null-terminated string are
 * ASCII, i.e. < 0x80, false otherwise.
 */
extern JS_PUBLIC_API bool StringIsASCII(const char* s);

/*
 * Returns true if all characters in the given span are ASCII,
 * i.e. < 0x80, false otherwise.
 */
extern JS_PUBLIC_API bool StringIsASCII(mozilla::Span<const char> s);

}  // namespace JS

inline void JS_free(JS::Latin1CharsZ& ptr) { js_free((void*)ptr.get()); }
inline void JS_free(JS::UTF8CharsZ& ptr) { js_free((void*)ptr.get()); }

/**
 * DEPRECATED
 *
 * Allocate memory sufficient to contain the characters of |str| truncated to
 * Latin-1 and a trailing null terminator, fill the memory with the characters
 * interpreted in that manner plus the null terminator, and return a pointer to
 * the memory.
 *
 * This function *loses information* when it copies the characters of |str| if
 * |str| contains code units greater than 0xFF.  Additionally, users that
 * depend on null-termination will misinterpret the copied characters if |str|
 * contains any nulls.  Avoid using this function if possible, because it will
 * eventually be removed.
 */
extern JS_PUBLIC_API JS::UniqueChars JS_EncodeStringToLatin1(JSContext* cx,
                                                             JSString* str);

/**
 * DEPRECATED
 *
 * Same behavior as JS_EncodeStringToLatin1(), but encode into a UTF-8 string.
 *
 * This function *loses information* when it copies the characters of |str| if
 * |str| contains invalid UTF-16: U+FFFD REPLACEMENT CHARACTER will be copied
 * instead.
 *
 * The returned string is also subject to misinterpretation if |str| contains
 * any nulls (which are faithfully transcribed into the returned string, but
 * which will implicitly truncate the string if it's passed to functions that
 * expect null-terminated strings).
 *
 * Avoid using this function if possible, because we'll remove it once we can
 * devise a better API for the task.
 */
extern JS_PUBLIC_API JS::UniqueChars JS_EncodeStringToUTF8(
    JSContext* cx, JS::Handle<JSString*> str);

/**
 * DEPRECATED
 *
 * Same behavior as JS_EncodeStringToLatin1(), but encode into an ASCII string.
 *
 * This function asserts in debug mode that the input string contains only
 * ASCII characters.
 *
 * The returned string is also subject to misinterpretation if |str| contains
 * any nulls (which are faithfully transcribed into the returned string, but
 * which will implicitly truncate the string if it's passed to functions that
 * expect null-terminated strings).
 *
 * Avoid using this function if possible, because we'll remove it once we can
 * devise a better API for the task.
 */
extern JS_PUBLIC_API JS::UniqueChars JS_EncodeStringToASCII(JSContext* cx,
                                                            JSString* str);

#endif /* js_CharacterEncoding_h */
