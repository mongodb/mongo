// top.h : DB usage monitor.

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#undef assert
#define assert MONGO_assert

namespace mongo {

    /**
     * tracks usage by collection
     */
    class Top {

    public:
        Top() : _lock("Top") { }

        struct UsageData {
            UsageData() : time(0) , count(0){}
            UsageData( const UsageData& older , const UsageData& newer );
            long long time;
            long long count;

            void inc( long long micros ){
                count++;
                time += micros;
            }
        };

        struct CollectionData {
            /**
             * constructs a diff
             */
            CollectionData(){}
            CollectionData( const CollectionData& older , const CollectionData& newer );
            
            UsageData total;
            
            UsageData readLock;
            UsageData writeLock;

            UsageData queries;
            UsageData getmore;
            UsageData insert;
            UsageData update;
            UsageData remove;
            UsageData commands;
        };

        typedef map<string,CollectionData> UsageMap;
        
    public:
        void record( const string& ns , int op , int lockType , long long micros , bool command );
        void append( BSONObjBuilder& b );
        void cloneMap(UsageMap& out) const;
        CollectionData getGlobalData() const { return _global; }
        void collectionDropped( const string& ns );

    public: // static stuff
        static Top global;
        
    private:
        void _appendToUsageMap( BSONObjBuilder& b , const UsageMap& map ) const;        
        void _appendStatsEntry( BSONObjBuilder& b , const char * statsName , const UsageData& map ) const;        
        void _record( CollectionData& c , int op , int lockType , long long micros , bool command );

        mutable mongo::mutex _lock;
        CollectionData _global;
        UsageMap _usage;
        string _lastDropped;
    };

    /* Records per namespace utilization of the mongod process.
       No two functions of this class may be called concurrently.
    */
    class TopOld {
        typedef boost::posix_time::ptime T;
        typedef boost::posix_time::time_duration D;
        typedef boost::tuple< D, int, int, int > UsageData;
    public:
        TopOld() : _read(false), _write(false) { }
        
        /* these are used to record activity: */
        
        void clientStart( const char *client ) {
            clientStop();
            _currentStart = currentTime();
            _current = client;
        }

        /* indicate current request is a read operation. */
        void setRead() { _read = true; }

        void setWrite() { _write = true; }

        void clientStop() {
            if ( _currentStart == T() )
                return;
            D d = currentTime() - _currentStart;

            {
                scoped_lock L(topMutex);
                recordUsage( _current, d );
            }

            _currentStart = T();
            _read = false;
            _write = false;
        }

        /* these are used to fetch the stats: */

        struct Usage { 
            string ns; 
            D time; 
            double pct; 
            int reads, writes, calls; 
        };

        static void usage( vector< Usage > &res ) {
            scoped_lock L(topMutex);

            // Populate parent namespaces
            UsageMap snapshot;
            UsageMap totalUsage;
            fillParentNamespaces( snapshot, _snapshot );
            fillParentNamespaces( totalUsage, _totalUsage );
        
            multimap< D, string, more > sorted;
            for( UsageMap::iterator i = snapshot.begin(); i != snapshot.end(); ++i )
                sorted.insert( make_pair( i->second.get<0>(), i->first ) );
            for( multimap< D, string, more >::iterator i = sorted.begin(); i != sorted.end(); ++i ) {
                if ( trivialNs( i->second.c_str() ) )
                    continue;
                Usage u;
                u.ns = i->second;
                u.time = totalUsage[ u.ns ].get<0>();
                u.pct = _snapshotDuration != D() ? 100.0 * i->first.ticks() / _snapshotDuration.ticks() : 0;
                u.reads = snapshot[ u.ns ].get<1>();
                u.writes = snapshot[ u.ns ].get<2>();
                u.calls = snapshot[ u.ns ].get<3>();
                res.push_back( u );
            }
            for( UsageMap::iterator i = totalUsage.begin(); i != totalUsage.end(); ++i ) {
                if ( snapshot.count( i->first ) != 0 || trivialNs( i->first.c_str() ) )
                    continue;
                Usage u;
                u.ns = i->first;
                u.time = i->second.get<0>();
                u.pct = 0;
                u.reads = 0;
                u.writes = 0;
                u.calls = 0;
                res.push_back( u );
            }
        }

        static void completeSnapshot() {
            scoped_lock L(topMutex);

            if ( &_snapshot == &_snapshotA ) {
                _snapshot = _snapshotB;
                _nextSnapshot = _snapshotA;
            } else {
                _snapshot = _snapshotA;
                _nextSnapshot = _snapshotB;
            }
            _snapshotDuration = currentTime() - _snapshotStart;
            _snapshotStart = currentTime();
            _nextSnapshot.clear();
        }

    private:
        static mongo::mutex topMutex;
        static bool trivialNs( const char *ns ) {
            const char *ret = strrchr( ns, '.' );
            return ret && ret[ 1 ] == '\0';
        }
        typedef map<string,UsageData> UsageMap; // duration, # reads, # writes, # total calls
        static T currentTime() {
            return boost::posix_time::microsec_clock::universal_time();
        }
        void recordUsage( const string &client, D duration ) {
            recordUsageForMap( _totalUsage, client, duration );
            recordUsageForMap( _nextSnapshot, client, duration );
        }
        void recordUsageForMap( UsageMap &map, const string &client, D duration ) {
            UsageData& g = map[client];
            g.get< 0 >() += duration;
            if ( _read && !_write )
                g.get< 1 >()++;
            else if ( !_read && _write )
                g.get< 2 >()++;
            g.get< 3 >()++;        
        }
        static void fillParentNamespaces( UsageMap &to, const UsageMap &from ) {
            for( UsageMap::const_iterator i = from.begin(); i != from.end(); ++i ) {
                string current = i->first;
                size_t dot = current.rfind( "." );
                if ( dot == string::npos || dot != current.length() - 1 ) {
                    inc( to[ current ], i->second );
                }
                while( dot != string::npos ) {
                    current = current.substr( 0, dot );
                    inc( to[ current ], i->second );
                    dot = current.rfind( "." );
                }            
            }        
        }
        static void inc( UsageData &to, const UsageData &from ) {
            to.get<0>() += from.get<0>();
            to.get<1>() += from.get<1>();
            to.get<2>() += from.get<2>();
            to.get<3>() += from.get<3>();
        }
        struct more { bool operator()( const D &a, const D &b ) { return a > b; } };
        string _current;
        T _currentStart;
        static T _snapshotStart;
        static D _snapshotDuration;
        static UsageMap _totalUsage;
        static UsageMap _snapshotA;
        static UsageMap _snapshotB;
        static UsageMap &_snapshot;
        static UsageMap &_nextSnapshot;
        bool _read;
        bool _write;
    };

} // namespace mongo
