/*    Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/base/string_data.h"

namespace mongo {
namespace logger {

/**
 * Representation of the severity / priority of a log message.
 *
 * Severities are totally ordered, from most severe to least severe as follows:
 * Severe, Error, Warning, Info, Log, Debug(1), Debug(2), ...
 */
class LogSeverity {
public:
    //
    // Static factory methods for getting LogSeverity objects of the various severity levels.
    //

    static inline LogSeverity Severe();
    static inline LogSeverity Error();
    static inline LogSeverity Warning();
    static inline LogSeverity Info();
    static inline LogSeverity Log();  // === Debug(0)
    static inline LogSeverity Debug(int debugLevel);

    /**
     * Casts an integer to a severity.
     *
     * Do not use this.  It exists to enable a handful of leftover uses of LOG(0) and the
     * deprecated LabeledLevel.
     */
    static inline LogSeverity cast(int);

    inline int toInt() const;

    /**
     * Returns a LogSeverity object that is one unit "more severe" than this one.
     */
    inline LogSeverity moreSevere() const;

    /**
     * Returns a LogSeverity object that is one unit "less severe" than this one.
     */
    inline LogSeverity lessSevere() const;

    /**
     * Returns a std::string naming this severity level.
     *
     * See toStringData(), below.
     */
    inline std::string toString() const;

    /**
     * Returns a StringData naming this security level.
     *
     * Not all levels are uniquely named.  Debug(N) is named "debug", regardless of "N",
     * e.g.
     */
    StringData toStringData() const;

    /**
     * Returns a single capital letter naming this severity level.
     * Equivalent to upper-case of first letter of toStringData() result.
     *
     * Not all levels are uniquely named.
     */
    char toChar() const;

    //
    // Comparison operations.
    //

    /// Returns true if this is exactly as severe as other.
    inline bool operator==(const LogSeverity other) const;

    /// Returns true if this is not exactly as severe as other.
    inline bool operator!=(const LogSeverity other) const;

    /// Returns true if this is less severe than other.
    inline bool operator<(const LogSeverity other) const;

    /// Returns true if this is no more severe than other.
    inline bool operator<=(const LogSeverity other) const;

    /// Returns true if this is more severe than other.
    inline bool operator>(const LogSeverity other) const;

    /// Returns true if this is no less severe than other.
    inline bool operator>=(const LogSeverity other) const;

private:
    explicit LogSeverity(int severity) : _severity(severity) {}

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

std::ostream& operator<<(std::ostream& os, LogSeverity severity);

}  // namespace logger
}  // namespace mongo

#include "mongo/logger/log_severity-inl.h"
