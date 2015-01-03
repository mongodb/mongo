// snapshots.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#define MONGO_PCH_WHITELISTED
#include "mongo/platform/basic.h"
#include "mongo/pch.h"
#undef MONGO_PCH_WHITELISTED

#include "mongo/db/stats/snapshots.h"

#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

/**
   handles snapshotting performance metrics and other such things
 */
namespace mongo {

    void SnapshotData::takeSnapshot() {
        _created = curTimeMicros64();
        Top::global.cloneMap(_usage);
    }

    SnapshotDelta::SnapshotDelta( const SnapshotData& older , const SnapshotData& newer )
        : _older( older ) , _newer( newer ) {
        verify( _newer._created > _older._created );
        _elapsed = _newer._created - _older._created;
    }

    Top::UsageMap SnapshotDelta::collectionUsageDiff() {
        verify( _newer._created > _older._created );
        Top::UsageMap u;

        for ( Top::UsageMap::const_iterator i=_newer._usage.begin(); i != _newer._usage.end(); ++i ) {
            Top::UsageMap::const_iterator j = _older._usage.find(i->first);
            if (j != _older._usage.end())
                u[i->first] = Top::CollectionData( j->second , i->second );
            else
                u[i->first] = i->second;
        }
        return u;
    }

    Snapshots::Snapshots(int n)
        : _lock("Snapshots"), _n(n)
        , _snapshots(new SnapshotData[n])
        , _loc(0)
        , _stored(0)
    {}

    const SnapshotData* Snapshots::takeSnapshot() {
        scoped_lock lk(_lock);
        _loc = ( _loc + 1 ) % _n;
        _snapshots[_loc].takeSnapshot();
        if ( _stored < _n )
            _stored++;
        return &_snapshots[_loc];
    }

    auto_ptr<SnapshotDelta> Snapshots::computeDelta( int numBack ) {
        scoped_lock lk(_lock);
        auto_ptr<SnapshotDelta> p;
        if ( numBack < numDeltas() )
            p.reset( new SnapshotDelta( getPrev(numBack+1) , getPrev(numBack) ) );
        return p;
    }

    const SnapshotData& Snapshots::getPrev( int numBack ) {
        int x = _loc - numBack;
        if ( x < 0 )
            x += _n;
        return _snapshots[x];
    }

    void SnapshotThread::run() {
        Client::initThread("snapshot");
        Client& client = cc();

        long long numLoops = 0;

        const SnapshotData* prev = 0;

        while ( ! inShutdown() ) {
            try {
                const SnapshotData* s = statsSnapshots.takeSnapshot();
                prev = s;
            }
            catch ( std::exception& e ) {
                log() << "ERROR in SnapshotThread: " << e.what() << endl;
            }

            numLoops++;
            sleepsecs(4);
        }

        client.shutdown();
    }

    Snapshots statsSnapshots;
    SnapshotThread snapshotThread;
}
