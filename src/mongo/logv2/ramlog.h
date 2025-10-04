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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"

#include <array>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace mongo::logv2 {

/**
 * Variable-capacity circular log of line-oriented messages.
 *
 * Holds up to RamLog::kMaxLines lines and caps total space to RamLog::kMaxSizeBytes [1]. There is
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
 * 1. In the degenerate case of a single log line being above RamLog::kMaxSizeBytes, it may
 *    keep up to two log lines and exceed the size cap.
 */
class RamLog {
    RamLog(const RamLog&) = delete;
    RamLog& operator=(const RamLog&) = delete;

public:
    class LineIterator;
    friend class RamLog::LineIterator;

    /**
     * Returns a pointer to the ramlog named "name", creating one if it did not already exist.
     *
     * Synchronizes on the RamLog catalog lock, _namedLock.
     */
    static RamLog* get(const std::string& name);

    /**
     * Returns a pointer to the ramlog named "name", creating one if it did not already exist.
     * Allows setting custom maxLines and maxSizeBytes.
     *
     * Synchronizes on the RamLog catalog lock, _namedLock.
     */
    static RamLog* get(const std::string& name, size_t maxLines, size_t maxSizeBytes);

    /**
     * Returns a pointer to the ramlog named "name", or NULL if no such ramlog exists.
     *
     * Synchronizes on the RamLog catalog lock, _namedLock.
     */
    static RamLog* getIfExists(const std::string& name);

    /**
     * Writes the names of all existing ramlogs into "names".
     *
     * Synchronizes on the RamLog catalog lock, _namedLock.
     */
    static void getNames(std::vector<std::string>& names);

    /**
     * Writes "str" as a line into the RamLog.  If "str" is longer than the maximum
     * line size of the log, it keeps two lines.
     *
     * Synchronized on the instance's own mutex, _mutex.
     */
    void write(const std::string& str);

    /**
     * Empties out the RamLog.
     */
    void clear();

    /**
     * Inspect maxLines setting
     */
    size_t getMaxLines() const {
        return _maxLines;
    }

    /**
     * Inspect maxSizeBytes setting
     */
    size_t getMaxSizeBytes() const {
        return _maxSizeBytes;
    }

private:
    explicit RamLog(StringData name, size_t maxLines, size_t maxSizeBytes);
    ~RamLog();  // want this private as we want to leak so we can use them till the very end

    StringData getLine(size_t lineNumber) const;

    size_t getLineCount() const;

    void trimIfNeeded(size_t newStr);

    static RamLog* getImpl(const std::string& name,
                           size_t maxLines = kMaxLines,
                           size_t maxSizeBytes = kMaxSizeBytes);

private:
    // Default maximum number of lines
    static constexpr size_t kMaxLines = 1024;

    // Default maximum capacity of RamLog of string data
    static constexpr size_t kMaxSizeBytes = 1024 * 1024;

    // Maximum number of lines
    size_t _maxLines;

    // Maximum capacity of RamLog of string data
    size_t _maxSizeBytes;

    // Guards all non-static data.
    // stdx::recursive_mutex // NOLINT is intentional, stdx::mutex can not be used here
    mutable stdx::recursive_mutex _mutex;  // NOLINT

    // Array of lines
    std::vector<std::string> _lines;

    // First line of ram log
    size_t _firstLinePosition;

    // Last line of ram log
    size_t _lastLinePosition;

    // Total size of bytes written
    size_t _totalSizeBytes;

    // Name of Ram Log
    std::string _name;

    // Total lines written since last clear, can be > kMaxLines
    size_t _totalLinesWritten;
};

/**
 * Iterator over the lines of a RamLog.
 *
 * Also acts as a means of inspecting other properites of a ramlog consistently.
 *
 * Instances of LineIterator hold the lock for the underlying RamLog for their whole lifetime,
 * and so should not be kept around.
 */
class RamLog::LineIterator {
    LineIterator(const LineIterator&) = delete;
    LineIterator& operator=(const LineIterator&) = delete;

public:
    explicit LineIterator(RamLog* ramlog);

    /**
     * Returns true if there are more lines available to return by calls to next().
     */
    bool more() const {
        return _nextLineIndex < _ramlog->getLineCount();
    }

    /**
     * Returns the next line and advances the iterator.
     */
    StringData next() {
        return _ramlog->getLine(_nextLineIndex++);  // Postfix increment.
    }

    /**
     * Returns the total number of lines ever written to the ramlog.
     */
    size_t getTotalLinesWritten();

private:
    const RamLog* _ramlog;

    // Holds RamLog's mutex
    stdx::lock_guard<stdx::recursive_mutex> _lock;

    size_t _nextLineIndex;
};

}  // namespace mongo::logv2
