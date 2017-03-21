/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/db/signed_logical_time.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
class TimeProofService;
class ServiceContext;
class OperationContext;

/**
 * LogicalClock maintain the clusterTime for a clusterNode. Every cluster node in a replica set has
 * an instance of the LogicalClock installed as a ServiceContext decoration. LogicalClock owns the
 * TimeProofService that allows it to generate proofs to sign LogicalTime values and to validate the
 * proofs of SignedLogicalTime values.LogicalClock instance must be created before the instance
 * starts up.
 */
class LogicalClock {
public:
    // Decorate ServiceContext with LogicalClock instance.
    static LogicalClock* get(ServiceContext* service);
    static LogicalClock* get(OperationContext* ctx);
    static void set(ServiceContext* service, std::unique_ptr<LogicalClock> logicalClock);

    static constexpr Seconds kMaxAcceptableLogicalClockDrift =
        Seconds(365 * 24 * 60 * 60);  // 1 year

    /**
     *  Creates an instance of LogicalClock. The TimeProofService must already be fully initialized.
     */
    LogicalClock(ServiceContext*, std::unique_ptr<TimeProofService>);

    /**
     * The method sets clusterTime to the newTime if the newTime > _clusterTime and the newTime
     * passes the rate check and proof validation.
     * Returns an error if the newTime does not pass the rate check or proof validation,
     * OK otherwise.
     */
    Status advanceClusterTime(const SignedLogicalTime&);

    /**
     * Similar to advaneClusterTime, but only does rate checking and not proof validation.
     */
    Status advanceClusterTimeFromTrustedSource(SignedLogicalTime newTime);

    /**
     * Similar to advanceClusterTimeFromTrustedSource, but also signs the new time. Note that this
     * should only be used on trusted LogicalTime (for example, LogicalTime extracted from local
     * oplog entry).
     */
    Status signAndAdvanceClusterTime(LogicalTime newTime);

    /**
     * Returns the current clusterTime.
     */
    SignedLogicalTime getClusterTime();

    /**
     * Returns the next clusterTime value and provides a guarantee that any future call to
     * reserveTicks() will return a value at least 'nTicks' ticks in the future from the current
     * clusterTime.
     */
    LogicalTime reserveTicks(uint64_t nTicks);

    /**
     * Resets _clusterTime to the signed time created from newTime. Should be used at the
     * initialization after reading the oplog. Must not be called on already initialized clock.
     */
    void initClusterTimeFromTrustedSource(LogicalTime newTime);

private:
    /**
     * Utility to create valid SignedLogicalTime from LogicalTime.
     */
    SignedLogicalTime _makeSignedLogicalTime(LogicalTime);

    Status _advanceClusterTime_inlock(SignedLogicalTime newTime);

    /**
     * Rate limiter for advancing logical time. Rejects newTime if its seconds value is more than
     * kMaxAcceptableLogicalClockDrift seconds ahead of this node's wall clock.
     */
    Status _passesRateLimiter_inlock(LogicalTime newTime);

    ServiceContext* const _service;
    std::unique_ptr<TimeProofService> _timeProofService;

    // the mutex protects _clusterTime
    stdx::mutex _mutex;
    SignedLogicalTime _clusterTime;

    /**
     * Temporary key only used for unit tests.
     *
     * TODO: SERVER-28436 Implement KeysCollectionManager
     * Remove _tempKey and its uses from logical clock, and pass actual key from key manager.
     */
    TimeProofService::Key _tempKey = {};
};

}  // namespace mongo
