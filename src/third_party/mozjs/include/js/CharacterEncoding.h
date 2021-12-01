/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_CharacterEncoding_h
#define js_CharacterEncoding_h

#include "mozilla/Range.h"

#include "js/TypeDecls.h"
#include "js/Utility.h"

class JSFlatString;

namespace JS {

/*
 * By default, all C/C++ 1-byte-per-character strings passed into the JSAPI
 * are treated as ISO/IEC 8859-1, also known as Latin-1. That is, each
 * byte is treated as a 2-byte character, and there is no way to pass in a
 * string containing characters beyond U+00FF.
 */
class Latin1Chars : public mozilla::Range<Latin1Char>
{
    typedef mozilla::Range<Latin1Char> Base;

  public:
    using CharT = Latin1Char;

    Latin1Chars() : Base() {}
    Latin1Chars(char* aBytes, size_t aLength) : Base(reinterpret_cast<Latin1Char*>(aBytes), aLength) {}
    Latin1Chars(const Latin1Char* aBytes, size_t aLength)
      : Base(const_cast<Latin1Char*>(aBytes), aLength)
    {}
    Latin1Chars(const char* aBytes, size_t aLength)
      : Base(reinterpret_cast<Latin1Char*>(const_cast<char*>(aBytes)), aLength)
    {}
};

/*
 * A Latin1Chars, but with \0 termination for C compatibility.
 */
class Latin1CharsZ : public mozilla::RangedPtr<Latin1Char>
{
    typedef mozilla::RangedPtr<Latin1Char> Base;

  public:
    using CharT = Latin1Char;

    Latin1CharsZ() : Base(nullptr, 0) {}

    Latin1CharsZ(char* aBytes, size_t aLength)
      : Base(reinterpret_cast<Latin1Char*>(aBytes), aLength)
    {
        MOZ_ASSERT(aBytes[aLength] == '\0');
    }

    Latin1CharsZ(Latin1Char* aBytes, size_t aLength)
      : Base(aBytes, aLength)
    {
        MOZ_ASSERT(aBytes[aLength] == '\0');
    }

    using Base::operator=;

    char* c_str() { return reinterpret_cast<char*>(get()); }
};

class UTF8Chars : public mozilla::Range<unsigned char>
{
    typedef mozilla::Range<unsigned char> Base;

  public:
    using CharT = unsigned char;

    UTF8Chars() : Base() {}
    UTF8Chars(char* aBytes, size_t aLength)
      : Base(reinterpret_cast<unsigned char*>(aBytes), aLength)
    {}
    UTF8Chars(const char* aBytes, size_t aLength)
      : Base(reinterpret_cast<unsigned char*>(const_cast<char*>(aBytes)), aLength)
    {}
};

/*
 * SpiderMonkey also deals directly with UTF-8 encoded text in some places.
 */
class UTF8CharsZ : public mozilla::RangedPtr<unsigned char>
{
    typedef mozilla::RangedPtr<unsigned char> Base;

  public:
    using CharT = unsigned char;

    UTF8CharsZ() : Base(nullptr, 0) {}

    UTF8CharsZ(char* aBytes, size_t aLength)
      : Base(reinterpret_cast<unsigned char*>(aBytes), aLength)
    {
        MOZ_ASSERT(aBytes[aLength] == '\0');
    }

    UTF8CharsZ(unsigned char* aBytes, size_t aLength)
      : Base(aBytes, aLength)
    {
        MOZ_ASSERT(aBytes[aLength] == '\0');
    }

    using Base::operator=;

    char* c_str() { return reinterpret_cast<char*>(get()); }
};

/*
 * A wrapper for a "const char*" that is encoded using UTF-8.
 * This class does not manage ownership of the data; that is left
 * to others.  This differs from UTF8CharsZ in that the chars are
 * const and it allows assignment.
 */
class JS_PUBLIC_API(ConstUTF8CharsZ)
{
    const char* data_;

  public:
    using CharT = unsigned char;

    ConstUTF8CharsZ() : data_(nullptr)
    {}

    ConstUTF8CharsZ(const char* aBytes, size_t aLength)
      : data_(aBytes)
    {
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
class TwoByteChars : public mozilla::Range<char16_t>
{
    typedef mozilla::Range<char16_t> Base;

  public:
    using CharT = char16_t;

    TwoByteChars() : Base() {}
    TwoByteChars(char16_t* aChars, size_t aLength) : Base(aChars, aLength) {}
    TwoByteChars(const char16_t* aChars, size_t aLength) : Base(const_cast<char16_t*>(aChars), aLength) {}
};

/*
 * A TwoByteChars, but \0 terminated for compatibility with JSFlatString.
 */
class TwoByteCharsZ : public mozilla::RangedPtr<char16_t>
{
    typedef mozilla::RangedPtr<char16_t> Base;

  public:
    using CharT = char16_t;

    TwoByteCharsZ() : Base(nullptr, 0) {}

    TwoByteCharsZ(char16_t* chars, size_t length)
      : Base(chars, length)
    {
        MOZ_ASSERT(chars[length] == '\0');
    }

    using Base::operator=;
};

typedef mozilla::RangedPtr<const char16_t> ConstCharPtr;

/*
 * Like TwoByteChars, but the chars are const.
 */
class ConstTwoByteChars : public mozilla::Range<const char16_t>
{
    typedef mozilla::Range<const char16_t> Base;

  public:
    using CharT = char16_t;

    ConstTwoByteChars() : Base() {}
    ConstTwoByteChars(const char16_t* aChars, size_t aLength) : Base(aChars, aLength) {}
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
extern Latin1CharsZ
LossyTwoByteCharsToNewLatin1CharsZ(JSContext* cx,
                                   const mozilla::Range<const char16_t> tbchars);

inline Latin1CharsZ
LossyTwoByteCharsToNewLatin1CharsZ(JSContext* cx, const char16_t* begin, size_t length)
{
    const mozilla::Range<const char16_t> tbchars(begin, length);
    return JS::LossyTwoByteCharsToNewLatin1CharsZ(cx, tbchars);
}

template <typename CharT>
extern UTF8CharsZ
CharsToNewUTF8CharsZ(JSContext* maybeCx, const mozilla::Range<CharT> chars);

JS_PUBLIC_API(uint32_t)
Utf8ToOneUcs4Char(const uint8_t* utf8Buffer, int utf8Length);

/*
 * Inflate bytes in UTF-8 encoding to char16_t.
 * - On error, returns an empty TwoByteCharsZ.
 * - On success, returns a malloc'd TwoByteCharsZ, and updates |outlen| to hold
 *   its length;  the length value excludes the trailing null.
 */
extern JS_PUBLIC_API(TwoByteCharsZ)
UTF8CharsToNewTwoByteCharsZ(JSContext* cx, const UTF8Chars utf8, size_t* outlen);

/*
 * Like UTF8CharsToNewTwoByteCharsZ, but for ConstUTF8CharsZ.
 */
extern JS_PUBLIC_API(TwoByteCharsZ)
UTF8CharsToNewTwoByteCharsZ(JSContext* cx, const ConstUTF8CharsZ& utf8, size_t* outlen);

/*
 * The same as UTF8CharsToNewTwoByteCharsZ(), except that any malformed UTF-8 characters
 * will be replaced by \uFFFD. No exception will be thrown for malformed UTF-8
 * input.
 */
extern JS_PUBLIC_API(TwoByteCharsZ)
LossyUTF8CharsToNewTwoByteCharsZ(JSContext* cx, const UTF8Chars utf8, size_t* outlen);

extern JS_PUBLIC_API(TwoByteCharsZ)
LossyUTF8CharsToNewTwoByteCharsZ(JSContext* cx, const ConstUTF8CharsZ& utf8, size_t* outlen);

/*
 * Returns the length of the char buffer required to encode |s| as UTF8.
 * Does not include the null-terminator.
 */
JS_PUBLIC_API(size_t)
GetDeflatedUTF8StringLength(JSFlatString* s);

/*
 * Encode |src| as UTF8. The caller must either ensure |dst| has enough space
 * to encode the entire string or pass the length of the buffer as |dstlenp|,
 * in which case the function will encode characters from the string until
 * the buffer is exhausted. Does not write the null terminator.
 *
 * If |dstlenp| is provided, it will be updated to hold the number of bytes
 * written to the buffer. If |numcharsp| is provided, it will be updated to hold
 * the number of Unicode characters written to the buffer (which can be less
 * than the length of the string, if the buffer is exhausted before the string
 * is fully encoded).
 */
JS_PUBLIC_API(void)
DeflateStringToUTF8Buffer(JSFlatString* src, mozilla::RangedPtr<char> dst,
                          size_t* dstlenp = nullptr, size_t* numcharsp = nullptr);

/*
 * The smallest character encoding capable of fully representing a particular
 * string.
 */
enum class SmallestEncoding {
    ASCII,
    Latin1,
    UTF16
};

/*
 * Returns the smallest encoding possible for the given string: if all
 * codepoints are <128 then ASCII, otherwise if all codepoints are <256
 * Latin-1, else UTF16.
 */
JS_PUBLIC_API(SmallestEncoding)
FindSmallestEncoding(UTF8Chars utf8);

/*
  * Return a null-terminated Latin-1 string copied from the input string,
  * storing its length (excluding null terminator) in |*outlen|.  Fail and
  * report an error if the string contains non-Latin-1 codepoints.  Returns
  * Latin1CharsZ() on failure.
 */
extern JS_PUBLIC_API(Latin1CharsZ)
UTF8CharsToNewLatin1CharsZ(JSContext* cx, const UTF8Chars utf8, size_t* outlen);

/*
 * Return a null-terminated Latin-1 string copied from the input string,
 * storing its length (excluding null terminator) in |*outlen|.  Non-Latin-1
 * codepoints are replaced by '?'.  Returns Latin1CharsZ() on failure.
 */
extern JS_PUBLIC_API(Latin1CharsZ)
LossyUTF8CharsToNewLatin1CharsZ(JSContext* cx, const UTF8Chars utf8, size_t* outlen);

/*
 * Returns true if all characters in the given null-terminated string are
 * ASCII, i.e. < 0x80, false otherwise.
 */
extern JS_PUBLIC_API(bool)
StringIsASCII(const char* s);

/*
 * Returns true if the given length-delimited string is a valid UTF-8 string,
 * false otherwise.
 */
extern JS_PUBLIC_API(bool)
StringIsUTF8(const uint8_t* s, uint32_t length);

} // namespace JS

inline void JS_free(JS::Latin1CharsZ& ptr) { js_free((void*)ptr.get()); }
inline void JS_free(JS::UTF8CharsZ& ptr) { js_free((void*)ptr.get()); }

#endif /* js_CharacterEncoding_h */
