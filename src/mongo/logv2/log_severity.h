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

#include <algorithm>
#include <iostream>
#include <string>

#include "mongo/base/string_data.h"

namespace mongo::logv2 {

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
     * Returns a std::string naming this severity level.
     *
     * See toStringData(), below.
     */
    std::string toString() const;

    /**
     * Returns a StringData naming this security level.
     *
     * Not all levels are uniquely named.  Debug(N) is named "debug", regardless of "N",
     * e.g.
     */
    StringData toStringData() const;

    /**
     * Returns two characters naming this severity level. For non-debug levels, returns
     * a single character mapping to the first letter of the string returned by
     * `toStringData`, followed by a space. For debug levels, returns 'DN', where N
     * is an integer greater than zero.
     *
     * All levels are uniquely named.
     */
    StringData toStringDataCompact() const;

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
};

}  // namespace mongo::logv2
