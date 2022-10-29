/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#ifdef _WIN32
#include "named_pipe.h"

#include <fmt/format.h>
#include <string>
#include <system_error>

#include "mongo/db/storage/io_error_message.h"
#include "mongo/logv2/log.h"
#include "mongo/util/errno_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
using namespace fmt::literals;

NamedPipeOutput::NamedPipeOutput(const std::string& pipeRelativePath)
    : _pipeAbsolutePath(kDefaultPipePath + pipeRelativePath),
      _pipe(CreateNamedPipeA(_pipeAbsolutePath.c_str(),
                             PIPE_ACCESS_OUTBOUND,
                             (PIPE_TYPE_BYTE | PIPE_WAIT),
                             1,          // nMaxInstances
                             0,          // nOutBufferSize
                             0,          // nInBufferSize
                             0,          // nDefaultTimeOut
                             nullptr)),  // lpSecurityAttributes
      _isOpen(false) {
    uassert(7005006,
            "Failed to create a named pipe, error={}"_format(
                getErrorMessage("CreateNamedPipe", _pipeAbsolutePath)),
            _pipe != INVALID_HANDLE_VALUE);
}

NamedPipeOutput::~NamedPipeOutput() {
    close();
}

void NamedPipeOutput::open() {
    if (_isOpen) {
        return;
    }
    auto res = ConnectNamedPipe(_pipe, nullptr);
    if (!res) {
        LOGV2_ERROR(7005007,
                    "Failed to connect a named pipe",
                    "error"_attr = getErrorMessage("ConnectNamedPipe", _pipeAbsolutePath));
        return;
    }
    _isOpen = true;
}

int NamedPipeOutput::write(const char* data, int size) {
    uassert(7005012, "Output must have been opened before writing", _isOpen);
    DWORD nWritten = 0;
    // Write the reply to the pipe.
    auto res = WriteFile(_pipe,      // handle to pipe
                         data,       // buffer to write from
                         size,       // number of bytes to write
                         &nWritten,  // number of bytes written
                         nullptr);   // not overlapped I/O

    if (!res || size != nWritten) {
        LOGV2_ERROR(7005008,
                    "Failed to write to a named pipe",
                    "error"_attr = getErrorMessage("write", _pipeAbsolutePath));
        return -1;
    }

    return static_cast<int>(nWritten);
}

void NamedPipeOutput::close() {
    if (_isOpen) {
        // Flush the pipe to allow the client to read the pipe's contents
        // before disconnecting. Then disconnect the pipe, and close the
        // handle to this pipe instance.
        FlushFileBuffers(_pipe);
        DisconnectNamedPipe(_pipe);
        CloseHandle(_pipe);
        _isOpen = false;
    }
}

NamedPipeInput::NamedPipeInput(const std::string& pipeRelativePath)
    : _pipeAbsolutePath(kDefaultPipePath + pipeRelativePath),
      _pipe(INVALID_HANDLE_VALUE),
      _isOpen(false),
      _isGood(false),
      _isEof(false) {}

NamedPipeInput::~NamedPipeInput() {
    close();
}

void NamedPipeInput::doOpen() {
    _pipe =
        CreateFileA(_pipeAbsolutePath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (_pipe == INVALID_HANDLE_VALUE) {
        return;
    }

    _isOpen = true;
    _isGood = true;
}

int NamedPipeInput::doRead(char* data, int size) {
    DWORD nRead = 0;
    auto res = ReadFile(_pipe, data, size, &nRead, nullptr);
    if (!res) {
        _isGood = false;
        // The pipe writer has already gone and we treat this as EOF and any data that has been read
        // must be returned as is.
        if (auto ec = lastSystemError().value(); ec == ERROR_PIPE_NOT_CONNECTED) {
            _isEof = true;
        }
    } else if (nRead == 0) {
        _isGood = false;
        _isEof = true;
    }
    return static_cast<int>(nRead);
}

void NamedPipeInput::doClose() {
    CloseHandle(_pipe);
    _isOpen = false;
    _isGood = false;
    _isEof = false;
}

bool NamedPipeInput::isOpen() const {
    return _isOpen;
}

bool NamedPipeInput::isGood() const {
    return _isGood;
}

bool NamedPipeInput::isFailed() const {
    return !_isGood;
}

bool NamedPipeInput::isEof() const {
    return _isEof;
}
}  // namespace mongo
#endif
