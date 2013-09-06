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
#include "mongo/pch.h"
#include "../jsobj.h"
#include "top.h"
#include "../../util/background.h"

/**
   handles snapshotting performance metrics and other such things
 */
namespace mongo {

    class SnapshotThread;

    /**
     * stores a point in time snapshot
     * i.e. all counters at a given time
     */
    class SnapshotData {
        void takeSnapshot();

        unsigned long long _created;
        Top::CollectionData _globalUsage;
        unsigned long long _totalWriteLockedTime; // micros of total time locked
        Top::UsageMap _usage;

        friend class SnapshotThread;
        friend class SnapshotDelta;
        friend class Snapshots;
    };

    /**
     * contains performance information for a time period
     */
    class SnapshotDelta {
    public:
        SnapshotDelta( const SnapshotData& older , const SnapshotData& newer );

        unsigned long long start() const {
            return _older._created;
        }

        unsigned long long elapsed() const {
            return _elapsed;
        }

        unsigned long long timeInWriteLock() const {
            return _newer._totalWriteLockedTime - _older._totalWriteLockedTime;
        }
        double percentWriteLocked() const {
            double e = (double) elapsed();
            double w = (double) timeInWriteLock();
            return w/e;
        }

        Top::CollectionData globalUsageDiff();
        Top::UsageMap collectionUsageDiff();

    private:
        const SnapshotData& _older;
        const SnapshotData& _newer;

        unsigned long long _elapsed;
    };

    class Snapshots {
    public:
        Snapshots(int n=100);

        const SnapshotData* takeSnapshot();

        int numDeltas() const { return _stored-1; }

        const SnapshotData& getPrev( int numBack = 0 );
        auto_ptr<SnapshotDelta> computeDelta( int numBack = 0 );


        void outputLockInfoHTML( stringstream& ss );
    private:
        mongo::mutex _lock;
        int _n;
        boost::scoped_array<SnapshotData> _snapshots;
        int _loc;
        int _stored;
    };

    class SnapshotThread : public BackgroundJob {
    public:
        virtual string name() const { return "snapshot"; }
        void run();
    };

    extern Snapshots statsSnapshots;
    extern SnapshotThread snapshotThread;


}
