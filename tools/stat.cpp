// stat.cpp

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
#include "client/dbclient.h"
#include "db/json.h"
#include "../util/httpclient.h"

#include "tool.h"

#include <fstream>
#include <iostream>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace mongo {
    
    class Stat : public Tool {
    public:

        Stat() : Tool( "stat" , NO_LOCAL , "admin" ){
            _sleep = 1;
            _rowNum = 0;
            _showHeaders = true;
            _http = false;

            add_hidden_options()
                ( "sleep" , po::value<int>() , "time to sleep between calls" )
                ;
            add_options()
                ("noheaders", "don't output column names")
                ("rowcount,n", po::value<int>()->default_value(0), "number of stats lines to print (0 for indefinite)")
                ("http", "use http instead of raw db connection")
                ;

            addPositionArg( "sleep" , 1 );

            _autoreconnect = true;
        }

        virtual void printExtraHelp( ostream & out ){
            out << "usage: " << _name << " [options] [sleep time]" << endl;
            out << "sleep time: time to wait (in seconds) between calls" << endl;
        }

        virtual void printExtraHelpAfter( ostream & out ){
            out << "\n";
            out << " Fields\n";
            out << "   inserts/s \t- # of inserts per second\n";
            out << "   query/s   \t- # of queries per second\n";
            out << "   update/s  \t- # of updates per second\n";
            out << "   delete/s  \t- # of deletes per second\n";
            out << "   getmore/s \t- # of get mores (cursor batch) per second\n";
            out << "   command/s \t- # of commands per second\n";
            out << "   flushes/s \t- # of fsync flushes per second\n";
            out << "   mapped    \t- amount of data mmaped (total data size) megabytes\n";
            out << "   visze     \t- virtual size of process in megabytes\n";
            out << "   res       \t- resident size of process in megabytes\n";
            out << "   faults/s  \t- # of pages faults/sec (linux only)\n";
            out << "   locked    \t- percent of time in global write lock\n";
            out << "   idx miss  \t- percent of btree page misses (sampled)\n";
            out << "   q t|r|w   \t- ops waiting for lock from db.currentOp() (total|read|write)\n";
            out << "   conn      \t- number of open connections\n";
        }

        
        BSONObj stats(){
            if ( _http ){
                HttpClient c;
                HttpClient::Result r;
                
                string url;
                {
                    stringstream ss;
                    ss << "http://" << _host;
                    if ( _host.find( ":" ) == string::npos )
                        ss << ":28017";
                    ss << "/_status";
                    url = ss.str();
                }

                if ( c.get( url , &r ) != 200 ){
                    cout << "error (http): " << r.getEntireResponse() << endl;
                    return BSONObj();
                }
                
                BSONObj x = fromjson( r.getBody() );
                BSONElement e = x["serverStatus"];
                if ( e.type() != Object ){
                    cout << "BROKEN: " << x << endl;
                    return BSONObj();
                }
                return e.embeddedObjectUserCheck();
            }
            BSONObj out;
            if ( ! conn().simpleCommand( _db , &out , "serverStatus" ) ){
                cout << "error: " << out << endl;
                return BSONObj();
            }
            return out.getOwned();
        }

        double diff( const string& name , const BSONObj& a , const BSONObj& b ){
            BSONElement x = a.getFieldDotted( name.c_str() );
            BSONElement y = b.getFieldDotted( name.c_str() );
            if ( ! x.isNumber() || ! y.isNumber() )
                return -1;
            return ( y.number() - x.number() ) / _sleep;
        }
        
        double percent( const char * outof , const char * val , const BSONObj& a , const BSONObj& b ){
            double x = ( b.getFieldDotted( val ).number() - a.getFieldDotted( val ).number() );
            double y = ( b.getFieldDotted( outof ).number() - a.getFieldDotted( outof ).number() );
            if ( y == 0 )
                return 0;
            double p = x / y;
            p = (double)((int)(p * 1000)) / 10;
            return p;
        }

        void cellstart( stringstream& ss , string name , unsigned& width ){
            if ( ! _showHeaders ) {
                return;
            }
            if ( name.size() > width )
                width = name.size();
            if ( _rowNum % 20 == 0 )
                cout << setw(width) << name << " ";            
        }

        void cell( stringstream& ss , string name , unsigned width , double val ){
            cellstart( ss , name , width );
            ss << setw(width) << setprecision(3) << val << " ";
        }

        void cell( stringstream& ss , string name , unsigned width , int val ){
            cellstart( ss , name , width );
            ss << setw(width) << val << " ";
        }

        void cell( stringstream& ss , string name , unsigned width , const string& val ){
            cellstart( ss , name , width );
            ss << setw(width) << val << " ";
        }

        string doRow( const BSONObj& a , const BSONObj& b ){
            stringstream ss;

            if ( b["opcounters"].type() == Object ){
                BSONObj ax = a["opcounters"].embeddedObject();
                BSONObj bx = b["opcounters"].embeddedObject();
                BSONObjIterator i( bx );
                while ( i.more() ){
                    BSONElement e = i.next();
                    cell( ss , (string)(e.fieldName()) + "/s" , 6 , (int)diff( e.fieldName() , ax , bx ) );
                }
            }

	    if ( b["backgroundFlushing"].type() == Object ){
                BSONObj ax = a["backgroundFlushing"].embeddedObject();
                BSONObj bx = b["backgroundFlushing"].embeddedObject();
                BSONObjIterator i( bx );
                cell( ss , "flushes/s" , 6 , (int)diff( "flushes" , ax , bx ) );
            }

            if ( b.getFieldDotted("mem.supported").trueValue() ){
                BSONObj bx = b["mem"].embeddedObject();
                BSONObjIterator i( bx );
                cell( ss , "mapped" , 6 , bx["mapped"].numberInt() );
                cell( ss , "vsize" , 6 , bx["virtual"].numberInt() );
                cell( ss , "res" , 6 , bx["resident"].numberInt() );
            }

            if ( b["extra_info"].type() == Object ){
                BSONObj ax = a["extra_info"].embeddedObject();
                BSONObj bx = b["extra_info"].embeddedObject();
                if ( ax["page_faults"].type() || ax["page_faults"].type() )
                    cell( ss , "faults/s" , 6 , (int)diff( "page_faults" , ax , bx ) );
            }
            
            cell( ss , "locked %" , 8 , percent( "globalLock.totalTime" , "globalLock.lockTime" , a , b ) );
            cell( ss , "idx miss %" , 8 , percent( "indexCounters.btree.accesses" , "indexCounters.btree.misses" , a , b ) );

            if ( b.getFieldDotted( "globalLock.currentQueue" ).type() == Object ){
                int r = b.getFieldDotted( "globalLock.currentQueue.readers" ).numberInt();
                int w = b.getFieldDotted( "globalLock.currentQueue.writers" ).numberInt();
                stringstream temp;
                temp << r+w << "|" << r << "|" << w;
                cell( ss , "q t|r|w" , 10 , temp.str() );
            }
            cell( ss , "conn" , 5 , b.getFieldDotted( "connections.current" ).numberInt() );

            {
                struct tm t;
                time_t_to_Struct( time(0), &t , true );
                stringstream temp;
                temp << setfill('0') << setw(2) << t.tm_hour 
                     << ":" 
                     << setfill('0') << setw(2) << t.tm_min
                     << ":" 
                     << setfill('0') << setw(2) << t.tm_sec;
                cell( ss , "time" , 10 , temp.str() );
            }

            if ( _showHeaders && _rowNum % 20 == 0 ){
                // this is the newline after the header line
                cout << endl;
            }
            _rowNum++;

            return ss.str();
        }
        
        virtual void preSetup(){
            if ( hasParam( "http" ) ){
                _http = true;
                _noconnection = true;
            }
        }

        int run(){ 
            _sleep = getParam( "sleep" , _sleep );
            if ( hasParam( "noheaders" ) ) {
                _showHeaders = false;
            }
            _rowCount = getParam( "rowcount" , 0 );

            BSONObj prev = stats();
            if ( prev.isEmpty() )
                return -1;

            while ( _rowCount == 0 || _rowNum < _rowCount ){
                sleepsecs(_sleep);
                BSONObj now;
                try {
                    now = stats();
                }
                catch ( std::exception& e ){
                    cout << "can't get data: " << e.what() << endl;
                    continue;
                }

                if ( now.isEmpty() )
                    return -2;
                
                try {
                    cout << doRow( prev , now ) << endl;
                }
                catch ( AssertionException& e ){
                    cout << "\nerror: " << e.what() << "\n"
                         << now
                         << endl;
                }
                
                prev = now;
            }
            return 0;
        }
        

        int _sleep;
        int _rowNum;
        int _rowCount;
        bool _showHeaders;
        bool _http;
    };

}

int main( int argc , char ** argv ) {
    mongo::Stat stat;
    return stat.main( argc , argv );
}

