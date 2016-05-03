// linenoise_utf8.h
/*
 *    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include <algorithm>
#include <memory>
#include <string.h>

namespace linenoise_utf8 {

typedef unsigned char UChar8;  // UTF-8 octet
typedef char32_t UChar32;      // Unicode code point

// Error bits (or-ed together) returned from utf8toUChar32string
//
enum BadUTF8 { BadUTF8_no_error = 0x00, BadUTF8_invalid_byte = 0x01, BadUTF8_surrogate = 0x02 };

/**
 * Convert a null terminated UTF-8 std::string from UTF-8 and store it in a UChar32 destination
 * buffer Always null terminates the destination std::string if at least one character position is
 * available Errors in the UTF-8 encoding will be handled in two ways: the erroneous characters will
 * be converted to the Unicode error character U+FFFD and flag bits will be set in the
 * conversionErrorCode int.
 *
 * @param uchar32output                 Destination UChar32 buffer
 * @param utf8input                     Source UTF-8 string
 * @param outputBufferSizeInCharacters  Destination buffer size in characters
 * @param outputUnicodeCharacterCount   Number of UChar32 characters placed in output buffer
 * @param conversionErrorCode           Flag bits from enum BadUTF8, or zero if no error
 */
void copyString8to32(UChar32* uchar32output,
                     const UChar8* utf8input,
                     size_t outputBufferSizeInCharacters,
                     size_t& outputUnicodeCharacterCount,
                     int& conversionErrorCode);

/**
 * Copy a null terminated UChar32 std::string to a UChar32 destination buffer
 * Always null terminates the destination std::string if at least one character position is
 * available
 *
 * @param dest32                    Destination UChar32 buffer
 * @param source32                  Source UChar32 string
 * @param destLengthInCharacters    Destination buffer length in characters
 */
void copyString32(UChar32* dest32, const UChar32* source32, size_t destLengthInCharacters);

/**
 * Convert a specified number of UChar32 characters from a possibly null terminated UChar32
 * std::string to UTF-8 and store it in a UChar8 destination buffer
 * Always null terminates the destination std::string if at least one character position is
 * available
 *
 * @param dest8                     Destination UChar8 buffer
 * @param source32                  Source UChar32 string
 * @param outputBufferSizeInBytes   Destination buffer size in bytes
 * @param charCount                 Maximum number of UChar32 characters to process
 * @return                          Count of bytes written to output buffer, not including null
 *                                  terminator
 */
size_t copyString32to8counted(UChar8* dest8,
                              const UChar32* source32,
                              size_t outputBufferSizeInBytes,
                              size_t charCount);

/**
 * Convert a null terminated UChar32 std::string to UTF-8 and store it in a UChar8 destination
 * buffer
 * Always null terminates the destination std::string if at least one character position is
 * available
 *
 * @param dest8                     Destination UChar8 buffer
 * @param source32                  Source UChar32 string
 * @param outputBufferSizeInBytes   Destination buffer size in bytes
 * @return                          Count of bytes written to output buffer, not including null
 *                                  terminator
 */
size_t copyString32to8(UChar8* dest8, const UChar32* source32, size_t outputBufferSizeInBytes);

/**
 * Count characters (i.e. Unicode code points, array elements) in a null terminated UChar32 string
 *
 * @param str32     Source UChar32 string
 * @return          std::string length in characters
 */
size_t strlen32(const UChar32* str32);

/**
 * Compare two UChar32 null-terminated strings with length parameter
 *
 * @param first32   First std::string to compare
 * @param second32  Second std::string to compare
 * @param length    Maximum number of characters to compare
 * @return          Negative if first < second, positive if first > second, zero if equal
 */
int strncmp32(UChar32* first32, UChar32* second32, size_t length);

/**
 * Internally convert an array of UChar32 characters of specified length to UTF-8 and write it to
 * fileHandle
 *
 * @param fileHandle                File handle to write to
 * @param string32                  Source UChar32 character array, may not be null terminated
 * @param sourceLengthInCharacters  Number of source characters to convert and write
 * @return                          Number of bytes written, -1 on error
 */
int write32(int fileHandle, const UChar32* string32, unsigned int sourceLengthInCharacters);

/**
 * Template and classes for UChar8 and UChar32 strings
 */
template <typename char_type>
struct UtfStringMixin {
    typedef char_type char_t;  // inherited

    UtfStringMixin() : _len(0), _cap(0), _chars(0) {}

    UtfStringMixin(const UtfStringMixin& other)  // copies like std::string
        : _len(other._len),
          _cap(other._len + 1),
          _chars(other._chars),
          _str(new char_t[_cap]) {
        memcpy(_str.get(), other._str.get(), _cap * sizeof(char_t));
    }

    UtfStringMixin& operator=(UtfStringMixin copy) {
        this->swap(copy);
        return *this;
    }

    char_t* get() const {
        return _str.get();
    }
    char_t& operator[](size_t idx) {
        return _str[idx];
    }
    const char_t& operator[](size_t idx) const {
        return _str[idx];
    }

    size_t length() const {
        return _len;
    }
    size_t capacity() const {
        return _cap;
    }
    size_t chars() const {
        return _chars;
    }

    void swap(UtfStringMixin& other) {
        std::swap(_len, other._len);
        std::swap(_cap, other._cap);
        std::swap(_chars, other._chars);
        _str.swap(other._str);
    }

protected:
    size_t _len;    // in units of char_t without nul
    size_t _cap;    // size of _str buffer including nul
    size_t _chars;  // number of codepoints
    std::unique_ptr<char_t[]> _str;
};

struct Utf32String;

struct Utf8String : public UtfStringMixin<UChar8> {
    Utf8String() {}
    explicit Utf8String(const UChar32* s, int chars = -1) {
        if (chars == -1) {
            initFrom32(s, strlen32(s));
        } else {
            initFrom32(s, chars);
        }
    }
    explicit Utf8String(const Utf32String& c);  // defined after utf32String

private:
    void initFrom32(const UChar32* s, int chars) {
        _chars = chars;
        _cap = _chars * sizeof(UChar32) + 1;
        _str.reset(new char_t[_cap]);
        _len = copyString32to8counted(_str.get(), s, _cap, chars);
    }
};

struct Utf32String : public UtfStringMixin<UChar32> {
    Utf32String() {}
    explicit Utf32String(const UChar32* s) {
        _chars = _len = strlen32(s);
        _cap = _len + 1;
        _str.reset(new UChar32[_cap]);
        memcpy(_str.get(), s, _cap * sizeof(UChar32));
    }
    explicit Utf32String(const UChar32* s, int textLen) {
        _chars = _len = textLen;
        _cap = _len + 1;
        _str.reset(new UChar32[_cap]);
        memcpy(_str.get(), s, _len * sizeof(UChar32));
        _str[_len] = 0;
    }
    explicit Utf32String(const UChar8* s, int chars = -1) {
        initFrom8(s, chars);
    }
    explicit Utf32String(const Utf8String& c) {
        initFrom8(c.get(), c.chars());
    }
    explicit Utf32String(size_t reserve) {
        _len = 0;
        _cap = reserve;
        _chars = 0;
        _str.reset(new UChar32[_cap]);
        _str[0] = 0;
    }
    void initFromBuffer(void) {
        _chars = _len = strlen32(_str.get());
    }

private:
    void initFrom8(const UChar8* s, int chars) {
        Utf32String temp;
        if (chars == -1) {
            temp._cap = strlen(reinterpret_cast<const char*>(s)) + 1;  // worst case ASCII
        } else {
            temp._cap = chars + 1;
        }
        temp._str.reset(new char_t[temp._cap]);
        int error;
        copyString8to32(temp._str.get(), s, temp._cap, temp._chars, error);
        temp._len = temp._chars;
        this->swap(temp);
    }
};

inline Utf8String::Utf8String(const Utf32String& s) {
    initFrom32(s.get(), s.chars());
}

}  // namespace linenoise_utf8
