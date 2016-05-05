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
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {
// This is the value of the next timestamp to handed out.
AtomicUInt64 globalTimestamp(0);
}  // namespace

void setGlobalTimestamp(const Timestamp& newTime) {
    globalTimestamp.store(newTime.asULL() + 1);
}

Timestamp getLastSetTimestamp() {
    return Timestamp(globalTimestamp.load() - 1);
}

Timestamp getNextGlobalTimestamp(unsigned count) {
    const unsigned now = durationCount<Seconds>(
        getGlobalServiceContext()->getFastClockSource()->now().toDurationSinceEpoch());
    invariant(now != 0);  // This is a sentinel value for null Timestamps.
    invariant(count != 0);

    // Optimistic approach: just increment the timestamp, assuming the seconds still match.
    auto first = globalTimestamp.fetchAndAdd(count);
    auto currentTimestamp = first + count;  // What we just set it to.
    unsigned globalSecs = Timestamp(currentTimestamp).getSecs();

    // Fail if time is not moving forward for 2**31 calls to getNextGlobalTimestamp.
    if (MONGO_unlikely(globalSecs > now) && Timestamp(currentTimestamp).getInc() >= 1U << 31) {
        mongo::severe() << "clock skew detected, prev: " << globalSecs << " now: " << now;
        fassertFailed(17449);
    }

    // If the seconds need to be updated, try to do it. This can happen at most once per second.
    if (MONGO_unlikely(globalSecs < now)) {
        // First fix the seconds portion.
        while (globalSecs < now) {
            const auto desired = Timestamp(now, 1).asULL();

            auto actual = globalTimestamp.compareAndSwap(currentTimestamp, desired);
            if (actual == currentTimestamp)
                break;  // We successfully set the secs, so we're done here.

            // We raced with someone else. Try again, unless they fixed the secs field for us.
            currentTimestamp = actual;
            globalSecs = Timestamp(currentTimestamp).getSecs();
        }

        // Now reserve our timestamps with the new value of secs.
        first = globalTimestamp.fetchAndAdd(count);
    }

    return Timestamp(first);
}
}  // namespace mongo
