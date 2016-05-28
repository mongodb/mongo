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

#include "mongo/logger/rotatable_file_writer.h"

#include <boost/filesystem/operations.hpp>
#include <cstdio>
#include <fstream>

#include "mongo/base/string_data.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace logger {

namespace {
/**
 * Renames file "oldName" to "newName".
 *
 * Both names are UTF-8 encoded.
 */
int renameFile(const std::string& oldName, const std::string& newName);
}  // namespace

#ifdef _WIN32
namespace {

/**
 * Converts UTF-8 encoded "utf8Str" to std::wstring.
 */
std::wstring utf8ToWide(StringData utf8Str) {
    if (utf8Str.empty())
        return std::wstring();

    // A Windows wchar_t encoding of a unicode codepoint never takes more instances of wchar_t
    // than the UTF-8 encoding takes instances of char.
    std::unique_ptr<wchar_t[]> tempBuffer(new wchar_t[utf8Str.size()]);
    tempBuffer[0] = L'\0';
    int finalSize = MultiByteToWideChar(CP_UTF8,            // Code page
                                        0,                  // Flags
                                        utf8Str.rawData(),  // Input string
                                        utf8Str.size(),     // Count
                                        tempBuffer.get(),   // UTF-16 output buffer
                                        utf8Str.size()      // Buffer size in wide characters
                                        );
    // TODO(schwerin): fassert finalSize > 0?
    return std::wstring(tempBuffer.get(), finalSize);
}


/**
 * Minimal implementation of a std::streambuf for writing to Win32 files via HANDLEs.
 *
 * We require this implementation and the std::ostream subclass below to handle the following:
 * (1) Opening files for shared-delete access, so that open file handles may be renamed.
 * (2) Opening files with non-ASCII characters in their names.
 */
class Win32FileStreambuf : public std::streambuf {
    MONGO_DISALLOW_COPYING(Win32FileStreambuf);

public:
    Win32FileStreambuf();
    virtual ~Win32FileStreambuf();

    bool open(StringData fileName, bool append);
    bool is_open() {
        return _fileHandle != INVALID_HANDLE_VALUE;
    }

private:
    virtual std::streamsize xsputn(const char* s, std::streamsize count);
    virtual int_type overflow(int_type ch = traits_type::eof());

    std::streamsize writeToFile(const char* s, std::streamsize count);

    HANDLE _fileHandle;
};

/**
 * Minimal implementation of a stream to Win32 files.
 */
class Win32FileOStream : public std::ostream {
    MONGO_DISALLOW_COPYING(Win32FileOStream);

public:
    /**
     * Constructs an instance, opening "fileName" in append or truncate mode according to
     * "append".
     */
    Win32FileOStream(const std::string& fileName, bool append) : std::ostream(&_buf), _buf() {
        if (!_buf.open(fileName, append)) {
            setstate(failbit);
        }
    }

    virtual ~Win32FileOStream() {}

private:
    Win32FileStreambuf _buf;
};

Win32FileStreambuf::Win32FileStreambuf() : _fileHandle(INVALID_HANDLE_VALUE) {}
Win32FileStreambuf::~Win32FileStreambuf() {
    if (is_open()) {
        CloseHandle(_fileHandle);  // TODO(schwerin): Should we check for failure?
    }
}

bool Win32FileStreambuf::open(StringData fileName, bool append) {
    _fileHandle = CreateFileW(utf8ToWide(fileName).c_str(),         // lpFileName
                              GENERIC_WRITE,                        // dwDesiredAccess
                              FILE_SHARE_DELETE | FILE_SHARE_READ,  // dwShareMode
                              NULL,                                 // lpSecurityAttributes
                              OPEN_ALWAYS,                          // dwCreationDisposition
                              FILE_ATTRIBUTE_NORMAL,                // dwFlagsAndAttributes
                              NULL                                  // hTemplateFile
                              );


    if (INVALID_HANDLE_VALUE == _fileHandle)
        return false;

    LARGE_INTEGER zero;
    zero.QuadPart = 0LL;

    if (append) {
        if (SetFilePointerEx(_fileHandle, zero, NULL, FILE_END)) {
            return true;
        }
    } else {
        if (SetFilePointerEx(_fileHandle, zero, NULL, FILE_BEGIN) && SetEndOfFile(_fileHandle)) {
            return true;
        }
    }
    // TODO(schwerin): Record error info?
    CloseHandle(_fileHandle);
    return false;
}

std::streamsize Win32FileStreambuf::writeToFile(const char* s, std::streamsize count) {
    DWORD totalBytesWritten = 0;

    while (count > totalBytesWritten) {
        DWORD bytesWritten;
        if (!WriteFile(_fileHandle, s, count - totalBytesWritten, &bytesWritten, NULL)) {
            break;
        }
        totalBytesWritten += bytesWritten;
    }

    return totalBytesWritten;
}

// Called when strings are written to ostream
std::streamsize Win32FileStreambuf::xsputn(const char* s, std::streamsize count) {
    DWORD totalBytesWritten = 0;

    // Scan for embedded newlines before end
    // this should be rare since the newline should only be at the end
    const char* startPos = s;
    for (int i = 0; i < count; i++) {
        if (s[i] == '\n') {
            totalBytesWritten += writeToFile(startPos, i - (startPos - s));
            writeToFile("\r\n", 2);
            totalBytesWritten += 1;  // Caller expected we only wrote 1 char, so tell them so
            startPos = &s[i + 1];
        }
    }

    // Did the string not end on "\n"? Write the remaining, no need for CRLF
    // as upper layers are responsible for it
    if ((startPos - s) != count) {
        totalBytesWritten += writeToFile(startPos, count - (startPos - s));
    }

    return totalBytesWritten;
}

// Overflow is called for single character writes to the ostream
Win32FileStreambuf::int_type Win32FileStreambuf::overflow(int_type ch) {
    if (ch == traits_type::eof())
        return ~ch;  // Returning traits_type::eof() => failure, anything else => success.
    char toPut = static_cast<char>(ch);
    if (1 == xsputn(&toPut, 1))
        return ch;
    return traits_type::eof();
}

// Win32 implementation of renameFile that handles non-ascii file names.
int renameFile(const std::string& oldName, const std::string& newName) {
    return _wrename(utf8ToWide(oldName).c_str(), utf8ToWide(newName).c_str());
}

}  // namespace
#else

namespace {

// *nix implementation of renameFile that assumes the OS is encoding file names in UTF-8.
int renameFile(const std::string& oldName, const std::string& newName) {
    return rename(oldName.c_str(), newName.c_str());
}

}  // namespace
#endif

RotatableFileWriter::RotatableFileWriter() : _stream(nullptr) {}

RotatableFileWriter::Use::Use(RotatableFileWriter* writer)
    : _writer(writer), _lock(writer->_mutex) {}

Status RotatableFileWriter::Use::setFileName(const std::string& name, bool append) {
    _writer->_fileName = name;
    return _openFileStream(append);
}

Status RotatableFileWriter::Use::rotate(bool renameOnRotate, const std::string& renameTarget) {
    if (_writer->_stream) {
        _writer->_stream->flush();

        if (renameOnRotate) {
            try {
                if (boost::filesystem::exists(renameTarget)) {
                    return Status(
                        ErrorCodes::FileRenameFailed,
                        mongoutils::str::stream() << "Renaming file " << _writer->_fileName
                                                  << " to "
                                                  << renameTarget
                                                  << " failed; destination already exists");
                }
            } catch (const std::exception& e) {
                return Status(
                    ErrorCodes::FileRenameFailed,
                    mongoutils::str::stream() << "Renaming file " << _writer->_fileName << " to "
                                              << renameTarget
                                              << " failed; Cannot verify whether destination "
                                                 "already exists: "
                                              << e.what());
            }

            if (0 != renameFile(_writer->_fileName, renameTarget)) {
                return Status(ErrorCodes::FileRenameFailed,
                              mongoutils::str::stream() << "Failed  to rename \""
                                                        << _writer->_fileName
                                                        << "\" to \""
                                                        << renameTarget
                                                        << "\": "
                                                        << strerror(errno)
                                                        << " ("
                                                        << errno
                                                        << ')');
                // TODO(schwerin): Make errnoWithDescription() available in the logger library, and
                // use it here.
            }
        }
    }
    return _openFileStream(false);
}

Status RotatableFileWriter::Use::status() {
    if (!_writer->_stream) {
        return Status(ErrorCodes::FileNotOpen,
                      mongoutils::str::stream() << "File \"" << _writer->_fileName
                                                << "\" not open");
    }
    if (_writer->_stream->fail()) {
        return Status(ErrorCodes::FileStreamFailed,
                      mongoutils::str::stream() << "File \"" << _writer->_fileName
                                                << "\" in failed state");
    }
    return Status::OK();
}

Status RotatableFileWriter::Use::_openFileStream(bool append) {
    using std::swap;

#ifdef _WIN32
    std::unique_ptr<std::ostream> newStream(new Win32FileOStream(_writer->_fileName, append));
#else
    std::ios::openmode mode = std::ios::out;
    if (append) {
        mode |= std::ios::app;
    } else {
        mode |= std::ios::trunc;
    }
    std::unique_ptr<std::ostream> newStream(new std::ofstream(_writer->_fileName.c_str(), mode));
#endif

    if (newStream->fail()) {
        return Status(ErrorCodes::FileNotOpen, "Failed to open \"" + _writer->_fileName + "\"");
    }
    swap(_writer->_stream, newStream);
    return status();
}

}  // namespace logger
}  // namespace mongo
