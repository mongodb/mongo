/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/logger/rotatable_file_writer.h"

#include <boost/scoped_array.hpp>
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
    std::wstring utf8ToWide(const StringData& utf8Str) {
        if (utf8Str.empty())
            return std::wstring();

        // A Windows wchar_t encoding of a unicode codepoint never takes more instances of wchar_t
        // than the UTF-8 encoding takes instances of char.
        boost::scoped_array<wchar_t> tempBuffer(new wchar_t[utf8Str.size()]);
        tempBuffer[0] = L'\0';
        int finalSize = MultiByteToWideChar(
                CP_UTF8,               // Code page
                0,                     // Flags
                utf8Str.rawData(),     // Input string
                utf8Str.size(),        // Count
                tempBuffer.get(),      // UTF-16 output buffer
                utf8Str.size()         // Buffer size in wide characters
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

        bool open(const StringData& fileName, bool append);
        bool is_open() { return _fileHandle != INVALID_HANDLE_VALUE; }

    private:
        virtual std::streamsize xsputn(const char* s, std::streamsize count);
        virtual int_type overflow(int_type ch = traits_type::eof());

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

    bool Win32FileStreambuf::open(const StringData& fileName, bool append) {
        _fileHandle = CreateFileW(
                utf8ToWide(fileName).c_str(),         // lpFileName
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
        }
        else {
            if (SetFilePointerEx(_fileHandle, zero, NULL, FILE_BEGIN) &&
                SetEndOfFile(_fileHandle)) {

                return true;
            }

        }
        // TODO(schwerin): Record error info?
        CloseHandle(_fileHandle);
        return false;
    }

    std::streamsize Win32FileStreambuf::xsputn(const char* s, std::streamsize count) {
        DWORD totalBytesWritten = 0;
        while (count > totalBytesWritten) {
            DWORD bytesWritten;
            if (!WriteFile(
                        _fileHandle,
                        s,
                        count - totalBytesWritten,
                        &bytesWritten,
                        NULL)) {
                break;
            }
            totalBytesWritten += bytesWritten;
        }
        return totalBytesWritten;
    }

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

    RotatableFileWriter::RotatableFileWriter() : _stream(NULL) {}

    RotatableFileWriter::Use::Use(RotatableFileWriter* writer) :
        _writer(writer),
        _lock(writer->_mutex) {
    }

    Status RotatableFileWriter::Use::setFileName(const std::string& name, bool append) {
        _writer->_fileName = name;
        return _openFileStream(append);
    }

    Status RotatableFileWriter::Use::rotate(const std::string& renameTarget) {
        if (_writer->_stream) {
            _writer->_stream->flush();
            if (0 != renameFile(_writer->_fileName, renameTarget)) {
                return Status(ErrorCodes::FileRenameFailed, mongoutils::str::stream() <<
                              "Failed  to rename \"" << _writer->_fileName << "\" to \"" <<
                              renameTarget << "\": " << strerror(errno) << " (" << errno << ')');
                //TODO(schwerin): Make errnoWithDescription() available in the logger library, and
                //use it here.
            }
        }
        return _openFileStream(false);
    }

    Status RotatableFileWriter::Use::status() {
        if (!_writer->_stream) {
            return Status(ErrorCodes::FileNotOpen,
                          mongoutils::str::stream() << "File \"" << _writer->_fileName <<
                          "\" not open");
        }
        if (_writer->_stream->fail()) {
            return Status(ErrorCodes::FileStreamFailed,
                          mongoutils::str::stream() << "File \"" << _writer->_fileName <<
                          "\" in failed state");
        }
        return Status::OK();
    }

    Status RotatableFileWriter::Use::_openFileStream(bool append) {
        using std::swap;

#ifdef _WIN32
        boost::scoped_ptr<std::ostream> newStream(
                new Win32FileOStream(_writer->_fileName, append));
#else
        std::ios::openmode mode = std::ios::out;
        if (append) {
            mode |= std::ios::app;
        }
        else {
            mode |= std::ios::trunc;
        }
        boost::scoped_ptr<std::ostream> newStream(
                new std::ofstream(_writer->_fileName.c_str(), mode));
#endif

        if (newStream->fail()) {
            return Status(ErrorCodes::FileNotOpen, "Failed to open \"" + _writer->_fileName + "\"");
        }
        swap(_writer->_stream, newStream);
        return status();
    }

}  // namespace logger
}  // namespace mongo
