/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

// IWYU pragma: private, include "mongo/unittest/unittest.h"
// IWYU pragma: friend "mongo/unittest/.*"

#include "mongo/bson/bsonobj.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mongo::unittest {

/**
 * Manages capturing of log messages by code under test.
 */
class LogCaptureGuard {
public:
    /** The object is automatically started. */
    LogCaptureGuard() : LogCaptureGuard{true} {}

    /** Selects whether it will be automatically started. */
    explicit LogCaptureGuard(bool willStart);

    /** The object is automatically stopped. */
    ~LogCaptureGuard();

    /**
     * Starts capturing log messages, saving a copy in this object.
     *
     * Log messages will still also go to their default destination; this
     * code simply adds an additional sink for log messages.
     *
     * Clears any previously captured log lines.
     *
     * Idempotent. If already started, this call does nothing, and previously captured
     * log lines are not cleared.
     */
    void start();

    /**
     * Stops capturing log messages.
     * Idempotent. Has no effect if already stopped.
     */
    void stop();

    /**
     * Gets a vector of strings, one log line per string, captured since
     * the last call to `start()`.
     */
    std::vector<std::string> getText() const;

    /**
     * Retrieve the BSON representation of the saved messages.
     * Log messages are saved as both text and BSON.
     */
    std::vector<BSONObj> getBSON() const;

    /**
     * Returns the number of collected log lines containing "needle".
     */
    size_t countTextContaining(const std::string& needle) const;

    /**
     * Returns the number of collected log lines where `needle` is a "subset" of that line.
     * For `needle` to be a "subset" in this sense, every element in `needle` must be a top-level
     * element of the line. Any `Obj` nodes in the `needle` element must exist in the corresponding
     * nodes of the line, and again the line's subtree `Obj` node may contain other elements.
     *
     * As a special case, when a BSON element in `needle` is `undefined`, we simply match on its
     * existence in the log lines, not its contained type or value.
     *
     * Example:
     *   LOGV2(12345678, "Test", "i"_attr = 1);
     *   // Log line has an `id` field of 12345678.
     *   ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 12345678)), 1);
     *   // Log line has a `msg` field of "Test"
     *   ASSERT_EQ(logs.countBSONContainingSubset(BSON("msg" << "Test")), 1);
     *   // Miss: not a recursive search, so cannot find `attr.i` by just using `i`.
     *   ASSERT_EQ(logs.countBSONContainingSubset(BSON("i" << 1)), 0);
     *   // Searching within the `attr` node will match.
     *   ASSERT_EQ(logs.countBSONContainingSubset(BSON("attr" << BSON("i" << 1))), 1);
     *   // `attr.c` doesn't exist, so this needle is not a subset.
     *   ASSERT_EQ(logs.countBSONContainingSubset(BSON("attr" << BSON("c" << 1))), 0);
     *   // Checks that the log line has an `attr.i`, without regard to its value.
     *   ASSERT_EQ(logs.countBSONContainingSubset(BSON("attr" << BSON("i" << BSONUndefined))), 1);
     */
    size_t countBSONContainingSubset(const BSONObj& needle) const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace mongo::unittest
