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
#define assert xassert

namespace mongo {

/* Records per namespace utilization of the mongod process.
   No two functions of this class may be called concurrently.
*/
class Top {
    typedef boost::posix_time::ptime T;
    typedef boost::posix_time::time_duration D;
public:
    Top() : read_(false), write_(false) { }

    /* these are used to record activity: */

    void clientStart( const char *client ) {
        clientStop();
        currentStart_ = currentTime();
        current_ = client;
    }

    /* indicate current request is a read operation. */
    void setRead() { read_ = true; }

    void setWrite() { write_ = true; }

    void clientStop() {
        if ( currentStart_ == T() )
            return;
        D d = currentTime() - currentStart_;

        {
            boostlock L(topMutex);
            recordUsage( current_, d );
        }

        currentStart_ = T();
        read_ = false;
        write_ = false;
    }

    /* these are used to fetch the stats: */

    struct Usage { 
        string ns; 
        D time; 
        double pct; 
        int reads, writes, calls; 
    };

    static void usage( vector< Usage > &res ) {
        boostlock L(topMutex);

        // Populate parent namespaces
        UsageMap snapshot;
        UsageMap totalUsage;
        fillParentNamespaces( snapshot, snapshot_ );
        fillParentNamespaces( totalUsage, totalUsage_ );
        
        multimap< D, string, more > sorted;
        for( UsageMap::iterator i = snapshot.begin(); i != snapshot.end(); ++i )
            sorted.insert( make_pair( i->second.get<0>(), i->first ) );
        for( multimap< D, string, more >::iterator i = sorted.begin(); i != sorted.end(); ++i ) {
            if ( trivialNs( i->second.c_str() ) )
                continue;
            Usage u;
            u.ns = i->second;
            u.time = totalUsage[ u.ns ].get<0>();
            u.pct = snapshotDuration_ != D() ? 100.0 * i->first.ticks() / snapshotDuration_.ticks() : 0;
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
        boostlock L(topMutex);

        if ( &snapshot_ == &snapshotA_ ) {
            snapshot_ = snapshotB_;
            nextSnapshot_ = snapshotA_;
        } else {
            snapshot_ = snapshotA_;
            nextSnapshot_ = snapshotB_;
        }
        snapshotDuration_ = currentTime() - snapshotStart_;
        snapshotStart_ = currentTime();
        nextSnapshot_.clear();
    }

private:
    static boost::mutex topMutex;
    static bool trivialNs( const char *ns ) {
        const char *ret = strrchr( ns, '.' );
        return ret && ret[ 1 ] == '\0';
    }
    typedef map< string, boost::tuple< D, int, int, int > > UsageMap; // duration, # reads, # writes, # total calls
    static T currentTime() {
        return boost::posix_time::microsec_clock::universal_time();
    }
    void recordUsage( const string &client, D duration ) {
        recordUsageForMap( totalUsage_, client, duration );
        recordUsageForMap( nextSnapshot_, client, duration );
    }
    void recordUsageForMap( UsageMap &map, const string &client, D duration ) {
        map[ client ].get< 0 >() += duration;
        if ( read_ && !write_ )
            map[ client ].get< 1 >()++;
        else if ( !read_ && write_ )
            map[ client ].get< 2 >()++;
        map[ client ].get< 3 >()++;        
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
    static void inc( boost::tuple< D, int, int, int > &to, const boost::tuple< D, int, int, int > &from ) {
        to.get<0>() += from.get<0>();
        to.get<1>() += from.get<1>();
        to.get<2>() += from.get<2>();
        to.get<3>() += from.get<3>();
    }
    struct more { bool operator()( const D &a, const D &b ) { return a > b; } };
    string current_;
    T currentStart_;
    static T snapshotStart_;
    static D snapshotDuration_;
    static UsageMap totalUsage_;
    static UsageMap snapshotA_;
    static UsageMap snapshotB_;
    static UsageMap &snapshot_;
    static UsageMap &nextSnapshot_;
    bool read_;
    bool write_;
};

} // namespace mongo
