// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/ramlog.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/observable_mutex_registry.h"

#include <map>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace mongo::logv2 {

using std::string;

namespace {
typedef std::map<string, RamLog*> RM;
std::mutex* _namedLock = NULL;
RM* _named = NULL;
// TODO(SERVER-113226): Move these globals into a single structure to manage them better.
Atomic<size_t> _globalMaxLines = 1024;
Atomic<size_t> _globalMaxSizeBytes = 1024 * 1024;

}  // namespace

RamLog::RamLog(std::string_view name, size_t maxLines, size_t maxSizeBytes)
    : _maxLines(maxLines), _maxSizeBytes(maxSizeBytes), _name(name) {
    std::string tagName{name};
    if (!tagName.empty()) {
        tagName.front() = ctype::toUpper(tagName.front());
    }
    ObservableMutexRegistry::get().add(fmt::format("logv2RamLog{}Mutex", tagName), _mutex);
    _lines.resize(_maxLines);
    clear();
}

RamLog::~RamLog() {}

void RamLog::write(const std::string& str) {
    std::lock_guard lk(_mutex);
    _totalLinesWritten++;

    if (0 == str.size()) {
        return;
    }

    // Trim if we are going to go above the threshold
    trimIfNeeded(str.size());

    // Add the new line and adjust the space accounting
    _totalSizeBytes -= _lines[_lastLinePosition].size();
    _lines[_lastLinePosition] = str;
    _totalSizeBytes += str.size();

    // Advance the last line position to the next entry
    _lastLinePosition = (_lastLinePosition + 1) % _maxLines;

    // If _lastLinePosition is == _firstLinePosition, it means we wrapped around so advance
    // firstLinePosition
    if (_lastLinePosition == _firstLinePosition) {
        _firstLinePosition = (_firstLinePosition + 1) % _maxLines;
    }
}

// warning: this function must be invoked under existing mutex
void RamLog::trimIfNeeded(size_t newStr) {
    // Check if we are going to go past the size limit
    if ((_totalSizeBytes + newStr) < _maxSizeBytes) {
        return;
    }

    // Worst case, if the user adds a really large line, we will keep just one line
    if (getLineCount() == 0) {
        return;
    }

    // The buffer has grown large, so trim back enough to fit our new string
    size_t trimmedSpace = 0;

    // Trim down until we make enough space, keep at least one line though
    // This means with the line we are about to have, the log will actually have 2 lines
    while (getLineCount() > 1 && trimmedSpace < newStr) {
        size_t size = _lines[_firstLinePosition].size();
        trimmedSpace += size;
        _totalSizeBytes -= size;

        _lines[_firstLinePosition].clear();
        _lines[_firstLinePosition].shrink_to_fit();

        _firstLinePosition = (_firstLinePosition + 1) % _maxLines;
    }
}

void RamLog::clear() {
    std::lock_guard lk(_mutex);
    _totalLinesWritten = 0;
    _firstLinePosition = 0;
    _lastLinePosition = 0;
    _totalSizeBytes = 0;

    for (size_t i = 0; i < _maxLines; i++) {
        _lines[i].clear();
        _lines[i].shrink_to_fit();
    }
}

std::string_view RamLog::getLine(size_t lineNumber) const {
    if (lineNumber >= getLineCount()) {
        return "";
    }

    std::lock_guard lk(_mutex);
    return _lines[(lineNumber + _firstLinePosition) % _maxLines].c_str();
}

size_t RamLog::getLineCount() const {
    std::lock_guard lk(_mutex);

    if (_lastLinePosition < _firstLinePosition) {
        return (_maxLines - _firstLinePosition) + _lastLinePosition;
    }

    return _lastLinePosition - _firstLinePosition;
}

RamLog::LineIterator::LineIterator(RamLog* ramlog)
    : _ramlog(ramlog), _lock(ramlog->_mutex), _nextLineIndex(0) {}

size_t RamLog::LineIterator::getTotalLinesWritten() {
    std::lock_guard lk(_ramlog->_mutex);

    return _ramlog->_totalLinesWritten;
}

// ---------------
// static things
// ---------------

RamLog* RamLog::get(const std::string& name) {
    return getImpl(name);
}

RamLog* RamLog::get(const std::string& name, size_t maxLines, size_t maxSizeBytes) {
    return getImpl(name, maxLines, maxSizeBytes);
}

RamLog* RamLog::getImpl(const std::string& name, size_t maxLines, size_t maxSizeBytes) {
    if (!_namedLock) {
        // Guaranteed to happen before multi-threaded operation.
        _namedLock = new std::mutex();
    }

    std::lock_guard<std::mutex> lk(*_namedLock);
    if (!_named) {
        // Guaranteed to happen before multi-threaded operation.
        _named = new RM();
    }

    auto [iter, isNew] = _named->try_emplace(name);
    if (isNew)
        iter->second = new RamLog(name, maxLines, maxSizeBytes);
    return iter->second;
}

RamLog* RamLog::getIfExists(const std::string& name) {
    if (!_named) {
        return NULL;
    }
    std::lock_guard<std::mutex> lk(*_namedLock);
    auto iter = _named->find(name);
    return iter == _named->end() ? nullptr : iter->second;
}

void RamLog::getNames(std::vector<string>& names) {
    if (!_named) {
        return;
    }

    std::lock_guard<std::mutex> lk(*_namedLock);
    for (RM::iterator i = _named->begin(); i != _named->end(); ++i) {
        if (i->second->getLineCount()) {
            names.push_back(i->first);
        }
    }
}

void RamLog::setGlobalMaxLines(size_t maxLines) {
    _globalMaxLines.store(maxLines);
}

void RamLog::setGlobalMaxSizeBytes(size_t maxSizeBytes) {
    _globalMaxSizeBytes.store(maxSizeBytes);
}

size_t RamLog::getGlobalMaxLines() {
    return _globalMaxLines.load();
}

size_t RamLog::getGlobalMaxSizeBytes() {
    return _globalMaxSizeBytes.load();
}

/**
 * Ensures that RamLog::get() is called at least once during single-threaded operation,
 * ensuring that _namedLock and _named are initialized safely.
 */
MONGO_INITIALIZER(RamLogCatalogV2)(InitializerContext*) {
    if (!_namedLock) {
        if (_named) {
            uasserted(ErrorCodes::InternalError, "Inconsistent intiailization of RamLogCatalog.");
        }

        _namedLock = new std::mutex();
        _named = new RM();
    }
}

}  // namespace mongo::logv2
