// linenoise_utf8.cpp
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

#include "mongo/shell/linenoise_utf8.h"

#ifdef _WIN32
#include "mongo/platform/windows_basic.h"
#include "mongo/util/text.h"
#include <io.h>
#else
#include <unistd.h>
#endif

namespace linenoise_utf8 {

/**
 * Convert a null terminated UTF-8 string from UTF-8 and store it in a UChar32 destination buffer
 * Always null terminates the destination string if at least one character position is available
 * Errors in the UTF-8 encoding will be handled in two ways: the erroneous characters will be
 * converted to the Unicode error character U+FFFD and flag bits will be set in the
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
                     int& conversionErrorCode) {
    conversionErrorCode = BadUTF8_no_error;
    if (outputBufferSizeInCharacters == 0) {
        outputUnicodeCharacterCount = 0;
        return;
    }
    static const UChar32 errorCharacter = 0xFFFD;
    const UChar8* pIn = utf8input;
    UChar32* pOut = uchar32output;
    UChar32 uchar32;
    int reducedBufferSize = outputBufferSizeInCharacters - 1;
    while (*pIn && (pOut - uchar32output) < reducedBufferSize) {
        // default to error character so we don't set this in 18 places below
        uchar32 = errorCharacter;

        if (pIn[0] <= 0x7F) {  // 0x00000000 to 0x0000007F
            uchar32 = pIn[0];
            pIn += 1;
        } else if (pIn[0] <= 0xDF) {  // 0x00000080 to 0x000007FF
            if ((pIn[0] >= 0xC2) && (pIn[1] >= 0x80) && (pIn[1] <= 0xBF)) {
                uchar32 = ((pIn[0] & 0x1F) << 6) | (pIn[1] & 0x3F);
                pIn += 2;
            } else {
                conversionErrorCode |= BadUTF8_invalid_byte;
                pIn += 1;
            }
        } else if (pIn[0] == 0xE0) {  // 0x00000800 to 0x00000FFF
            if ((pIn[1] >= 0xA0) && (pIn[1] <= 0xBF)) {
                if ((pIn[2] >= 0x80) && (pIn[2] <= 0xBF)) {
                    uchar32 = ((pIn[1] & 0x3F) << 6) | (pIn[2] & 0x3F);
                    pIn += 3;
                } else {
                    conversionErrorCode |= BadUTF8_invalid_byte;
                    pIn += 2;
                }
            } else {
                conversionErrorCode |= BadUTF8_invalid_byte;
                pIn += 1;
            }
        } else if (pIn[0] <= 0xEC) {  // 0x00001000 to 0x0000CFFF
            if ((pIn[1] >= 0x80) && (pIn[1] <= 0xBF)) {
                if ((pIn[2] >= 0x80) && (pIn[2] <= 0xBF)) {
                    uchar32 = ((pIn[0] & 0x0F) << 12) | ((pIn[1] & 0x3F) << 6) | (pIn[2] & 0x3F);
                    pIn += 3;
                } else {
                    conversionErrorCode |= BadUTF8_invalid_byte;
                    pIn += 2;
                }
            } else {
                conversionErrorCode |= BadUTF8_invalid_byte;
                pIn += 1;
            }
        } else if (pIn[0] == 0xED) {  // 0x0000D000 to 0x0000D7FF
            if ((pIn[1] >= 0x80) && (pIn[1] <= 0x9F)) {
                if ((pIn[2] >= 0x80) && (pIn[2] <= 0xBF)) {
                    uchar32 = (0x0D << 12) | ((pIn[1] & 0x3F) << 6) | (pIn[2] & 0x3F);
                    pIn += 3;
                } else {
                    conversionErrorCode |= BadUTF8_invalid_byte;
                    pIn += 2;
                }
            }
            //                          // 0x0000D800 to 0x0000DFFF -- illegal surrogate value
            else if ((pIn[1] >= 0x80) && (pIn[1] <= 0xBF)) {
                if ((pIn[2] >= 0x80) && (pIn[2] <= 0xBF)) {
                    conversionErrorCode |= BadUTF8_surrogate;
                    pIn += 3;
                } else {
                    conversionErrorCode |= BadUTF8_invalid_byte;
                    pIn += 2;
                }
            } else {
                conversionErrorCode |= BadUTF8_invalid_byte;
                pIn += 1;
            }
        } else if (pIn[0] <= 0xEF) {  // 0x0000E000 to 0x0000FFFF
            if ((pIn[1] >= 0x80) && (pIn[1] <= 0xBF)) {
                if ((pIn[2] >= 0x80) && (pIn[2] <= 0xBF)) {
                    uchar32 = ((pIn[0] & 0x0F) << 12) | ((pIn[1] & 0x3F) << 6) | (pIn[2] & 0x3F);
                    pIn += 3;
                } else {
                    conversionErrorCode |= BadUTF8_invalid_byte;
                    pIn += 2;
                }
            } else {
                conversionErrorCode |= BadUTF8_invalid_byte;
                pIn += 1;
            }
        } else if (pIn[0] == 0xF0) {  // 0x00010000 to 0x0003FFFF
            if ((pIn[1] >= 0x90) && (pIn[1] <= 0xBF)) {
                if ((pIn[2] >= 0x80) && (pIn[2] <= 0xBF)) {
                    if ((pIn[3] >= 0x80) && (pIn[3] <= 0xBF)) {
                        uchar32 =
                            ((pIn[1] & 0x3F) << 12) | ((pIn[2] & 0x3F) << 6) | (pIn[3] & 0x3F);
                        pIn += 4;
                    } else {
                        conversionErrorCode |= BadUTF8_invalid_byte;
                        pIn += 3;
                    }
                } else {
                    conversionErrorCode |= BadUTF8_invalid_byte;
                    pIn += 2;
                }
            } else {
                conversionErrorCode |= BadUTF8_invalid_byte;
                pIn += 1;
            }
        } else if (pIn[0] <= 0xF4) {  // 0x00040000 to 0x0010FFFF
            if ((pIn[1] >= 0x80) && (pIn[1] <= 0xBF)) {
                if ((pIn[2] >= 0x80) && (pIn[2] <= 0xBF)) {
                    if ((pIn[3] >= 0x80) && (pIn[3] <= 0xBF)) {
                        uchar32 = ((pIn[0] & 0x07) << 18) | ((pIn[1] & 0x3F) << 12) |
                            ((pIn[2] & 0x3F) << 6) | (pIn[3] & 0x3F);
                        pIn += 4;
                    } else {
                        conversionErrorCode |= BadUTF8_invalid_byte;
                        pIn += 3;
                    }
                } else {
                    conversionErrorCode |= BadUTF8_invalid_byte;
                    pIn += 2;
                }
            } else {
                conversionErrorCode |= BadUTF8_invalid_byte;
                pIn += 1;
            }
        } else {
            conversionErrorCode |= BadUTF8_invalid_byte;
            pIn += 1;
        }
        if (uchar32 != 0xFEFF) {  // do not store Byte Order Mark
            *pOut++ = uchar32;
        }
    }
    *pOut = 0;
    outputUnicodeCharacterCount = pOut - uchar32output;
}

/**
 * Copy a null terminated UChar32 string to a UChar32 destination buffer
 * Always null terminates the destination string if at least one character position is available
 *
 * @param dest32                    Destination UChar32 buffer
 * @param source32                  Source UChar32 string
 * @param destLengthInCharacters    Destination buffer length in characters
 */
void copyString32(UChar32* dest32, const UChar32* source32, size_t destLengthInCharacters) {
    if (destLengthInCharacters) {
        while (*source32 && --destLengthInCharacters > 0) {
            *dest32++ = *source32++;
        }
        *dest32 = 0;
    }
}

/**
 * Convert a specified number of UChar32 characters from a possibly null terminated UChar32 string
 * to UTF-8 and store it in a UChar8 destination buffer
 * Always null terminates the destination string if at least one character position is available
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
                              size_t charCount) {
    size_t outputUTF8ByteCount = 0;
    if (outputBufferSizeInBytes) {
        size_t reducedBufferSize = outputBufferSizeInBytes - 4;
        while (charCount-- && *source32 && outputUTF8ByteCount < reducedBufferSize) {
            UChar32 c = *source32++;
            if (c <= 0x7F) {
                *dest8++ = c;
                outputUTF8ByteCount += 1;
            } else if (c <= 0x7FF) {
                *dest8++ = 0xC0 | (c >> 6);
                *dest8++ = 0x80 | (0x3F & c);
                outputUTF8ByteCount += 2;
            } else if (c <= 0xFFFF) {
                *dest8++ = 0xE0 | (c >> 12);
                *dest8++ = 0x80 | (0x3F & (c >> 6));
                *dest8++ = 0x80 | (0x3F & c);
                outputUTF8ByteCount += 3;
            } else if (c <= 0x1FFFFF) {
                *dest8++ = 0xF0 | (c >> 18);
                *dest8++ = 0x80 | (0x3F & (c >> 12));
                *dest8++ = 0x80 | (0x3F & (c >> 6));
                *dest8++ = 0x80 | (0x3F & c);
                outputUTF8ByteCount += 4;
            }
        }
        *dest8 = 0;
    }
    return outputUTF8ByteCount;
}

/**
 * Convert a null terminated UChar32 string to UTF-8 and store it in a UChar8 destination buffer
 * Always null terminates the destination string if at least one character position is available
 *
 * @param dest8                     Destination UChar8 buffer
 * @param source32                  Source UChar32 string
 * @param outputBufferSizeInBytes   Destination buffer size in bytes
 * @return                          Count of bytes written to output buffer, not including null
 *                                  terminator
 */
size_t copyString32to8(UChar8* dest8, const UChar32* source32, size_t outputBufferSizeInBytes) {
    return copyString32to8counted(dest8, source32, outputBufferSizeInBytes, 0x7FFFFFFF);
}

/**
 * Count characters (i.e. Unicode code points, array elements) in a null terminated UChar32 string
 *
 * @param str32     Source UChar32 string
 * @return          String length in characters
 */
size_t strlen32(const UChar32* str32) {
    size_t length = 0;
    while (*str32++) {
        ++length;
    }
    return length;
}

/**
 * Compare two UChar32 null-terminated strings with length parameter
 *
 * @param first32   First string to compare
 * @param second32  Second string to compare
 * @param length    Maximum number of characters to compare
 * @return          Negative if first < second, positive if first > second, zero if equal
 */
int strncmp32(UChar32* first32, UChar32* second32, size_t length) {
    while (length--) {
        if (*first32 == 0 || *first32 != *second32) {
            return *first32 - *second32;
        }
        ++first32;
        ++second32;
    }
    return 0;
}

/**
 * Internally convert an array of UChar32 characters of specified length to UTF-8 and write it to
 * fileHandle
 *
 * @param fileHandle                File handle to write to
 * @param string32                  Source UChar32 characters, may not be null terminated
 * @param sourceLengthInCharacters  Number of source characters to convert and write
 * @return                          Number of bytes written, -1 on error
 */
int write32(int fileHandle, const UChar32* string32, unsigned int sourceLengthInCharacters) {
    size_t tempBufferBytes = 4 * sourceLengthInCharacters + 1;
    std::unique_ptr<char[]> tempCharString(new char[tempBufferBytes]);
    size_t count = copyString32to8counted(reinterpret_cast<UChar8*>(tempCharString.get()),
                                          string32,
                                          tempBufferBytes,
                                          sourceLengthInCharacters);
#if defined(_WIN32)
    if (_isatty(fileHandle)) {
        bool success = mongo::writeUtf8ToWindowsConsole(tempCharString.get(), count);
        if (!success) {
            return -1;
        }
        return count;
    } else {
        return _write(fileHandle, tempCharString.get(), count);
    }
#else
    return write(fileHandle, tempCharString.get(), count);
#endif
}

}  // namespace linenoise_utf8
