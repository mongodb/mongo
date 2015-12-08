/**
 *    Copyright (C) 2014 MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/db/global_timestamp.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
AtomicUInt64 globalTimestamp(0);
}  // namespace

void setGlobalTimestamp(const Timestamp& newTime) {
    globalTimestamp.store(newTime.asULL());
}

Timestamp getLastSetTimestamp() {
    return Timestamp(globalTimestamp.load());
}

Timestamp getNextGlobalTimestamp() {
    const unsigned now = Date_t::now().toMillisSinceEpoch() / 1000;

    // Optimistic approach: just increment the timestamp, assuming the seconds still match.
    auto next = globalTimestamp.addAndFetch(1);
    unsigned globalSecs = Timestamp(next).getSecs();

    // Fail if time is not moving forward for 2**31 calls to getNextGlobalTimestamp.
    if (globalSecs > now && Timestamp(next).getInc() >= 1U << 31) {
        mongo::warning() << "clock skew detected, prev: " << Timestamp(next).getSecs()
                         << " now: " << now << std::endl;
        fassertFailed(17449);
    }

    //  While the seconds need to be updated, try to do it.
    while (globalSecs < now) {
        const auto expected = next;
        const auto desired = Timestamp(now, 1).asULL();

        // If the compareAndSwap was not successful, assume someone else updated the seconds.
        auto actual = globalTimestamp.compareAndSwap(expected, desired);
        if (actual == expected) {
            next = desired;
        } else {
            next = globalTimestamp.addAndFetch(1);
        }

        // Either way, the seconds should no longer be less than now, but repeat if we raced.
        globalSecs = Timestamp(next).getSecs();
    }

    return Timestamp(next);
}
}  // namespace mongo
