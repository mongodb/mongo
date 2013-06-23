// stat_util.cpp

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

#include <iomanip>

#include "stat_util.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongoutils;

namespace mongo {

    StatUtil::StatUtil( double seconds , bool all ) :
        _seconds( seconds ) ,
        _all( all )
        
    {

    }

    bool StatUtil::_in( const BSONElement& me , const BSONElement& arr ) {
        if ( me.type() != String || arr.type() != Array )
            return false;

        string s = me.String();
        BSONForEach(e, arr.Obj()) {
            if ( e.type() == String && s == e.String() )
                return true;
        }
        return false;
    }

    BSONObj StatUtil::doRow( const BSONObj& a , const BSONObj& b ) {
        BSONObjBuilder result;

        bool isMongos =  b["shardCursorType"].type() == Object || b["process"].String() == "mongos";

        if ( a["opcounters"].isABSONObj() && b["opcounters"].isABSONObj() ) {
            BSONObj ax = a["opcounters"].embeddedObject();
            BSONObj bx = b["opcounters"].embeddedObject();

            BSONObj ar = a["opcountersRepl"].isABSONObj() ? a["opcountersRepl"].embeddedObject() : BSONObj();
            BSONObj br = b["opcountersRepl"].isABSONObj() ? b["opcountersRepl"].embeddedObject() : BSONObj();

            BSONObjIterator i( bx );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( ar.isEmpty() || br.isEmpty() ) {
                    _append( result , e.fieldName() , 6 , (int)diff( e.fieldName() , ax , bx ) );
                }
                else {
                    string f = e.fieldName();

                    int m = (int)diff( f , ax , bx );
                    int r = (int)diff( f , ar , br );

                    string myout;

                    if ( f == "command" ) {
                        myout = str::stream() << m << "|" << r;
                    }
                    else if ( f == "getmore" ) {
                        myout = str::stream() << m;
                    }
                    else if ( m && r ) {
                        // this is weird...
                        myout = str::stream() << m << "|" << r;
                    }
                    else if ( m ) {
                        myout = str::stream() << m;
                    }
                    else if ( r ) {
                        myout = str::stream() << "*" << r;
                    }
                    else {
                        myout = "*0";
                    }

                    _append( result , f , 6 , myout );
                }
            }
        }

        if ( b["backgroundFlushing"].type() == Object ) {
            BSONObj ax = a["backgroundFlushing"].embeddedObject();
            BSONObj bx = b["backgroundFlushing"].embeddedObject();
            _append( result , "flushes" , 6 , (int)diff( "flushes" , ax , bx ) );
        }

        if ( b.getFieldDotted("mem.supported").trueValue() ) {
            BSONObj bx = b["mem"].embeddedObject();
            BSONObjIterator i( bx );
            if (!isMongos)
                _appendMem( result , "mapped" , 6 , bx["mapped"].numberInt() );
            _appendMem( result , "vsize" , 6 , bx["virtual"].numberInt() );
            _appendMem( result , "res" , 6 , bx["resident"].numberInt() );

            if ( !isMongos && _all )
                _appendMem( result , "non-mapped" , 6 , bx["virtual"].numberInt() - bx["mapped"].numberInt() );
        }

        if ( b["extra_info"].type() == Object ) {
            BSONObj ax = a["extra_info"].embeddedObject();
            BSONObj bx = b["extra_info"].embeddedObject();
            if ( ax["page_faults"].type() )
                _append( result , "faults" , 6 , (int)diff( "page_faults" , ax , bx ) );
        }

        if (!isMongos) {
            
            if ( b["locks"].isABSONObj() ) {
                // report either the global lock % or the db with the highest lock % + the global lock
                NamespaceStats prevStats = parseServerStatusLocks( a );
                NamespaceStats curStats = parseServerStatusLocks( b );
                vector<NamespaceDiff> diffs = computeDiff( prevStats , curStats );

                if ( diffs.size() == 0 ) {
                    _append( result , "locked %" , 8 , 0 );
                }
                else {

                    // diff() divides the result by _seconds, need total uptime here
                    double uptimeMillis = diff( "uptimeMillis" , a , b ) * _seconds;
                    unsigned idx = diffs.size()-1;
                    
                    double lockToReport = diffs[idx].write;
                    if ( diffs[idx].ns != "." ) {
                        for ( unsigned i = 0; i < diffs.size(); i++ ) {
                            if ( diffs[i].ns == "." )
                                lockToReport += diffs[i].write;
                        }
                    }

                    stringstream ss;
                    ss.setf( ios::fixed );
                    ss << diffs[idx].ns << ":" << setprecision(1) << ( 100.0 * lockToReport / uptimeMillis ) << "%";
                    // set the width to be the greater of the field header size or the actual field length, e.g., 'mylongdb:89.1%' 
                    _append( result , "locked db" , ( ss.str().size() > 10 ? ss.str().size() : 10 ) , ss.str() );
                }
            }
            else {
                _append( result , "locked %" , 8 , percent( "globalLock.totalTime" , "globalLock.lockTime" , a , b ) );
            }

            
            _append( result , "idx miss %" , 8 , percent( "indexCounters.btree.accesses" , "indexCounters.btree.misses" , a , b ) );
        }

        if ( b.getFieldDotted( "globalLock.currentQueue" ).type() == Object ) {
            int r = b.getFieldDotted( "globalLock.currentQueue.readers" ).numberInt();
            int w = b.getFieldDotted( "globalLock.currentQueue.writers" ).numberInt();
            stringstream temp;
            temp << r << "|" << w;
            _append( result , "qr|qw" , 9 , temp.str() );
        }

        if ( b.getFieldDotted( "globalLock.activeClients" ).type() == Object ) {
            int r = b.getFieldDotted( "globalLock.activeClients.readers" ).numberInt();
            int w = b.getFieldDotted( "globalLock.activeClients.writers" ).numberInt();
            stringstream temp;
            temp << r << "|" << w;
            _append( result , "ar|aw" , 7 , temp.str() );
        }

        if ( a["network"].isABSONObj() && b["network"].isABSONObj() ) {
            BSONObj ax = a["network"].embeddedObject();
            BSONObj bx = b["network"].embeddedObject();
            _appendNet( result , "netIn" , diff( "bytesIn" , ax , bx ) );
            _appendNet( result , "netOut" , diff( "bytesOut" , ax , bx ) );
        }

        _append( result , "conn" , 5 , b.getFieldDotted( "connections.current" ).numberInt() );

        if ( b["repl"].type() == Object ) {

            BSONObj x = b["repl"].embeddedObject();
            bool isReplSet = x["setName"].type() == String;

            stringstream ss;

            if ( isReplSet ) {
                string setName = x["setName"].String();
                _append( result , "set" , setName.size() , setName );
            }

            if ( x["ismaster"].trueValue() )
                ss << "PRI";
            else if ( x["secondary"].trueValue() )
                ss << "SEC";
            else if ( x["isreplicaset"].trueValue() )
                ss << "REC";
            else if ( x["arbiterOnly"].trueValue() )
                ss << "ARB";
            else if ( _in( x["me"] , x["passives"] ) )
                ss << "PSV";
            else if ( isReplSet ) 
                ss << "UNK";
            else
                ss << "SLV";

            _append( result , "repl" , 4 , ss.str() );

        }
        else if ( isMongos ) {
            _append( result , "repl" , 4 , "RTR" );
        }

        {
            struct tm t;
            time_t_to_Struct( time(0), &t , true );
            stringstream temp;
            temp << setfill('0') << setw(2) << t.tm_hour
                 << ":"
                 << setfill('0') << setw(2) << t.tm_min
                 << ":"
                 << setfill('0') << setw(2) << t.tm_sec;
            _append( result , "time" , 10 , temp.str() );
        }
        return result.obj();
    }



    double StatUtil::percent( const char * outof , const char * val , const BSONObj& a , const BSONObj& b ) {
        double x = ( b.getFieldDotted( val ).number() - a.getFieldDotted( val ).number() );
        double y = ( b.getFieldDotted( outof ).number() - a.getFieldDotted( outof ).number() );
        if ( y == 0 )
            return 0;
        double p = x / y;
        p = (double)((int)(p * 1000)) / 10;
        return p;
    }



    double StatUtil::diff( const string& name , const BSONObj& a , const BSONObj& b ) {
        BSONElement x = a.getFieldDotted( name.c_str() );
        BSONElement y = b.getFieldDotted( name.c_str() );
        if ( ! x.isNumber() || ! y.isNumber() )
            return -1;
        return ( y.number() - x.number() ) / _seconds;
    }


    void StatUtil::_appendNet( BSONObjBuilder& result , const string& name , double diff ) {
        // I think 1000 is correct for megabit, but I've seen conflicting things (ERH 11/2010)
        const double div = 1000;

        string unit = "b";

        if ( diff >= div ) {
            unit = "k";
            diff /= div;
        }

        if ( diff >= div ) {
            unit = "m";
            diff /= div;
        }

        if ( diff >= div ) {
            unit = "g";
            diff /= div;
        }

        string out = str::stream() << (int)diff << unit;
        _append( result , name , 6 , out );
    }


    
    void StatUtil::_appendMem( BSONObjBuilder& result , const string& name , unsigned width , double sz ) {
        string unit = "m";
        if ( sz > 1024 ) {
            unit = "g";
            sz /= 1024;
        }
        
        if ( sz >= 1000 ) {
            string s = str::stream() << (int)sz << unit;
            _append( result , name , width , s );
            return;
        }
        
        stringstream ss;
        ss << setprecision(3) << sz << unit;
        _append( result , name , width , ss.str() );
    }

    NamespaceStats StatUtil::parseServerStatusLocks( const BSONObj& serverStatus ) {
        NamespaceStats stats;

        if ( ! serverStatus["locks"].isABSONObj() ) {
            cout << "locks doesn't exist, old mongod? (<2.2?)" << endl;
            return stats;
        }
        
        BSONObj locks = serverStatus["locks"].Obj();
        
        BSONObjIterator i( locks );
        while ( i.more() ) {
            BSONElement e = i.next();
            
            NamespaceInfo& s = stats[e.fieldName()];
            s.ns = e.fieldName();
            
            BSONObj temp = e.Obj()["timeLockedMicros"].Obj();
            s.read = ( temp["r"].numberLong() + temp["R"].numberLong() ) / 1000;
            s.write = ( temp["w"].numberLong() + temp["W"].numberLong() ) / 1000;
        }
        
        return stats;
        
    }

    vector<NamespaceDiff> StatUtil::computeDiff( const NamespaceStats& prev , const NamespaceStats& current ) {
        vector<NamespaceDiff> data;
        
        for ( NamespaceStats::const_iterator i = current.begin() ; i != current.end(); ++i ) {
            const string& ns = i->first;
            const NamespaceInfo& b = i->second;
            if ( prev.find( ns ) == prev.end() )
                    continue;
            const NamespaceInfo& a = prev.find( ns )->second;
            
            // invalid, data fixed in 1.8.0
            if ( ns[0] == '?' )
                continue;
            
            data.push_back( NamespaceDiff( a , b ) );
        }

        std::sort( data.begin() , data.end() );

        return data;
    }
}

