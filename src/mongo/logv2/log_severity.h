// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] logv2 {

/**
 * Representation of the severity / priority of a log message.
 *
 * Severities are totally ordered, from most severe to least severe as follows:
 * Severe, Error, Warning, Info, Log, Debug(1), Debug(2), ...
 */
class LogSeverity {
public:
    static constexpr int kMaxDebugLevel = 5;

    /**
     * Factory functions for getting LogSeverity objects of the various severity levels
     * @{
     */
    static constexpr LogSeverity Severe() noexcept {
        return LogSeverity(-4);
    }
    static constexpr LogSeverity Error() noexcept {
        return LogSeverity(-3);
    }
    static constexpr LogSeverity Warning() noexcept {
        return LogSeverity(-2);
    }
    static constexpr LogSeverity Info() noexcept {
        return LogSeverity(-1);
    }
    /** Log() === Debug(0) */
    static constexpr LogSeverity Log() noexcept {
        return LogSeverity(0);
    }

    /**
     * Logs like a default (debug-0) level log in production, but debug-1 log in testing. This log
     * level is for log lines that may be spammy in testing but are more rare in production. As
     * such, they may be useful in investigations. This level also preserves backwards compatibility
     * for logs that are no longer as useful as when they were introduced. It is preferred to use
     * other severities over this one as it introduces a difference between testing and production.
     *
     * To ensure logs with this severity are tested, if using this severity make sure that the log's
     * code path is run in a test where the verbosity for the log's component is set to at least 1.
     */
    static LogSeverity ProdOnly() {
        return getSuppressProdOnly() ? LogSeverity::Debug(1) : LogSeverity::Log();
    }
    /** @} */

    /**
     * Construct a LogSeverity to represent the given debug level.
     * Debug levels above kMaxDebugLevel will be clamped to kMaxDebugLevel.
     */
    static constexpr LogSeverity Debug(int debugLevel) noexcept {
        return LogSeverity(std::min(debugLevel, kMaxDebugLevel));
    }

    /**
     * Casts an integer to a severity.
     */
    static constexpr LogSeverity cast(int ll) noexcept {
        return LogSeverity(ll);
    }

    constexpr int toInt() const noexcept {
        return _severity;
    }

    /**
     * Returns a LogSeverity object that is one unit "more severe" than this one.
     */
    constexpr LogSeverity moreSevere() const noexcept {
        return LogSeverity(_severity - 1);
    }

    /**
     * Returns a LogSeverity object that is one unit "less severe" than this one.
     */
    constexpr LogSeverity lessSevere() const noexcept {
        return LogSeverity(_severity + 1);
    }

    /**
     * Returns a std::string_view naming this security level.
     *
     * Not all levels are uniquely named.  Debug(N) is named "debug", regardless of "N",
     * e.g.
     */
    std::string_view toStringData() const;

    /**
     * Returns two characters naming this severity level. For non-debug levels, returns
     * a single character mapping to the first letter of the string returned by
     * `toStringData`, followed by a space. For debug levels, returns 'DN', where N
     * is an integer greater than zero.
     *
     * All levels are uniquely named.
     */
    std::string_view toStringDataCompact() const;

    /**
     * Comparison operations.
     * @{
     */
    friend constexpr bool operator==(LogSeverity a, LogSeverity b) noexcept {
        return a._order() == b._order();
    }
    friend constexpr bool operator!=(LogSeverity a, LogSeverity b) noexcept {
        return a._order() != b._order();
    }
    friend constexpr bool operator<(LogSeverity a, LogSeverity b) noexcept {
        return a._order() < b._order();
    }
    friend constexpr bool operator>(LogSeverity a, LogSeverity b) noexcept {
        return a._order() > b._order();
    }
    friend constexpr bool operator<=(LogSeverity a, LogSeverity b) noexcept {
        return a._order() <= b._order();
    }
    friend constexpr bool operator>=(LogSeverity a, LogSeverity b) noexcept {
        return a._order() >= b._order();
    }
    /** @} */

    friend std::ostream& operator<<(std::ostream& os, LogSeverity severity) {
        return os << severity.toStringData();
    }

    /** True when `LogSeverity::ProdOnly()` should be quiet (i.e. during testing). */
    static bool getSuppressProdOnly() {
        return _suppressProdOnly;
    }

    /**
     * `LogSeverity::ProdOnly()` is verbose by default, but can be set to
     * quiet by setting this suppression to true. This is also set
     * as a side-effect of setting the server parameter
     * 'enableTestCommands'.  Not synchronized. Call in single threaded
     * mode only, i.e. startup or unit tests.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] static void suppressProdOnly_forTest(bool b) {
        _suppressProdOnly = b;
    }

private:
    explicit constexpr LogSeverity(int severity) noexcept : _severity{severity} {}

    /** Express the inverse sense of _severity for the comparison ops in one place. */
    constexpr int _order() noexcept {
        return -_severity;
    }

    /// The stored severity.  More negative is more severe.  NOTE: This means that the >, <, >=
    /// and <= operators on LogSeverity have opposite sense of the same operators on the
    /// underlying integer.  That is, given severities S1 and S2, S1 > S2 means that S1.toInt()
    /// < S2.toInt().
    ///
    /// TODO(schwerin): Decide if we should change this so more positive is more severe.  The
    /// logLevel parameter in the database is more compatible with this sense, but it's not
    /// totally intuitive.  One could also remove the operator overloads in favor of named
    /// methods, isNoMoreSevereThan, isLessSevereThan, isMoreSevereThan, isNoLessSevereThan,
    /// isSameSeverity and isDifferentSeverity.
    int _severity;

    /// Controls whether `LogSeverity::ProdOnly()` should be quiet.
    static inline bool _suppressProdOnly = false;
};

}  // namespace logv2
}  // namespace mongo
