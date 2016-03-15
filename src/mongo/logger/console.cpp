/*    Copyright 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/logger/console.h"

#include <iostream>

#include "mongo/base/init.h"

#ifdef _WIN32
#include <io.h>
#endif

namespace mongo {
namespace {

/*
 * Theory of operation:
 *
 * At process start, the loader initializes "consoleMutex" to NULL.  At some point during static
 * initialization, the static initialization process, running in the one and only extant thread,
 * allocates a new stdx::mutex on the heap and assigns consoleMutex to point to it.  While
 * consoleMutex is still NULL, we know that there is only one thread extant, so it is safe to
 * skip locking the consoleMutex in the Console constructor.  Once the mutex is initialized,
 * users of Console can start acquiring it.
 */

stdx::mutex* consoleMutex = new stdx::mutex;

#if defined(_WIN32)
/**
 * Very basic implementation of a stream buffer backed by
 * a fixed sized internal buffer that flushes
 * to the console using WriteConsoleW().
 *
 * Implementation notes:
 * WriteConsoleW() vs std::wcout
 *     std::wcout produces garbage output for messages containing 2-byte sequences
 *     under all the terminal environments tested (Command Prompt, Power Shell, Cygwin).
 *     3-byte sequences will cause std::wcout to go into a bad state (failbit=true).
 *     Also tried calling _setmode( _fileno(stdout), ...) with various modes (_O_U16TEXT,
 *     ...), all of which produces garbage results on the console.
 */

class ConsoleStreamBuffer : public std::streambuf {
public:
    explicit ConsoleStreamBuffer(HANDLE consoleHandle) : _consoleHandle(consoleHandle) {
        // leave room at end of buffer for overflow character
        setp(&(_buffer[0]), &(_buffer[_bufferSize - 1]));
    }

    int_type overflow(int_type ch) {
        if (ch == traits_type::eof()) {
            return ch;
        }

        // push to end of buffer
        *(pptr()) = ch;
        pbump(1);

        // If the overflow byte is part of a UTF-8 multi-byte sequence,
        // locate the beginning of the byte sequence and determine if
        // the byte sequence represents a complete unicode code point.
        // If the sequence is complete, proceed to flush the buffer.
        // If the sequence is incomplete, flush the buffer up to but
        // not including incomplete code point before re-inserting the
        // bytes back into the internal buffer.

        if (ch & 0x80) {
            // length of unicode byte sequence can be at most 4 in length. See RFC 3629
            int length = 0;

            // do not look back beyond 4 characters (maximum length for UTF-8 sequences)
            // p will point to first byte of unicode code point upon exit from loop
            char* sequenceBegin = pptr();
            for (int i = 0; i < 4; i++) {
                length++;
                sequenceBegin--;
                // check for beginning of code point
                if (*sequenceBegin & 0x40) {
                    break;
                }
                // check for invalid byte. all bytes in multi-byte sequence
                // should have left most bit set
                if (!(*sequenceBegin & 0x80)) {
                    break;
                }
            }

            // get expected length of code point
            // cast to unsigned type to avoid sign extension
            int expectedLength = _bitsToSequenceLength[uint8_t(*sequenceBegin) >> 4];

            // if beginning of sequence was not found, expectedLength will be zero and
            // we will consider the multi-byte sequence to be garbage and flush the
            // entire buffer

            // if code point is incomplete, rewind and flush buffer before
            // refilling with incomplete byte sequence
            if (length < expectedLength) {
                // rewind internal buffer to end before unfinished byte sequence
                pbump(-length);

                // store result. should eventually return
                // original overflow character on success
                int_type result = flushToConsole() ? ch : traits_type::eof();

                // after flushing pptr() will point to beginning of buffer
                // copy bytes starting from p to beginning of internal buffer
                for (int i = 0; i < length; i++) {
                    *(pptr()) = *sequenceBegin;
                    pbump(1);
                    sequenceBegin++;
                }

                return result;
            }
        }

        return flushToConsole() ? ch : traits_type::eof();
    }

    int sync() {
        return flushToConsole() ? 0 : -1;
    }

private:
    // keep this value reasonable. this class is used primarily
    // to buffer log messages
    static const size_t _bufferSize = 1024U;

    // mapping of leftmost 4 bits of first byte of code point
    // to number of expected bytes in complete code point
    // 110x -> 2
    // 1110 -> 3
    // 1111 -> 4
    static const int _bitsToSequenceLength[];

    // In the event that WriteConsoleW fails, return false
    // to allow stream to update error state flags (most likely badbit)

    bool flushToConsole() {
        std::ptrdiff_t n = pptr() - pbase();
        pbump(-n);

        // convert multi-byte buffer to wide characters and output using WriteConsoleW

        wchar_t bufferWide[_bufferSize];
        int length = MultiByteToWideChar(CP_UTF8, 0, _buffer, n, bufferWide, _bufferSize);
        const wchar_t* unwrittenBegin = bufferWide;
        int unwrittenCount = length;  // m holds number of unwritten wide characters in buffer

        while (unwrittenCount > 0) {
            DWORD written;
            BOOL success =
                WriteConsoleW(_consoleHandle, unwrittenBegin, unwrittenCount, &written, NULL);
            if (!success) {
                return false;
            }
            unwrittenCount -= written;
            unwrittenBegin += written;
        }
        return true;
    }

    HANDLE _consoleHandle;
    char _buffer[_bufferSize];
};

// 0x0 - 0xb - invalid start of multi-byte sequence`

const int ConsoleStreamBuffer::_bitsToSequenceLength[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 3, 4};

// Create a output stream to redirect console writes
// to WriteConsoleW() if there is a real console available (FILE_TYPE_CHAR)
// Otherwise, return std::cout
//
// Command Prompt and Power Shell (GetFileType() == FILE_TYPE_CHAR):
//     Set the font to either Lucida Console or Consolas to see 2 and 3-byte sequences.
//     The fonts provided by the existing Windows console programs do not
//     render complex 4-byte sequences propertly.
// File redirection (GetFileType() == FILE_TYPE_DISK):
//     When the output is redirected from the console, WriteConsoleW will fail and we
//     will fall back on using _write().
// Cygwin (GetFileType() == FILE_TYPE_PIPE):
//     An "invalid handle" error message will be displayed on first log message and
//     flushToConsole() will fall back on using _write().

std::ostream* getWindowsOutputStream() {
    HANDLE consoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD consoleType = GetFileType(consoleHandle);
    if (consoleType != FILE_TYPE_CHAR) {
        return &std::cout;
    }
    std::streambuf* windowsStreamBuffer = new ConsoleStreamBuffer(consoleHandle);
    std::ostream* windowsOutputStream = new std::ostream(windowsStreamBuffer);
    return windowsOutputStream;
}

std::ostream* windowsOutputStream = getWindowsOutputStream();
#endif  // defined(_WIN32)

// This initializer causes a Console object to be constructed, which,
// in turn, causes std::ios_base::Init::Init to be called from a
// single threaded context, ensuring that std::cout is prepared in a
// single threaded context and available for all TUs. This is safe
// even for TUs that do not include <iostream>, since either the call
// to Console() here is the first call to that constructor, or the
// first logging call writing via the Console will happen elsewhere in
// the initalizer chain.
MONGO_INITIALIZER(EnsureIosBaseInitConstructed)(InitializerContext*) {
    Console();
    return Status::OK();
}

}  // namespace

Console::Console() : _consoleLock() {
    // Don't get clever and make this non-static. At least when using
    // libc++, ios::base::Init::Init is not thread safe (see
    // SERVER-22974). Instead, rely on C++11 magic statics to ensure
    // that we create this at the right time. Some systems (like
    // VS2013 and older), don't actually implement magic statics
    // yet. But that is OK! We ensure that this is called from
    // single-threaded context via a mongo initializer above.
    static const std::ios_base::Init initializeCout;

    if (consoleMutex) {
        stdx::unique_lock<stdx::mutex> lk(*consoleMutex);
        lk.swap(_consoleLock);
    }
}

std::ostream& Console::out() {
#if defined(_WIN32)
    // check value of ostream in case
    // static initializer has not been invoked
    if (windowsOutputStream) {
        return *windowsOutputStream;
    }
#endif  // defined(_WIN32)
    return std::cout;
}

}  // namespace mongo
