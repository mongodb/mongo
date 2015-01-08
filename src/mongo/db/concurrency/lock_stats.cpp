/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/lock_stats.h"

namespace mongo {

    LockStats::LockStats() {

    }

    void LockStats::recordAcquisition(ResourceId resId, LockMode mode) {
        PerModeAtomicLockStats& stat = get(resId);
        stat.stats[mode].numAcquisitions.addAndFetch(1);
    }

    void LockStats::recordWait(ResourceId resId, LockMode mode) {
        PerModeAtomicLockStats& stat = get(resId);
        stat.stats[mode].numWaits.addAndFetch(1);
    }

    void LockStats::recordWaitTime(ResourceId resId, LockMode mode, uint64_t waitMicros) {
        PerModeAtomicLockStats& stat = get(resId);
        stat.stats[mode].combinedWaitTimeMicros.addAndFetch(waitMicros);
    }

    void LockStats::append(const LockStats& other) {
        // Append all lock stats
        for (int i = 0; i < ResourceTypesCount; i++) {
            for (int mode = 0; mode < LockModesCount; mode++) {
                const AtomicLockStats& otherStats = other._stats[i].stats[mode];

                AtomicLockStats& thisStats = _stats[i].stats[mode];
                thisStats.append(otherStats);
            }
        }

        // Append the oplog stats
        for (int mode = 0; mode < LockModesCount; mode++) {
            const AtomicLockStats& otherStats = other._oplogStats.stats[mode];

            AtomicLockStats& thisStats = _oplogStats.stats[mode];
            thisStats.append(otherStats);
        }
    }

    void LockStats::report(BSONObjBuilder* builder) const {
        // All indexing below starts from offset 1, because we do not want to report/account
        // position 0, which is a sentinel value for invalid resource/no lock.

        for (int i = 1; i < ResourceTypesCount; i++) {
            BSONObjBuilder resBuilder(builder->subobjStart(
                                                resourceTypeName(static_cast<ResourceType>(i))));

            _report(&resBuilder, _stats[i]);

            resBuilder.done();
        }

        BSONObjBuilder resBuilder(builder->subobjStart("oplog"));
        _report(&resBuilder, _oplogStats);
        resBuilder.done();
    }

    void LockStats::_report(BSONObjBuilder* builder,
                                        const PerModeAtomicLockStats& stat) const {

        // All indexing below starts from offset 1, because we do not want to report/account
        // position 0, which is a sentinel value for invalid resource/no lock.

        // Num acquires
        {
            BSONObjBuilder numAcquires(builder->subobjStart("acquireCount"));
            for (int mode = 1; mode < LockModesCount; mode++) {
                numAcquires.append(legacyModeName(static_cast<LockMode>(mode)),
                                   stat.stats[mode].numAcquisitions.load());
            }
            numAcquires.done();
        }

        // Num waits
        {
            BSONObjBuilder numWaits(builder->subobjStart("acquireWaitCount"));
            for (int mode = 1; mode < LockModesCount; mode++) {
                numWaits.append(legacyModeName(static_cast<LockMode>(mode)),
                                stat.stats[mode].numWaits.load());
            }
            numWaits.done();
        }

        // Total time waiting
        {
            BSONObjBuilder timeAcquiring(builder->subobjStart("timeAcquiringMicros"));
            for (int mode = 1; mode < LockModesCount; mode++) {
                timeAcquiring.append(legacyModeName(static_cast<LockMode>(mode)),
                                     stat.stats[mode].combinedWaitTimeMicros.load());
            }
            timeAcquiring.done();
        }
    }

    void LockStats::reset() {
        for (int i = 0; i < ResourceTypesCount; i++) {
            for (int mode = 0; mode < LockModesCount; mode++) {
                _stats[i].stats[mode].reset();
            }
        }

        for (int mode = 0; mode < LockModesCount; mode++) {
            _oplogStats.stats[mode].reset();
        }
    }

    LockStats::PerModeAtomicLockStats& LockStats::get(ResourceId resId) {
        if (resId == resourceIdOplog) {
            return _oplogStats;
        }
        else {
            return _stats[resId.getType()];
        }
    }


    //
    // AtomicLockStats
    //

    void LockStats::AtomicLockStats::append(const AtomicLockStats& other) {
        numAcquisitions.addAndFetch(other.numAcquisitions.load());
        numWaits.addAndFetch(other.numWaits.load());
        combinedWaitTimeMicros.addAndFetch(other.combinedWaitTimeMicros.load());
    }

    void LockStats::AtomicLockStats::reset() {
        numAcquisitions.store(0);
        numWaits.store(0);
        combinedWaitTimeMicros.store(0);
    }

} // namespace mongo
