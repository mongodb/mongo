// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/ramlog.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"

#include <utility>

#include <fmt/format.h>

namespace mongo::logv2 {

namespace {

struct GlobalState {
    stdx::unordered_map<std::string, RamLog*> named;
    size_t maxLines{1024};
    size_t maxSizeBytes{1024 * 1024};
};

synchronized_value<GlobalState>& globalState() {
    static StaticImmortal<synchronized_value<GlobalState>> state;
    return *state;
}

}  // namespace

RamLog* RamLog::get(std::string_view name) {
    auto g = globalState().synchronize();
    auto [iter, isNew] = g->named.try_emplace(name);
    if (isNew) {
        iter->second = new RamLog(std::string{name}, g->maxLines, g->maxSizeBytes);
    }
    return iter->second;
}

RamLog* RamLog::getIfExists(std::string_view name) {
    auto g = globalState().synchronize();
    auto iter = g->named.find(name);
    return iter == g->named.end() ? nullptr : iter->second;
}

std::vector<std::string> RamLog::getNames() {
    std::vector<std::string> names;
    auto g = globalState().synchronize();
    names.reserve(g->named.size());  // could be more than needed but small either way
    for (const auto& [name, rmlog] : g->named) {
        std::lock_guard lk{rmlog->_mutex};
        if (rmlog->getLineCount(lk) > 0) {
            names.push_back(name);
        }
    }

    return names;
}

void RamLog::setGlobalMaxLines(size_t maxLines) {
    uassert(ErrorCodes::BadValue, "ramLogMaxLines must be greater than 0", maxLines > 0);
    auto g = globalState().synchronize();
    g->maxLines = maxLines;
    for (auto& [_name, ramlog] : g->named) {
        ramlog->updateMaxLines(maxLines);
    }
}

void RamLog::setGlobalMaxSizeBytes(size_t maxSizeBytes) {
    auto g = globalState().synchronize();
    g->maxSizeBytes = maxSizeBytes;
    for (auto& [_name, ramlog] : g->named) {
        ramlog->updateMaxSizeBytes(maxSizeBytes);
    }
}

size_t RamLog::getGlobalMaxLines() {
    return globalState()->maxLines;
}

size_t RamLog::getGlobalMaxSizeBytes() {
    return globalState()->maxSizeBytes;
}

// In another world, this would take a std::string by value rather than const reference, since
// it's taking ownership of the string and can move it into its internal buffer.
// However, the API that calls this function in ramlog_sink.h itself takes a const std::string&,
// and is meant to share a signature with other sinks, so it shouldn't be changed.
// Therefore, taking the string by const reference and maybe being able to do a memcpy into an
// existing string buffer internally is the best solution.
void RamLog::write(const std::string& str) {
    const auto len = str.size();
    std::lock_guard lk(_mutex);
    _totalLinesWritten++;

    if (len == 0) {
        return;
    }

    // Trim if we are going to go above the threshold
    trimIfNeeded(len, lk);

    // Add the new line and adjust the space accounting
    _totalSizeBytes -= _lines[_lastLinePosition].size();
    _lines[_lastLinePosition] = str;
    _totalSizeBytes += len;

    // Advance the last line position to the next entry
    _lastLinePosition = (_lastLinePosition + 1) % _maxLines;

    // If _lastLinePosition is == _firstLinePosition, it means we wrapped around so advance
    // firstLinePosition
    if (_lastLinePosition == _firstLinePosition) {
        _firstLinePosition = (_firstLinePosition + 1) % _maxLines;
    }
}

void RamLog::clear() {
    std::lock_guard lk(_mutex);
    _totalLinesWritten = 0;
    _firstLinePosition = 0;
    _lastLinePosition = 0;
    _totalSizeBytes = 0;

    for (auto& line : _lines) {
        line.clear();
        line.shrink_to_fit();
    }
}

RamLog::RamLog(std::string name, size_t maxLines, size_t maxSizeBytes)
    : _maxLines(maxLines), _maxSizeBytes(maxSizeBytes), _lines(maxLines), _name(std::move(name)) {
    std::string tagName{_name};
    if (!tagName.empty()) {
        tagName.front() = ctype::toUpper(tagName.front());
    }
    ObservableMutexRegistry::get().add(fmt::format("logv2RamLog{}Mutex", tagName), _mutex);
}

void RamLog::trimIfNeeded(size_t spaceNeeded, WithLock lk) {
    size_t totalSpaceNeeded = _totalSizeBytes + spaceNeeded;
    // Check if we are going to go past the size limit
    if (totalSpaceNeeded < _maxSizeBytes) {
        return;
    }

    // Worst case, if the user adds a really large line, we will keep just one line
    if (getLineCount(lk) == 0) {
        return;
    }

    // Trim down until we make enough space, keep at least one line though
    // This means with the line we are about to have, the log will actually have 2 lines
    while (getLineCount(lk) > 1 && totalSpaceNeeded >= _maxSizeBytes) {
        size_t size = _lines[_firstLinePosition].size();
        totalSpaceNeeded -= size;
        _totalSizeBytes -= size;

        _lines[_firstLinePosition].clear();
        _lines[_firstLinePosition].shrink_to_fit();

        _firstLinePosition = (_firstLinePosition + 1) % _maxLines;
    }
}

void RamLog::updateMaxLines(size_t newMaxLines) {
    std::lock_guard lk(_mutex);
    if (newMaxLines == _maxLines) {
        return;
    }

    // migrate the lines from newest to oldest
    std::vector<std::string> newLines{newMaxLines};
    auto currentSrc = _lastLinePosition;
    auto currentDst = newMaxLines;

    while (currentSrc != _firstLinePosition && currentDst > 1) {
        if (currentSrc == 0) {
            currentSrc = _maxLines;
        }

        newLines[--currentDst] = std::move(_lines[--currentSrc]);
    }

    _lines = std::move(newLines);
    _maxLines = newMaxLines;

    if (currentDst == newMaxLines) {
        _firstLinePosition = 0;
        _lastLinePosition = 0;
    } else {
        _firstLinePosition = currentDst;
        _lastLinePosition = 0;
    }
}

void RamLog::updateMaxSizeBytes(size_t newMaxSizeBytes) {
    std::lock_guard lk{_mutex};
    _maxSizeBytes = newMaxSizeBytes;
    if (_totalSizeBytes > newMaxSizeBytes) {
        trimIfNeeded(0, lk);
    }
}

std::string_view RamLog::getLine(size_t lineNumber, WithLock lk) const {
    if (lineNumber >= getLineCount(lk)) {
        return "";
    }

    return _lines[(lineNumber + _firstLinePosition) % _maxLines];
}

size_t RamLog::getLineCount(WithLock) const {
    if (_lastLinePosition < _firstLinePosition) {
        return (_maxLines - _firstLinePosition) + _lastLinePosition;
    }

    return _lastLinePosition - _firstLinePosition;
}

}  // namespace mongo::logv2
