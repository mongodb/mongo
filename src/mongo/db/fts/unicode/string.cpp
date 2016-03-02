/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/fts/unicode/string.h"

#include <algorithm>
#include <boost/algorithm/searching/boyer_moore.hpp>

#include "mongo/db/fts/unicode/byte_vector.h"
#include "mongo/platform/bits.h"
#include "mongo/shell/linenoise_utf8.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace unicode {

namespace {
template <typename OutputIterator>
inline void appendUtf8Codepoint(char32_t codepoint, OutputIterator* outputIt) {
    if (codepoint <= 0x7f /* max 1-byte codepoint */) {
        *(*outputIt)++ = (codepoint);
    } else if (codepoint <= 0x7ff /* max 2-byte codepoint*/) {
        *(*outputIt)++ = ((codepoint >> (6 * 1)) | 0xc0);  // 2 leading 1s.
        *(*outputIt)++ = (((codepoint >> (6 * 0)) & 0x3f) | 0x80);
    } else if (codepoint <= 0xffff /* max 3-byte codepoint*/) {
        *(*outputIt)++ = ((codepoint >> (6 * 2)) | 0xe0);  // 3 leading 1s.
        *(*outputIt)++ = (((codepoint >> (6 * 1)) & 0x3f) | 0x80);
        *(*outputIt)++ = (((codepoint >> (6 * 0)) & 0x3f) | 0x80);
    } else {
        uassert(ErrorCodes::BadValue, "text contains invalid UTF-8", codepoint <= 0x10FFFF);
        *(*outputIt)++ = ((codepoint >> (6 * 3)) | 0xf0);  // 4 leading 1s.
        *(*outputIt)++ = (((codepoint >> (6 * 2)) & 0x3f) | 0x80);
        *(*outputIt)++ = (((codepoint >> (6 * 1)) & 0x3f) | 0x80);
        *(*outputIt)++ = (((codepoint >> (6 * 0)) & 0x3f) | 0x80);
    }
}
}

using linenoise_utf8::copyString32to8;
using linenoise_utf8::copyString8to32;

using std::u32string;

String::String(const StringData utf8_src) {
    // Convert UTF-8 input to UTF-32 data.
    setData(utf8_src);
}

void String::resetData(const StringData utf8_src) {
    // Convert UTF-8 input to UTF-32 data.
    setData(utf8_src);
}

void String::setData(const StringData utf8_src) {
    // _data is the target, resize it so that it's guaranteed to fit all of the input characters,
    // plus a null character if there isn't one.
    _data.resize(utf8_src.size() + 1);

    int result = 0;
    size_t resultSize = 0;

    // Although utf8_src.rawData() is not guaranteed to be null-terminated, copyString8to32 won't
    // access bad memory because it is limited by the size of its output buffer, which is set to the
    // size of utf8_src.
    copyString8to32(&_data[0],
                    reinterpret_cast<const unsigned char*>(&utf8_src.rawData()[0]),
                    _data.size(),
                    resultSize,
                    result);

    uassert(28755, "text contains invalid UTF-8", result == 0);

    // Resize _data so it is only as big as what it contains.
    _data.resize(resultSize);
    _needsOutputConversion = true;
}

std::string String::toString() {
    // _outputBuf is the target, resize it so that it's guaranteed to fit all of the input
    // characters, plus a null character if there isn't one.
    if (_needsOutputConversion) {
        _outputBuf.resize(_data.size() * 4 + 1);
        size_t resultSize = copyString32to8(
            reinterpret_cast<unsigned char*>(&_outputBuf[0]), &_data[0], _outputBuf.size());

        // Resize output so it is only as large as what it contains.
        _outputBuf.resize(resultSize);
        _needsOutputConversion = false;
    }
    return _outputBuf;
}

template <typename Func>
StringData String::substrToBufWithTransform(StackBufBuilder* buffer,
                                            size_t pos,
                                            size_t len,
                                            Func func) const {
    pos = std::min(pos, _data.size());
    len = std::min(len, _data.size() - pos);

    buffer->reset();
    auto outputIt = buffer->skip(len * 4);  // Reserve room for worst-case expansion.
    auto inputIt = _data.begin() + pos;
    for (size_t i = 0; i < len; i++) {
        appendUtf8Codepoint(func(*inputIt++), &outputIt);
    }
    buffer->setlen(outputIt - buffer->buf());
    return {buffer->buf(), size_t(buffer->len())};
}

StringData String::substrToBuf(StackBufBuilder* buffer, size_t pos, size_t len) const {
    const auto identityFunc = [](char32_t ch) { return ch; };
    return substrToBufWithTransform(buffer, pos, len, identityFunc);
}

StringData String::toLowerToBuf(StackBufBuilder* buffer,
                                CaseFoldMode mode,
                                size_t pos,
                                size_t len) const {
    const auto toLower = [mode](char32_t ch) { return codepointToLower(ch, mode); };
    return substrToBufWithTransform(buffer, pos, len, toLower);
}


StringData String::caseFoldAndStripDiacritics(StackBufBuilder* buffer,
                                              StringData utf8,
                                              SubstrMatchOptions options,
                                              CaseFoldMode mode) {
    // This fires if your input buffer the same as your output buffer.
    invariant(buffer->buf() != utf8.rawData());

    if ((options & kCaseSensitive) && (options & kDiacriticSensitive)) {
        // No transformation needed. Just return the input data unmodified.
        return utf8;
    }

    // Allocate space for up to 2x growth which is the worst possible case for stripping diacritics
    // and casefolding. Proof: the only case where 1 byte goes to >1 is 'I' in Turkish going to 2
    // bytes. The biggest codepoint is 4 bytes which is also 2x 2 bytes. This holds as long as we
    // don't map a single code point to more than one.
    buffer->reset();
    auto outputIt = buffer->skip(utf8.size() * 2);

    for (auto inputIt = utf8.begin(), endIt = utf8.end(); inputIt != endIt;) {
#ifdef MONGO_HAVE_FAST_BYTE_VECTOR
        if (size_t(endIt - inputIt) >= ByteVector::size) {
            // Try the fast path for 16 contiguous bytes of ASCII.
            auto word = ByteVector::load(&*inputIt);

            // Count the bytes of ASCII.
            uint32_t usableBytes = ByteVector::countInitialZeros(word.maskHigh());
            if (usableBytes) {
                if (!(options & kCaseSensitive)) {
                    if (mode == CaseFoldMode::kTurkish) {
                        ByteVector::Mask iMask = word.compareEQ('I').maskAny();
                        if (iMask) {
                            usableBytes =
                                std::min(usableBytes, ByteVector::countInitialZeros(iMask));
                        }
                    }
                    // 0xFF for each byte in word that is uppercase, 0x00 for all others.
                    ByteVector uppercaseMask = word.compareGT('A' - 1) & word.compareLT('Z' + 1);
                    word |= (uppercaseMask & ByteVector(0x20));  // Set the ascii lowercase bit.
                }

                if (!(options & kDiacriticSensitive)) {
                    ByteVector::Mask diacriticMask =
                        word.compareEQ('^').maskAny() | word.compareEQ('`').maskAny();
                    if (diacriticMask) {
                        usableBytes =
                            std::min(usableBytes, ByteVector::countInitialZeros(diacriticMask));
                    }
                }

                word.store(&*outputIt);
                outputIt += usableBytes;
                inputIt += usableBytes;
                if (usableBytes == ByteVector::size)
                    continue;
            }
            // If we get here, inputIt is positioned on a byte that we know needs special handling.
            // Either it isn't ASCII or it is a diacritic that needs to be stripped.
        }
#endif
        const uint8_t firstByte = *inputIt++;
        char32_t codepoint = 0;
        if (firstByte <= 0x7f) {
            // ASCII special case. Can use faster operations.
            if ((!(options & kCaseSensitive)) && (firstByte >= 'A' && firstByte <= 'Z')) {
                codepoint = (mode == CaseFoldMode::kTurkish && firstByte == 'I')
                    ? 0x131                // In Turkish, I -> ı (i with no dot).
                    : (firstByte | 0x20);  // Set the ascii lowercase bit on the character.
            } else {
                // ASCII has two pure diacritics that should be skipped and no characters that
                // change when removing diacritics.
                if ((options & kDiacriticSensitive) || !(firstByte == '^' || firstByte == '`')) {
                    *outputIt++ = (firstByte);
                }
                continue;
            }
        } else {
            // firstByte indicates that it is not an ASCII char.
            int leadingOnes = countLeadingZeros64(~(uint64_t(firstByte) << (64 - 8)));

            // Only checking enough to ensure that this code doesn't crash in the face of malformed
            // utf-8. We make no guarantees about what results will be returned in this case.
            uassert(ErrorCodes::BadValue,
                    "text contains invalid UTF-8",
                    leadingOnes > 1 && leadingOnes <= 4 && inputIt + leadingOnes - 1 <= endIt);

            codepoint = firstByte & (0xff >> leadingOnes);  // mask off the size indicator.
            for (int subByteIx = 1; subByteIx < leadingOnes; subByteIx++) {
                const uint8_t subByte = *inputIt++;
                codepoint <<= 6;
                codepoint |= subByte & 0x3f;  // mask off continuation bits.
            }

            if (!(options & kCaseSensitive)) {
                codepoint = codepointToLower(codepoint, mode);
            }

            if (!(options & kDiacriticSensitive)) {
                codepoint = codepointRemoveDiacritics(codepoint);
                if (!codepoint)
                    continue;  // codepoint is a pure diacritic.
            }
        }

        appendUtf8Codepoint(codepoint, &outputIt);
    }

    buffer->setlen(outputIt - buffer->buf());
    return {buffer->buf(), size_t(buffer->len())};
}

bool String::substrMatch(const std::string& str,
                         const std::string& find,
                         SubstrMatchOptions options,
                         CaseFoldMode cfMode) {
    if (cfMode == CaseFoldMode::kTurkish) {
        // Turkish comparisons are always case insensitive due to their handling of I/i.
        options &= ~kCaseSensitive;
    }

    StackBufBuilder haystackBuf;
    StackBufBuilder needleBuf;
    auto haystack = caseFoldAndStripDiacritics(&haystackBuf, str, options, cfMode);
    auto needle = caseFoldAndStripDiacritics(&needleBuf, find, options, cfMode);

    // Case sensitive and diacritic sensitive.
    return boost::algorithm::boyer_moore_search(
               haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

}  // namespace unicode
}  // namespace mongo
