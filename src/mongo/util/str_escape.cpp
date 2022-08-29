/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/util/str_escape.h"

#include <algorithm>
#include <array>
#include <iterator>

#include "mongo/util/assert_util.h"

namespace mongo::str {
namespace {
constexpr char kHexChar[] = "0123456789abcdef";

struct NoopBuffer {
    void append(const char* begin, const char* end) {}
};

// Appends the bytes in the range [begin, end) to the output buffer,
// which can either be a fmt::memory_buffer, or a std::string.
template <typename Buffer, typename Iterator>
void appendBuffer(Buffer& buffer, Iterator begin, Iterator end) {
    buffer.append(begin, end);
}

// 'singleHandler' Function to write a valid single byte UTF-8 sequence with desired escaping.
// 'invalidByteHandler' Function to write a byte of invalid UTF-8 encoding
// 'twoEscaper' Function to write a valid two byte UTF-8 sequence with desired escaping, for C1
// control codes.
// 'maxLength' Max length to write into output buffer; A value of std::string::npos means unbounded.
// An escape sequence will not be written if appending the entire sequence will exceed this limit.
// 'wouldWrite' Output to contain the total bytes that would have been written to the buffer if no
// size limit is in place.
//
// All these functions take a function object as their first parameter to perform the
// writing of any escaped data. This function expects the number of handled bytes as its first
// parameter and the corresponding escaped string as the second. They are templates to they can be
// inlined.
template <typename Buffer,
          typename SingleByteHandler,
          typename InvalidByteHandler,
          typename TwoByteEscaper>
void escape(Buffer& buffer,
            StringData str,
            SingleByteHandler singleHandler,
            InvalidByteHandler invalidByteHandler,
            TwoByteEscaper twoEscaper,
            size_t maxLength,
            size_t* wouldWrite) {
    // The range [inFirst, it) contains input that does not need to be escaped and that has not been
    // written to output yet.
    // The range [it, inLast) contains remaining input to scan. 'inFirst' is pointing to the
    // beginning of the input that has not yet been written to 'escaped'. 'it' is pointing to the
    // beginning of the unicode code point we're currently processing in the while-loop below.
    // 'inLast' is the end of the input sequence.
    auto inFirst = str.begin();
    auto inLast = str.end();
    auto it = inFirst;
    size_t cap = maxLength;
    size_t total = 0;

    // Writes an escaped sequence to output after flushing pending input that does not need to be
    // escaped. 'it' is assumed to be at the beginning of the input sequence represented by the
    // escaped data.
    // 'numHandled' the number of bytes of unescaped data being written escaped in 'escapeSequence'
    auto flushAndWrite = [&](size_t numHandled, StringData escapeSequence) {
        // Appends the range [wFirst, wLast) to the output if the result is within the max length.
        // 'canTruncate' controls the behavior if appending the entire range would exceed the limit.
        // If true, this appends input up to the length limit. Otherwise, none is appended.
        auto boundedWrite = [&](auto wFirst, auto wLast, bool canTruncate) {
            size_t len = std::distance(wFirst, wLast);
            total += len;
            if (maxLength != std::string::npos) {
                if (len > cap) {
                    if (!canTruncate) {
                        cap = 0;
                    }
                    len = cap;
                }
                cap -= len;
            }
            appendBuffer(buffer, wFirst, wFirst + len);
        };

        // Flush range of unmodified input
        boundedWrite(inFirst, it, true);
        inFirst = it + numHandled;

        // Write escaped data
        boundedWrite(escapeSequence.begin(), escapeSequence.end(), false);
    };

    auto isValidCodePoint = [&](auto pos, int len) {
        return std::distance(pos, inLast) >= len &&
            std::all_of(pos + 1, pos + len, [](uint8_t c) { return (c >> 6) == 0b10; });
    };

    // Helper function to write a valid one byte UTF-8 sequence from the input stream
    auto writeValid1Byte = [&]() { singleHandler(flushAndWrite, *it); };

    // Helper function to write a valid two byte UTF-8 sequence from the input stream
    auto writeValid2Byte = [&]() {
        uint8_t first = *it;
        uint8_t second = *(it + 1);

        if (MONGO_unlikely(first == 0xc2 && second >= 0x80 && second < 0xa0)) {
            twoEscaper(flushAndWrite, first, second);
        }
    };

    // Helper function to write an invalid UTF-8 sequence from the input stream
    // Will try and write up to num bytes but bail if we reach the end of the input.
    // Updates the position of 'it'.
    auto writeInvalid = [&](uint8_t c) { invalidByteHandler(flushAndWrite, c); };


    while (it != inLast) {
        uint8_t c = *it;
        bool bit7 = (c >> 7) & 1;
        if (MONGO_likely(!bit7)) {
            writeValid1Byte();
            ++it;
            continue;
        }

        bool bit6 = (c >> 6) & 1;
        if (MONGO_unlikely(!bit6)) {
            writeInvalid(c);
            ++it;
            continue;
        }

        bool bit5 = (c >> 5) & 1;
        if (!bit5) {
            // 2 byte sequence
            if (MONGO_likely(isValidCodePoint(it, 2))) {
                writeValid2Byte();
                it += 2;
            } else {
                writeInvalid(c);
                ++it;
            }

            continue;
        }

        bool bit4 = (c >> 4) & 1;
        if (!bit4) {
            // 3 byte sequence
            if (MONGO_likely(isValidCodePoint(it, 3))) {
                it += 3;
            } else {
                writeInvalid(c);
                ++it;
            }
            continue;
        }

        bool bit3 = (c >> 3) & 1;
        if (bit3) {
            writeInvalid(c);
            ++it;
            continue;
        }

        // 4 byte sequence
        if (MONGO_likely(isValidCodePoint(it, 4))) {
            it += 4;
        } else {
            writeInvalid(c);
            ++it;
        }
    }
    // Write last block
    flushAndWrite(0, {});
    if (wouldWrite) {
        *wouldWrite = total;
    }
}
}  // namespace

template <typename Buffer>
void escapeForTextCommon(Buffer& buffer, StringData str, size_t maxLength, size_t* wouldWrite) {
    auto singleByteHandler = [](const auto& writer, uint8_t unescaped) {
        switch (unescaped) {
            case '\0':
                writer(1, "\\0"_sd);
                break;
            case 0x01:
                writer(1, "\\x01"_sd);
                break;
            case 0x02:
                writer(1, "\\x02"_sd);
                break;
            case 0x03:
                writer(1, "\\x03"_sd);
                break;
            case 0x04:
                writer(1, "\\x04"_sd);
                break;
            case 0x05:
                writer(1, "\\x05"_sd);
                break;
            case 0x06:
                writer(1, "\\x06"_sd);
                break;
            case 0x07:
                writer(1, "\\a"_sd);
                break;
            case 0x08:
                writer(1, "\\b"_sd);
                break;
            case 0x09:
                writer(1, "\\t"_sd);
                break;
            case 0x0a:
                writer(1, "\\n"_sd);
                break;
            case 0x0b:
                writer(1, "\\v"_sd);
                break;
            case 0x0c:
                writer(1, "\\f"_sd);
                break;
            case 0x0d:
                writer(1, "\\r"_sd);
                break;
            case 0x0e:
                writer(1, "\\x0e"_sd);
                break;
            case 0x0f:
                writer(1, "\\x0f"_sd);
                break;
            case 0x10:
                writer(1, "\\x10"_sd);
                break;
            case 0x11:
                writer(1, "\\x11"_sd);
                break;
            case 0x12:
                writer(1, "\\x12"_sd);
                break;
            case 0x13:
                writer(1, "\\x13"_sd);
                break;
            case 0x14:
                writer(1, "\\x14"_sd);
                break;
            case 0x15:
                writer(1, "\\x15"_sd);
                break;
            case 0x16:
                writer(1, "\\x16"_sd);
                break;
            case 0x17:
                writer(1, "\\x17"_sd);
                break;
            case 0x18:
                writer(1, "\\x18"_sd);
                break;
            case 0x19:
                writer(1, "\\x19"_sd);
                break;
            case 0x1a:
                writer(1, "\\x1a"_sd);
                break;
            case 0x1b:
                writer(1, "\\e"_sd);
                break;
            case 0x1c:
                writer(1, "\\x1c"_sd);
                break;
            case 0x1d:
                writer(1, "\\x1d"_sd);
                break;
            case 0x1e:
                writer(1, "\\x1e"_sd);
                break;
            case 0x1f:
                writer(1, "\\x1f"_sd);
                break;
            case '\\':
                writer(1, "\\\\"_sd);
                break;
            case 0x7f:
                writer(1, "\\x7f"_sd);
                break;
            default:
                break;
        }
    };
    auto invalidByteHandler = [](const auto& writer, uint8_t invalid) {
        std::array<char, 4> buffer = {'\\', 'x', kHexChar[invalid >> 4], kHexChar[invalid & 0xf]};
        writer(1, StringData(buffer.data(), buffer.size()));
    };
    auto twoByteEscaper = [](const auto& writer, uint8_t first, uint8_t second) {
        std::array<char, 8> buffer = {'\\',
                                      'x',
                                      kHexChar[first >> 4],
                                      kHexChar[first & 0xf],
                                      '\\',
                                      'x',
                                      kHexChar[second >> 4],
                                      kHexChar[second & 0xf]};
        writer(2, StringData(buffer.data(), buffer.size()));
    };
    return escape(buffer,
                  str,
                  std::move(singleByteHandler),
                  std::move(invalidByteHandler),
                  std::move(twoByteEscaper),
                  maxLength,
                  wouldWrite);
}

void escapeForText(fmt::memory_buffer& buffer,
                   StringData str,
                   size_t maxLength,
                   size_t* wouldWrite) {
    escapeForTextCommon(buffer, str, maxLength, wouldWrite);
}

std::string escapeForText(StringData str, size_t maxLength, size_t* wouldWrite) {
    std::string buffer;
    escapeForTextCommon(buffer, str, maxLength, wouldWrite);
    return buffer;
}

template <typename Buffer>
void escapeForJSONCommon(Buffer& buffer, StringData str, size_t maxLength, size_t* wouldWrite) {
    auto singleByteHandler = [](const auto& writer, uint8_t unescaped) {
        switch (unescaped) {
            case '\0':
                writer(1, "\\u0000"_sd);
                break;
            case 0x01:
                writer(1, "\\u0001"_sd);
                break;
            case 0x02:
                writer(1, "\\u0002"_sd);
                break;
            case 0x03:
                writer(1, "\\u0003"_sd);
                break;
            case 0x04:
                writer(1, "\\u0004"_sd);
                break;
            case 0x05:
                writer(1, "\\u0005"_sd);
                break;
            case 0x06:
                writer(1, "\\u0006"_sd);
                break;
            case 0x07:
                writer(1, "\\u0007"_sd);
                break;
            case 0x08:
                writer(1, "\\b"_sd);
                break;
            case 0x09:
                writer(1, "\\t"_sd);
                break;
            case 0x0a:
                writer(1, "\\n"_sd);
                break;
            case 0x0b:
                writer(1, "\\u000b"_sd);
                break;
            case 0x0c:
                writer(1, "\\f"_sd);
                break;
            case 0x0d:
                writer(1, "\\r"_sd);
                break;
            case 0x0e:
                writer(1, "\\u000e"_sd);
                break;
            case 0x0f:
                writer(1, "\\u000f"_sd);
                break;
            case 0x10:
                writer(1, "\\u0010"_sd);
                break;
            case 0x11:
                writer(1, "\\u0011"_sd);
                break;
            case 0x12:
                writer(1, "\\u0012"_sd);
                break;
            case 0x13:
                writer(1, "\\u0013"_sd);
                break;
            case 0x14:
                writer(1, "\\u0014"_sd);
                break;
            case 0x15:
                writer(1, "\\u0015"_sd);
                break;
            case 0x16:
                writer(1, "\\u0016"_sd);
                break;
            case 0x17:
                writer(1, "\\u0017"_sd);
                break;
            case 0x18:
                writer(1, "\\u0018"_sd);
                break;
            case 0x19:
                writer(1, "\\u0019"_sd);
                break;
            case 0x1a:
                writer(1, "\\u001a"_sd);
                break;
            case 0x1b:
                writer(1, "\\u001b"_sd);
                break;
            case 0x1c:
                writer(1, "\\u001c"_sd);
                break;
            case 0x1d:
                writer(1, "\\u001d"_sd);
                break;
            case 0x1e:
                writer(1, "\\u001e"_sd);
                break;
            case 0x1f:
                writer(1, "\\u001f"_sd);
                break;
            case '\\':
                writer(1, "\\\\"_sd);
                break;
            case '\"':
                writer(1, "\\\""_sd);
                break;
            case 0x7f:
                writer(1, "\\u007f"_sd);
                break;
            default:
                break;
        }
    };
    auto invalidByteHandler = [](const auto& writer, uint8_t) {
        // Write Unicode Replacement Character when the encoding is bad
        writer(1, "\\ufffd"_sd);
    };
    auto twoByteEscaper = [](const auto& writer, uint8_t first, uint8_t second) {
        // Decode the UTF-8 and write the codepoint with \u
        uint16_t codepoint = ((first & 0b0001'1111) << 6) | (second & 0b0011'1111);
        std::array<char, 6> buffer = {'\\',
                                      'u',
                                      kHexChar[codepoint >> 12],
                                      kHexChar[(codepoint >> 8) & 0b0000'1111],
                                      kHexChar[(codepoint >> 4) & 0b0000'1111],
                                      kHexChar[codepoint & 0b0000'1111]};
        writer(2, StringData(buffer.data(), buffer.size()));
    };
    return escape(buffer,
                  str,
                  std::move(singleByteHandler),
                  std::move(invalidByteHandler),
                  std::move(twoByteEscaper),
                  maxLength,
                  wouldWrite);
}

void escapeForJSON(fmt::memory_buffer& buffer,
                   StringData str,
                   size_t maxLength,
                   size_t* wouldWrite) {
    escapeForJSONCommon(buffer, str, maxLength, wouldWrite);
}

std::string escapeForJSON(StringData str, size_t maxLength, size_t* wouldWrite) {
    std::string buffer;
    escapeForJSONCommon(buffer, str, maxLength, wouldWrite);
    return buffer;
}

bool validUTF8(StringData str) {
    // No-op buffer and handlers, defined to re-use escape method logic.
    NoopBuffer buffer;
    auto singleByteHandler = [](const auto& writer, uint8_t unescaped) {};
    auto twoByteEscaper = [](const auto& writer, uint8_t first, uint8_t second) {};

    // Throws an exception when an invalid UTF8 character is detected.
    auto invalidByteHandler = [](const auto& writer, uint8_t) {
        uasserted(ErrorCodes::BadValue, "Invalid UTF-8 Character");
    };

    try {
        escape(buffer,
               str,
               std::move(singleByteHandler),
               std::move(invalidByteHandler),
               std::move(twoByteEscaper),
               std::string::npos,
               nullptr);
        return true;
    } catch (const ExceptionFor<ErrorCodes::BadValue>&) {
        return false;
    }
}
}  // namespace mongo::str
