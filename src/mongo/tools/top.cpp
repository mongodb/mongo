// top.cpp

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

#include "db/json.h"
#include "../util/text.h"
#include "tool.h"
#include <fstream>
#include <iostream>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace mongo {

    class TopTool : public Tool {

        struct NSStat {
            string ns;

            // these need to be in millis
            long long read;
            long long write;

            string toString() const {
                stringstream ss;
                ss << ns << " r: " << read << " w: " << write;
                return ss.str();
            }
        };

        struct Diff {
            string ns;

            long long read;
            long long write;

            Diff( NSStat prev , NSStat now ) {
                ns = prev.ns;
                read = now.read - prev.read;
                write = now.write - prev.write;
            }

            long long total() const { return read + write; }

            bool operator<(const Diff& r) const {
                return total() < r.total();
            }
        };

        typedef map<string,NSStat> Stats;

    public:

        TopTool() : Tool( "top" , REMOTE_SERVER , "admin" ) {
            _sleep = 1;

            add_hidden_options()
            ( "sleep" , po::value<int>() , "time to sleep between calls" )
            ( "locks" , "use db lock info instead of top" )
            ;
            addPositionArg( "sleep" , 1 );

            _autoreconnect = true;
        }

        virtual void printExtraHelp( ostream & out ) {
            out << "View live MongoDB collection statistics.\n" << endl;
        }
        
        bool useLocks() {
            return hasParam( "locks" );
        }

        Stats getData() {
            if ( useLocks() )
                return getDataLocks();
            return getDataTop();
        }

        Stats getDataLocks() {
            Stats stats;

            BSONObj out;
            if ( ! conn().simpleCommand( _db , &out , "serverStatus" ) ) {
                cout << "error: " << out << endl;
                return stats;
            }

            out = out.getOwned();

            if ( ! out["locks"].isABSONObj() ) {
                cout << "locks doesn't exist, old mongod? (<2.2?)" << endl;
                return stats;
            }

            out = out["locks"].Obj();

            BSONObjIterator i( out );
            while ( i.more() ) {
                BSONElement e = i.next();

                NSStat& s = stats[e.fieldName()];
                s.ns = e.fieldName();

                BSONObj temp = e.Obj()["timeLocked"].Obj();
                s.read = ( temp["r"].numberLong() + temp["R"].numberLong() ) / 1000;
                s.write = ( temp["w"].numberLong() + temp["W"].numberLong() ) / 1000;
            }

            return stats;
        }

        Stats getDataTop() {
            Stats stats;

            BSONObj out;
            if ( ! conn().simpleCommand( _db , &out , "top" ) ) {
                cout << "error: " << out << endl;
                return stats;
            }

            if ( ! out["totals"].isABSONObj() ) {
                cout << "error: invalid top\n" << out << endl;
                return stats;
            }

            out = out.getOwned();
            out = out["totals"].Obj();

            BSONObjIterator i( out );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( ! e.isABSONObj() )
                    continue;

                NSStat& s = stats[e.fieldName()];
                s.ns = e.fieldName();
                s.read = e.Obj()["readLock"].Obj()["time"].numberLong() / 1000;
                s.write = e.Obj()["writeLock"].Obj()["time"].numberLong() / 1000;
            }

            return stats;
        }
        
        void printDiff( const Stats& prev , const Stats& now ) {
            if ( prev.size() == 0 || now.size() == 0 ) {
                cout << "." << endl;
                return;
            }
            
            vector<Diff> data;
            
            unsigned longest = 30;

            for ( Stats::const_iterator i=now.begin() ; i != now.end(); ++i ) {
                const string& ns = i->first;
                const NSStat& b = i->second;
                if ( prev.find( ns ) == prev.end() )
                    continue;
                const NSStat& a = prev.find( ns )->second;
                
                // invalid, data fixed in 1.8.0
                if ( ns[0] == '?' )
                    continue;
                
                if ( ! useLocks() && ns.find( '.' ) == string::npos )
                    continue;
                
                if ( ns.size() > longest )
                    longest = ns.size();

                data.push_back( Diff( a , b ) );
            }
            
            std::sort( data.begin() , data.end() );

            int numberWidth = 10;

            cout << "\n"
                 << setw(longest) << "ns"
                 << setw(numberWidth+2) << "total"
                 << setw(numberWidth+2) << "read"
                 << setw(numberWidth+2) << "write"
                 << "\t\t" << terseCurrentTime()
                 << endl;
            for ( int i=data.size()-1; i>=0 && data.size() - i < 10 ; i-- ) {
                cout << setw(longest) << data[i].ns 
                     << setw(numberWidth) << setprecision(3) << data[i].total() << "ms"
                     << setw(numberWidth) << setprecision(3) << data[i].read << "ms"
                     << setw(numberWidth) << setprecision(3) << data[i].write << "ms"
                     << endl;
            }

        }

        int run() {
            _sleep = getParam( "sleep" , _sleep );

            auth();
            
            Stats prev = getData();

            while ( true ) {
                sleepsecs( _sleep );
                
                Stats now;
                try {
                    now = getData();
                }
                catch ( std::exception& e ) {
                    cout << "can't get data: " << e.what() << endl;
                    continue;
                }

                if ( now.size() == 0 )
                    return -2;
                
                try {
                    printDiff( prev , now );
                }
                catch ( AssertionException& e ) {
                    cout << "\nerror: " << e.what() << endl;
                }

                prev = now;
            }

            return 0;
        }

    private:
        int _sleep;
    };

}

int main( int argc , char ** argv ) {
    mongo::TopTool top;
    return top.main( argc , argv );
}

