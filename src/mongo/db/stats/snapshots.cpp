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
*/

#include "mongo/pch.h"

#include "mongo/db/stats/snapshots.h"

#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"

/**
   handles snapshotting performance metrics and other such things
 */
namespace mongo {

    void SnapshotData::takeSnapshot() {
        _created = curTimeMicros64();
        _globalUsage = Top::global.getGlobalData();
//        _totalWriteLockedTime = d.dbMutex.info().getTimeLocked();
        Top::global.cloneMap(_usage);
    }

    SnapshotDelta::SnapshotDelta( const SnapshotData& older , const SnapshotData& newer )
        : _older( older ) , _newer( newer ) {
        verify( _newer._created > _older._created );
        _elapsed = _newer._created - _older._created;
    }

    Top::CollectionData SnapshotDelta::globalUsageDiff() {
        return Top::CollectionData( _older._globalUsage , _newer._globalUsage );
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

    void Snapshots::outputLockInfoHTML( stringstream& ss ) {
        scoped_lock lk(_lock);
        ss << "\n<div>";
        for ( int i=0; i<numDeltas(); i++ ) {
            SnapshotDelta d( getPrev(i+1) , getPrev(i) );
            unsigned e = (unsigned) d.elapsed() / 1000;
            ss << (unsigned)(100*d.percentWriteLocked());
            if( e < 3900 || e > 4100 )
                ss << '(' << e / 1000.0 << "s)";
            ss << ' ';
        }
        ss << "</div>\n";
    }

    void SnapshotThread::run() {
        Client::initThread("snapshotthread");
        Client& client = cc();

        long long numLoops = 0;

        const SnapshotData* prev = 0;

        while ( ! inShutdown() ) {
            try {
                const SnapshotData* s = statsSnapshots.takeSnapshot();

                if ( prev && cmdLine.cpu ) {
                    unsigned long long elapsed = s->_created - prev->_created;
                    SnapshotDelta d( *prev , *s );
                    log() << "cpu: elapsed:" << (elapsed/1000) <<"  writelock: " << (int)(100*d.percentWriteLocked()) << "%" << endl;
                }

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
