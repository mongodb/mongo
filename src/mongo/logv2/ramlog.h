// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::logv2 {

/**
 * Variable-capacity circular log of line-oriented messages.
 *
 * Controlled by the ramLogMaxLines and ramLogMaxSizeBytes server parameters in logv2_options.idl.
 *
 * Holds up to (ramLogMaxLines - 1) lines and caps total space to ramLogMaxSizeBytes [1]. There is
 * no limit on the length of the line. RamLog expects the caller to truncate lines to a reasonable
 * length.
 *
 * RamLogs are stored in a global registry, accessed via RamLog::get() and
 * RamLog::getIfExists().
 *
 * RamLogs and their registry are self-synchronizing.  See documentary comments.
 * To read a RamLog, instantiate a RamLog::LineIterator, documented below.
 *
 * Note:
 * 1. In the degenerate case of a single log line being above ramLogMaxSizeBytes, it may
 *    keep up to two log lines and exceed the size cap.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] RamLog {
    RamLog(const RamLog&) = delete;
    RamLog& operator=(const RamLog&) = delete;
    ~RamLog() = delete;  // This type must never be destroyed

public:
    class LineIterator;
    friend class RamLog::LineIterator;

    /**
     * Returns a pointer to the RamLog named "name", creating one if it did not already exist.
     *
     * "name" is the RamLog's public identity (e.g., "global" is looked up by the getLog command),
     * For its ObservableMutex tag we derive a valid camelCase metric name segment from it by
     * capitalizing its first letter. "name" should otherwise be camelCase.
     *
     * Synchronizes on the RamLog global lock.
     */
    static RamLog* get(std::string_view name);

    /**
     * Returns a pointer to the RamLog named "name", or NULL if no such RamLog exists.
     *
     * Synchronizes on the RamLog global lock.
     */
    static RamLog* getIfExists(std::string_view name);

    /**
     * Returns the names of all existing RamLogs.
     *
     * Synchronizes on the RamLog global lock.
     */
    static std::vector<std::string> getNames();

    /**
     * Sets the global maximum number of lines for newly created RamLogs.
     */
    static void setGlobalMaxLines(size_t maxLines);

    /**
     * Gets the global maximum number of lines for newly created RamLogs.
     */
    static size_t getGlobalMaxLines();

    /**
     * Sets the global maximum size in bytes for newly created RamLogs.
     */
    static void setGlobalMaxSizeBytes(size_t maxSizeBytes);

    /**
     * Gets the global maximum size in bytes for newly created RamLogs.
     */
    static size_t getGlobalMaxSizeBytes();

    /**
     * Writes "str" as a line into the RamLog.  If "str" is longer than the maximum
     * line size of the log, it keeps two lines.
     */
    void write(const std::string& str);

    /**
     * Empties out the RamLog.
     */
    void clear();

    /**
     * Inspect maxLines setting.
     */
    size_t getMaxLines() const {
        return _maxLines;
    }

    /**
     * Inspect maxSizeBytes setting.
     */
    size_t getMaxSizeBytes() const {
        return _maxSizeBytes;
    }

private:
    explicit RamLog(std::string name, size_t maxLines, size_t maxSizeBytes);

    void trimIfNeeded(size_t spaceNeeded, WithLock lk);

    /**
     * Set maxLines and reallocate storage according to the new value.
     */
    void updateMaxLines(size_t newMaxLines);

    /**
     * Set maxSizeBytes and trim existing storage according to the new value.
     */
    void updateMaxSizeBytes(size_t newMaxLines);

    std::string_view getLine(size_t lineNumber, WithLock) const;

    size_t getLineCount(WithLock) const;

    // Maximum number of lines.
    size_t _maxLines;

    // Maximum capacity of RamLog of string data.
    size_t _maxSizeBytes;

    // Guards all non-static data.
    mutable ObservableMutex<std::mutex> _mutex;

    // Array of lines
    std::vector<std::string> _lines;

    // First line of ram log
    size_t _firstLinePosition{0};

    // Last line of ram log
    size_t _lastLinePosition{0};

    // Total size of bytes written
    size_t _totalSizeBytes{0};

    // Name of Ram Log
    std::string _name;

    // Total lines written since last clear, can be > _maxLines.
    size_t _totalLinesWritten{0};
};

/**
 * Iterator over the lines of a RamLog.
 *
 * Also acts as a means of inspecting other properties of a RamLog consistently.
 *
 * Instances of LineIterator hold the lock for the underlying RamLog for their whole lifetime;
 * trying to call RamLog APIs directly while the iterator is alive may deadlock.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] RamLog::LineIterator {
    LineIterator(const LineIterator&) = delete;
    LineIterator& operator=(const LineIterator&) = delete;

public:
    explicit LineIterator(RamLog* ramlog)
        : _ramlog(ramlog), _lock(ramlog->_mutex), _nextLineIndex(0) {}

    /**
     * Returns the number of lines remaining in this iterator.
     */
    size_t len() const {
        return _ramlog->getLineCount(_lock) - _nextLineIndex;
    }

    /**
     * Returns true if there are more lines available to return by calls to next().
     */
    bool more() const {
        return len() != 0;
    }

    /**
     * Returns the next line and advances the iterator.
     */
    std::string_view next() {
        return _ramlog->getLine(_nextLineIndex++, _lock);  // Postfix increment.
    }

    /**
     * Returns the total number of lines ever written to the RamLog.
     */
    size_t getTotalLinesWritten() {
        return _ramlog->_totalLinesWritten;
    }

private:
    const RamLog* _ramlog;

    // Holds RamLog's mutex
    std::lock_guard<ObservableMutex<std::mutex>> _lock;

    size_t _nextLineIndex;
};

}  // namespace mongo::logv2
