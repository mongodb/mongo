/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/logv2/ramlog.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/util/map_util.h"
#include "mongo/util/str.h"

namespace mongo::logv2 {

using std::string;

namespace {
typedef std::map<string, RamLog*> RM;
stdx::mutex* _namedLock = NULL;  // NOLINT
RM* _named = NULL;

}  // namespace

RamLog::RamLog(StringData name) : _name(name) {
    clear();
}

RamLog::~RamLog() {}

void RamLog::write(const std::string& str) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _totalLinesWritten++;

    if (0 == str.size()) {
        return;
    }

    // Trim if we are going to go above the threshold
    trimIfNeeded(str.size(), lk);

    // Add the new line and adjust the space accounting
    _totalSizeBytes -= _lines[_lastLinePosition].size();
    _lines[_lastLinePosition] = str;
    _totalSizeBytes += str.size();

    // Advance the last line position to the next entry
    _lastLinePosition = (_lastLinePosition + 1) % kMaxLines;

    // If _lastLinePosition is == _firstLinePosition, it means we wrapped around so advance
    // firstLinePosition
    if (_lastLinePosition == _firstLinePosition) {
        _firstLinePosition = (_firstLinePosition + 1) % kMaxLines;
    }
}

void RamLog::trimIfNeeded(size_t newStr, WithLock lock) {
    // Check if we are going to go past the size limit
    if ((_totalSizeBytes + newStr) < kMaxSizeBytes) {
        return;
    }

    // Worst case, if the user adds a really large line, we will keep just one line
    if (getLineCount(lock) == 0) {
        return;
    }

    // The buffer has grown large, so trim back enough to fit our new string
    size_t trimmedSpace = 0;

    // Trim down until we make enough space, keep at least one line though
    // This means with the line we are about to have, the log will actually have 2 lines
    while (getLineCount(lock) > 1 && trimmedSpace < newStr) {
        size_t size = _lines[_firstLinePosition].size();
        trimmedSpace += size;
        _totalSizeBytes -= size;

        _lines[_firstLinePosition].clear();
        _lines[_firstLinePosition].shrink_to_fit();

        _firstLinePosition = (_firstLinePosition + 1) % kMaxLines;
    }
}

void RamLog::clear() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _totalLinesWritten = 0;
    _firstLinePosition = 0;
    _lastLinePosition = 0;
    _totalSizeBytes = 0;

    for (size_t i = 0; i < kMaxLines; i++) {
        _lines[i].clear();
        _lines[i].shrink_to_fit();
    }
}

StringData RamLog::getLine(size_t lineNumber, WithLock lock) const {
    if (lineNumber >= getLineCount(lock)) {
        return "";
    }

    return _lines[(lineNumber + _firstLinePosition) % kMaxLines].c_str();
}

size_t RamLog::getLineCount(WithLock) const {
    if (_lastLinePosition < _firstLinePosition) {
        return (kMaxLines - _firstLinePosition) + _lastLinePosition;
    }

    return _lastLinePosition - _firstLinePosition;
}

RamLog::LineIterator::LineIterator(RamLog* ramlog)
    : _ramlog(ramlog), _lock(ramlog->_mutex), _nextLineIndex(0) {}

size_t RamLog::LineIterator::getTotalLinesWritten() {
    return _ramlog->_totalLinesWritten;
}

// ---------------
// static things
// ---------------

RamLog* RamLog::get(const std::string& name) {
    if (!_namedLock) {
        // Guaranteed to happen before multi-threaded operation.
        _namedLock = new stdx::mutex();  // NOLINT
    }

    stdx::lock_guard<stdx::mutex> lk(*_namedLock);
    if (!_named) {
        // Guaranteed to happen before multi-threaded operation.
        _named = new RM();
    }

    RamLog* result = mapFindWithDefault(*_named, name, static_cast<RamLog*>(NULL));
    if (!result) {
        result = new RamLog(name);
        (*_named)[name] = result;
    }

    return result;
}

RamLog* RamLog::getIfExists(const std::string& name) {
    if (!_named) {
        return NULL;
    }
    stdx::lock_guard<stdx::mutex> lk(*_namedLock);
    return mapFindWithDefault(*_named, name, static_cast<RamLog*>(NULL));
}

void RamLog::getNames(std::vector<string>& names) {
    if (!_named) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lk(*_namedLock);
    for (RM::iterator i = _named->begin(); i != _named->end(); ++i) {
        if (i->second->getLineCount(lk)) {
            names.push_back(i->first);
        }
    }
}

/**
 * Ensures that RamLog::get() is called at least once during single-threaded operation,
 * ensuring that _namedLock and _named are initialized safely.
 */
MONGO_INITIALIZER(RamLogCatalogV2)(InitializerContext*) {
    if (!_namedLock) {
        if (_named) {
            return Status(ErrorCodes::InternalError,
                          "Inconsistent intiailization of RamLogCatalog.");
        }

        _namedLock = new stdx::mutex();  // NOLINT
        _named = new RM();
    }

    return Status::OK();
}

}  // namespace mongo::logv2
