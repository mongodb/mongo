// snapshots.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/stats/top.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/background.h"

/**
   handles snapshotting performance metrics and other such things
 */
namespace mongo {

class StatsSnapshotThread;

/**
 * stores a point in time snapshot
 * i.e. all counters at a given time
 */
class SnapshotData {
    void takeSnapshot();

    unsigned long long _created;
    Top::UsageMap _usage;

    friend class StatsSnapshotThread;
    friend class SnapshotDelta;
    friend class Snapshots;
};

/**
 * contains performance information for a time period
 */
class SnapshotDelta {
public:
    SnapshotDelta(const SnapshotData& older, const SnapshotData& newer);

    unsigned long long elapsed() const {
        return _elapsed;
    }

    Top::UsageMap collectionUsageDiff();

private:
    const SnapshotData& _older;
    const SnapshotData& _newer;

    unsigned long long _elapsed;
};

struct SnapshotDiff {
    Top::UsageMap usageDiff;
    unsigned long long timeElapsed;

    SnapshotDiff() = default;
    SnapshotDiff(Top::UsageMap map, unsigned long long elapsed)
        : usageDiff(std::move(map)), timeElapsed(elapsed) {}
};

class Snapshots {
public:
    Snapshots();

    const SnapshotData* takeSnapshot();

    StatusWith<SnapshotDiff> computeDelta();

private:
    stdx::mutex _lock;
    static const int kNumSnapshots = 2;
    SnapshotData _snapshots[kNumSnapshots];
    int _loc;
    int _stored;
};

class StatsSnapshotThread : public BackgroundJob {
public:
    virtual std::string name() const {
        return "statsSnapshot";
    }
    void run();
};

extern Snapshots statsSnapshots;
extern StatsSnapshotThread statsSnapshotThread;
}
