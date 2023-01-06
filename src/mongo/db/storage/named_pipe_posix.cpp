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

#ifndef _WIN32

#include "mongo/db/storage/named_pipe.h"

#include <cerrno>
#include <cstdio>
#include <fmt/format.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/storage/io_error_message.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"

namespace mongo {
using namespace fmt::literals;

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace {
// 'rc' must be the return code from POSIX-like OS I/O APIs.
inline bool hasSucceeded(int rc) {
    // POSIX-like I/O APIs return 0 for the successful operation.
    return rc == 0;
}

// Removes the named pipe and logs an error message when
// - either 'ignoreNoEntError' == true and there's an error other than the ENOENT error
// - or 'ignoreNoEntError' == false and there's any error
void removeNamedPipe(bool ignoreNoEntError, const char* pipeAbsolutePath) {
    if (!hasSucceeded(remove(pipeAbsolutePath))) {
        if (ignoreNoEntError && errno == ENOENT) {
            return;
        }

        LOGV2_ERROR(7097000,
                    "Failed to remove",
                    "error"_attr = getErrorMessage("remove", pipeAbsolutePath));
    }
}
}  // namespace

NamedPipeOutput::NamedPipeOutput(const std::string& pipeDir, const std::string& pipeRelativePath)
    : _pipeAbsolutePath(pipeDir + pipeRelativePath), _ofs() {
    // Just in case that uncleaned-up named pipe is still there. This is a test-only implementation
    // and so, it should be fine to just remove it and ignore the ENOENT error.
    removeNamedPipe(true /*ignoreNoEntError*/, _pipeAbsolutePath.c_str());
    uassert(7005005,
            "Failed to create a named pipe, error: {}"_format(
                getErrorMessage("mkfifo", _pipeAbsolutePath)),
            hasSucceeded(mkfifo(_pipeAbsolutePath.c_str(), 0664)));
}

NamedPipeOutput::~NamedPipeOutput() {
    close();
    // Makes sure that the named pipe is removed.
    removeNamedPipe(false /*ignoreNoEntError*/, _pipeAbsolutePath.c_str());
}

void NamedPipeOutput::open() {
    _ofs.open(_pipeAbsolutePath.c_str(), std::ios::binary | std::ios::app);
    if (!_ofs.is_open() || !_ofs.good()) {
        LOGV2_ERROR(7005009,
                    "Failed to open a named pipe",
                    "error"_attr = getErrorMessage("open", _pipeAbsolutePath));
    }
}

int NamedPipeOutput::write(const char* data, int size) {
    uassert(7005011, "Output must have been opened before writing", _ofs.is_open());
    _ofs.write(data, size);
    if (_ofs.fail()) {
        uasserted(7239300,
                  "Failed to write to a named pipe, error: {}"_format(
                      getErrorMessage("write", _pipeAbsolutePath)));
        return -1;
    }
    return size;
}

void NamedPipeOutput::close() {
    if (_ofs.is_open()) {
        _ofs.close();
    }
}

#undef MONGO_LOGV2_DEFAULT_COMPONENT
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

NamedPipeInput::NamedPipeInput(const std::string& pipeRelativePath)
    : _pipeAbsolutePath((externalPipeDir == "" ? kDefaultPipePath : externalPipeDir) +
                        pipeRelativePath),
      _ifs() {
    uassert(7001100,
            "Pipe path must not include '..' but {} does"_format(_pipeAbsolutePath),
            _pipeAbsolutePath.find("..") == std::string::npos);
}

NamedPipeInput::~NamedPipeInput() {
    close();
}

void NamedPipeInput::doOpen() {
    // MultiBsonStreamCursor's (MBSC) assembly buffer is designed to perform well without a lower-
    // layer IO buffer. Removing std::ifstream's default 8k "associated buffer" improves throughput
    // by 1.9% by eliminating the hidden copies from that buffer to MBSC's buffer. MBSC itself will
    // never copy data except when it (rarely) needs to expand its buffer, so by removing
    // std::ifstream's buffer we get an essentially zero-copy cursor that still avoids lots of tiny
    // IOs due to MBSC's assembly buffer algorithm.
    _ifs.rdbuf()->pubsetbuf(0, 0);

    // Retry the open every {1, 2, 4, 8, 16} ms for 1,000 reps each (allowing up to 31 seconds of
    // retry) in case the pipe writer has not finished creating the pipe yet.
    int retries = 0;
    int sleepMs = 1;
    bool opened;
    do {
        _ifs.open(_pipeAbsolutePath.c_str(), std::ios::binary | std::ios::in);
        opened = _ifs.is_open();
        if (!opened) {
            uassert(ErrorCodes::FileNotOpen,
                    "error = {}"_format(getErrorMessage("open", _pipeAbsolutePath)),
                    errno == ENOENT);
            stdx::this_thread::sleep_for(stdx::chrono::milliseconds(sleepMs));
            ++retries;
            if (retries % 1000 == 0) {
                sleepMs *= 2;
            }
        }
    } while (!opened && retries <= 5000);
    if (retries > 1000) {
        LOGV2_WARNING(7184900,
                      "NamedPipeInput::doOpen() waited for pipe longer than 1 sec",
                      "_pipeAbsolutePath"_attr = _pipeAbsolutePath);
    }
}

int NamedPipeInput::doRead(char* data, int size) {
    _ifs.read(data, size);
    return _ifs.gcount();
}

void NamedPipeInput::doClose() {
    _ifs.close();
}

bool NamedPipeInput::isOpen() const {
    return _ifs.is_open();
}

bool NamedPipeInput::isGood() const {
    return _ifs.good();
}

bool NamedPipeInput::isFailed() const {
    return _ifs.fail();
}

bool NamedPipeInput::isEof() const {
    return _ifs.eof();
}
}  // namespace mongo
#endif
