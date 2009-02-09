// top.h : DB usage monitor.
//

/**
 *    Copyright (C) 2009 10gen Inc.
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

namespace mongo {

// No two functions of this class may be called concurrently.
class Top {
public:
    typedef boost::posix_time::ptime T;
    typedef boost::posix_time::time_duration D;
    static void clientStart( const char *client ) {
        clientStop();
        currentStart_ = currentTime();
        current_ = client;
    }
    static void clientStop() {
        if ( currentStart_ == T() )
            return;
        D d = currentTime() - currentStart_;
        size_t dot = current_.rfind( "." );
        if ( dot == string::npos || dot != current_.length() - 1 )
            recordUsage( current_, d );
        while( dot != string::npos ) {
            current_ = current_.substr( 0, dot );
            recordUsage( current_, d );
            dot = current_.rfind( "." );
        }
        currentStart_ = T();
    }
    struct Usage { string ns; D time; double pct; };
    static void usage( vector< Usage > &res ) {
        multimap< D, string, more > sorted;
        for( map< string, D >::iterator i = snapshot_.begin(); i != snapshot_.end(); ++i )
            sorted.insert( make_pair( i->second, i->first ) );
        set< string > already;
        for( multimap< D, string >::iterator i = sorted.begin(); i != sorted.end(); ++i ) {
            Usage u;
            already.insert( i->second );
            u.ns = i->second;
            u.time = totalUsage_[ u.ns ];
            u.pct = snapshotDuration_ != D() ? 100.0 * i->first.ticks() / snapshotDuration_.ticks() : 0;
            res.push_back( u );
        }
        for( map< string, D >::iterator i = totalUsage_.begin(); i != totalUsage_.end(); ++i ) {
            if ( already.count( i->first ) != 0 )
                continue;
            Usage u;
            u.ns = i->first;
            u.time = i->second;
            u.pct = 0;
            res.push_back( u );
        }
    }
    static void completeSnapshot() {
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
    typedef map< string, D > UsageMap;
    static T currentTime() {
        return boost::posix_time::microsec_clock::universal_time();
    }
    static void recordUsage( const string &client, D duration ) {
        totalUsage_[ client ] += duration;
        nextSnapshot_[ client ] += duration;
    }
    struct more { bool operator()( const D &a, const D &b ) { return a > b; } };
    static string current_;
    static T currentStart_;
    static T snapshotStart_;
    static D snapshotDuration_;
    static UsageMap totalUsage_;
    static UsageMap snapshotA_;
    static UsageMap snapshotB_;
    static UsageMap &snapshot_;
    static UsageMap &nextSnapshot_;
};

} // namespace mongo