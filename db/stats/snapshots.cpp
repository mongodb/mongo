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

#include "pch.h"
#include "snapshots.h"
#include "../client.h"
#include "../clientcursor.h"
#include "../dbwebserver.h"
#include "../../util/mongoutils/html.h"

/**
   handles snapshotting performance metrics and other such things
 */
namespace mongo {
    void SnapshotData::takeSnapshot() {
        _created = curTimeMicros64();
        _globalUsage = Top::global.getGlobalData();
        _totalWriteLockedTime = dbMutex.info().getTimeLocked();
        Top::global.cloneMap(_usage);
    }

    SnapshotDelta::SnapshotDelta( const SnapshotData& older , const SnapshotData& newer )
        : _older( older ) , _newer( newer ) {
        assert( _newer._created > _older._created );
        _elapsed = _newer._created - _older._created;
    }

    Top::CollectionData SnapshotDelta::globalUsageDiff() {
        return Top::CollectionData( _older._globalUsage , _newer._globalUsage );
    }
    Top::UsageMap SnapshotDelta::collectionUsageDiff() {
        assert( _newer._created > _older._created );
        Top::UsageMap u;

        for ( Top::UsageMap::const_iterator i=_newer._usage.begin(); i != _newer._usage.end(); i++ ) {
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
        if (numDeltas() == 0) {
          ss << "&nbsp;";
          return;
        }
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

    using namespace mongoutils::html;

    class WriteLockStatus : public WebStatusPlugin {
    public:
        WriteLockStatus() : WebStatusPlugin( "write lock" , 51 , "% time in write lock, by 4 sec periods <a id=\"wlHelp\" href=\"http://www.mongodb.org/pages/viewpage.action?pageId=7209296\" title=\"snapshot: was the db in the write lock when this page was generated?\"><div class=\"help\"></div></a>" ) {}
        virtual void init() {}

        virtual void run( stringstream& ss ) {
            ss << labelValue("current status", (dbMutex.info().isLocked() ? "locked" : "not locked"));
            ss << "<div class=\"info\"><label>historical status</label>";
            statsSnapshots.outputLockInfoHTML( ss );
            ss << "</div>";
        }

    } writeLockStatus;

    class DBTopStatus : public WebStatusPlugin {
    public:
        DBTopStatus() : WebStatusPlugin( "dbtop" , 50 , "occurrences | percent of elapsed" ) {}

        void display( stringstream& ss , double elapsed , const Top::UsageData& usage, bool alt ) {
            openTd( ss, alt );
            ss << usage.count;
            ss << "</td>";
            openTd( ss, alt );
            double per = 100 * ((double)usage.time)/elapsed;
            if( per == (int) per )
                ss << (int) per;
            else
                ss << setprecision(1) << fixed << per;
            ss << '%';
            ss << "</td>";
        }
        
        void openTd( stringstream& ss, bool alt ) {
          if (alt) {
              ss << "<td class=\"alt\">";  
          } else {
              ss << "<td>";  
          }
        }

        void display( stringstream& ss , double elapsed , const string& ns , const Top::CollectionData& data ) {
            if ( ns != "total" && data.total.count == 0 )
                return;
            ss << "<tr><th>" << ns << "</th>";

            display( ss , elapsed , data.total, false );

            display( ss , elapsed , data.readLock, true );
            display( ss , elapsed , data.writeLock, false );

            display( ss , elapsed , data.queries, true );
            display( ss , elapsed , data.getmore, false );
            display( ss , elapsed , data.insert, true );
            display( ss , elapsed , data.update, false );
            display( ss , elapsed , data.remove, true );

            ss << "</tr>\n";
        }

        void run( stringstream& ss ) {
            auto_ptr<SnapshotDelta> delta = statsSnapshots.computeDelta();
            if ( ! delta.get() )
                return;

            ss << "<table id=\"dbtop\">";
            ss << "<tr><th>";
            ss << a("http://www.mongodb.org/display/DOCS/Developer+FAQ#DeveloperFAQ-What%27sa%22namespace%22%3F", "namespace") <<
               "ns</a></th>"
               "<th colspan=2 class=c>total</th>"
               "<th colspan=2 class=\"c alt\">reads</th>"
               "<th colspan=2 class=c>writes</th>"
               "<th colspan=2 class=\"c alt\">queries</th>"
               "<th colspan=2 class=c>getmores</th>"
               "<th colspan=2 class=\"c alt\">inserts</th>"
               "<th colspan=2 class=c>updates</th>"
               "<th colspan=2 class=\"c alt\">removes</th>";
            ss << "</tr>\n";

            display( ss , (double) delta->elapsed() , "total" , delta->globalUsageDiff() );

            Top::UsageMap usage = delta->collectionUsageDiff();
            for ( Top::UsageMap::iterator i=usage.begin(); i != usage.end(); i++ ) {
                display( ss , (double) delta->elapsed() , i->first , i->second );
            }

            ss << "</table>";

        }

        virtual void init() {}
    } dbtopStatus;

    Snapshots statsSnapshots;
    SnapshotThread snapshotThread;
}
