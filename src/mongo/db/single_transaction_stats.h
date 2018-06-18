/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

namespace mongo {

/**
 * Tracks metrics for a single multi-document transaction.
 */
class SingleTransactionStats {
public:
    /**
     * Returns the start time of the transaction in microseconds.
     *
     * This method cannot be called until setStartTime() has been called.
     */
    unsigned long long getStartTime() const {
        invariant(_startTime > 0);

        return _startTime;
    }

    /**
     * Sets the transaction's start time, only if it hasn't already been set.
     *
     * This method must only be called once.
     */
    void setStartTime(unsigned long long time) {
        invariant(_startTime == 0);

        _startTime = time;
    }

    /**
     * If the transaction is currently in progress, this method returns the duration
     * the transaction has been running for in microseconds.
     *
     * For a completed transaction, this method returns the total duration of the
     * transaction in microseconds.
     *
     * This method cannot be called until setStartTime() has been called.
     */
    unsigned long long getDuration() const {
        invariant(_startTime > 0);

        // The transaction hasn't ended yet, so we return how long it has currently
        // been running for.
        if (_endTime == 0) {
            return curTimeMicros64() - _startTime;
        }
        return _endTime - _startTime;
    }

    /**
     * Sets the transaction's end time, only if the start time has already been set.
     *
     * This method cannot be called until setStartTime() has been called.
     */
    void setEndTime(unsigned long long time) {
        invariant(_startTime > 0);

        _endTime = time;
    }

private:
    // The start time of the transaction in microseconds.
    unsigned long long _startTime{0};

    // The end time of the transaction in microseconds.
    unsigned long long _endTime{0};
};

}  // namespace mongo
