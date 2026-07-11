// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// IWYU pragma: private, include "mongo/unittest/unittest.h"
// IWYU pragma: friend "mongo/unittest/.*"

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mongo::unittest {

/**
 * Manages capturing of log messages by code under test.
 * Only one of these can be active at a time, as the logs
 * of all such guards are captured into a single static object.
 */
class [[MONGO_MOD_PUBLIC]] LogCaptureGuard {
public:
    /** The object is automatically started. */
    LogCaptureGuard() : LogCaptureGuard{true} {}

    /** Selects whether it will be automatically started. */
    explicit LogCaptureGuard(bool willStart);

    /** The object is automatically stopped. */
    ~LogCaptureGuard();

    /**
     * Starts capturing log messages.
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
