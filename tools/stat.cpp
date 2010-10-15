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
#include "../util/text.h"

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
            _http = false;
            _many = false;
            
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
            out << "   q r|w     \t- ops waiting for lock from db.currentOp() (read|write)\n";
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

        template<typename T>
        void _append( BSONObjBuilder& result , const string& name , unsigned width , const T& t ){
            if ( name.size() > width )
                width = name.size();
            result.append( name , BSON( "width" << (int)width << "data" << t ) );
        }

        /**
         * BSON( <field> -> BSON( width : ### , data : XXX ) )
         */
        BSONObj doRow( const BSONObj& a , const BSONObj& b ){
            BSONObjBuilder result;

            if ( b["opcounters"].type() == Object ){
                BSONObj ax = a["opcounters"].embeddedObject();
                BSONObj bx = b["opcounters"].embeddedObject();
                BSONObjIterator i( bx );
                while ( i.more() ){
                    BSONElement e = i.next();
                    _append( result , (string)(e.fieldName()) + "/s" , 6 , (int)diff( e.fieldName() , ax , bx ) );
                }
            }
            
            if ( b["backgroundFlushing"].type() == Object ){
                BSONObj ax = a["backgroundFlushing"].embeddedObject();
                BSONObj bx = b["backgroundFlushing"].embeddedObject();
                _append( result , "flushes/s" , 6 , (int)diff( "flushes" , ax , bx ) );
            }

            if ( b.getFieldDotted("mem.supported").trueValue() ){
                BSONObj bx = b["mem"].embeddedObject();
                BSONObjIterator i( bx );
                _append( result , "mapped" , 6 , bx["mapped"].numberInt() );
                _append( result , "vsize" , 6 , bx["virtual"].numberInt() );
                _append( result , "res" , 6 , bx["resident"].numberInt() );
            }

            if ( b["extra_info"].type() == Object ){
                BSONObj ax = a["extra_info"].embeddedObject();
                BSONObj bx = b["extra_info"].embeddedObject();
                if ( ax["page_faults"].type() || ax["page_faults"].type() )
                    _append( result , "faults/s" , 6 , (int)diff( "page_faults" , ax , bx ) );
            }
            
            _append( result , "locked %" , 8 , percent( "globalLock.totalTime" , "globalLock.lockTime" , a , b ) );
            _append( result , "idx miss %" , 8 , percent( "indexCounters.btree.accesses" , "indexCounters.btree.misses" , a , b ) );

            if ( b.getFieldDotted( "globalLock.currentQueue" ).type() == Object ){
                int r = b.getFieldDotted( "globalLock.currentQueue.readers" ).numberInt();
                int w = b.getFieldDotted( "globalLock.currentQueue.writers" ).numberInt();
                stringstream temp;
                temp << r << "|" << w;
                _append( result , "qr|qw" , 9 , temp.str() );
            }
            _append( result , "conn" , 5 , b.getFieldDotted( "connections.current" ).numberInt() );

            if ( b["repl"].type() == Object ){

                BSONObj x = b["repl"].embeddedObject();
                bool isReplSet = x["setName"].type() == String;

                stringstream ss;

                if ( isReplSet ){
                    string setName = x["setName"].String();
                    _append( result , "set" , setName.size() , setName );
                }

                if ( x["ismaster"].trueValue() )
                    ss << "M";
                else if ( x["secondary"].trueValue() )
                    ss << "SEC";
                else if ( isReplSet )
                    ss << "UNK";
                else
                    ss << "SLV";
                
                _append( result , "repl" , 4 , ss.str() );
                
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
        
        virtual void preSetup(){
            if ( hasParam( "http" ) ){
                _http = true;
                _noconnection = true;
            }

            if ( hasParam( "host" ) && 
                 getParam( "host" ).find( ',' ) != string::npos ){
                _noconnection = true;
                _many = true;
            }
        }

        int run(){ 
            _sleep = getParam( "sleep" , _sleep );
            if ( _many )
                return runMany();
            return runNormal();
        }

        static void printHeaders( const BSONObj& o ){
            BSONObjIterator i(o);
            while ( i.more() ){
                BSONElement e = i.next();
                BSONObj x = e.Obj();
                cout << setw( x["width"].numberInt() ) << e.fieldName() << ' ';
            }
            cout << endl;            
        }

        static void printData( const BSONObj& o ){
            BSONObjIterator i(o);
            while ( i.more() ){
                BSONObj x = i.next().Obj();
                int w = x["width"].numberInt();
                
                BSONElement data = x["data"];
                
                if ( data.type() == String )
                    cout << setw(w) << data.String();
                else if ( data.type() == NumberDouble )
                    cout << setw(w) << setprecision(3) << data.number();
                else if ( data.type() == NumberInt )
                    cout << setw(w) << data.numberInt();
                
                cout << ' ';
            }
            cout << endl; 
        }

        int runNormal(){
            bool showHeaders = ! hasParam( "noheaders" );
            int rowCount = getParam( "rowcount" , 0 );
            int rowNum = 0;

            BSONObj prev = stats();
            if ( prev.isEmpty() )
                return -1;

            while ( rowCount == 0 || rowNum < rowCount ){
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

                    BSONObj out = doRow( prev , now );

                    if ( showHeaders && rowNum % 10 == 0 ){
                        printHeaders( out );
                    }
                    
                    printData( out );

                }
                catch ( AssertionException& e ){
                    cout << "\nerror: " << e.what() << "\n"
                         << now
                         << endl;
                }
                
                prev = now;
                rowNum++;
            }
            return 0;
        }
        
        struct ServerState {
            ServerState() : lock( "Stat::ServerState" ){}
            string host;
            scoped_ptr<boost::thread> thr;
            
            mongo::mutex lock;

            BSONObj prev;
            BSONObj now;
            time_t lastUpdate;

            string error;
        };
            
        static void serverThread( shared_ptr<ServerState> state ){
            try {
                DBClientConnection conn( true );
                string errmsg;
                if ( ! conn.connect( state->host , errmsg ) )
                    state->error = errmsg;

                while ( 1 ){
                    try {
                        BSONObj out;
                        if ( conn.simpleCommand( "admin" , &out , "serverStatus" ) ){
                            scoped_lock lk( state->lock );
                            state->error = "";
                            state->lastUpdate = time(0);
                            state->prev = state->now;
                            state->now = out.getOwned();
                        }
                        else {
                            scoped_lock lk( state->lock );
                            state->error = "serverStatus failed";
                            state->lastUpdate = time(0);
                        }
                        
                    }
                    catch ( std::exception& e ){
                        scoped_lock lk( state->lock );
                        state->error = e.what();
                    }
                    
                    sleepsecs( 1 );
                }
                
                
            }
            catch ( std::exception& e ){
                cout << "serverThread (" << state->host << ") fatal error : " << e.what() << endl;
            }
            catch ( ... ){
                cout << "serverThread (" << state->host << ") fatal error" << endl;
            }
        }

        int runMany(){
            map<string,shared_ptr<ServerState> > threads;
            
            unsigned longestHost = 0;

            {
                string orig = getParam( "host" );
                StringSplitter ss( orig.c_str() , "," );
                while ( ss.more() ){
                    string host = ss.next();
                    if ( host.size() > longestHost )
                        longestHost = host.size();

                    shared_ptr<ServerState>& state = threads[host];
                    if ( state )
                        continue;

                    state.reset( new ServerState() );
                    state->host = host;
                    state->thr.reset( new boost::thread( boost::bind( serverThread , state ) ) );
                }
            }
            
            sleepsecs(1);

            int row = 0;

            while ( 1 ){
                sleepsecs( _sleep );
                
                cout << endl;
                int x = 0;
                for ( map<string,shared_ptr<ServerState> >::iterator i=threads.begin(); i!=threads.end(); ++i ){
                    scoped_lock lk( i->second->lock );

                    if ( i->second->error.size() ){
                        cout << setw( longestHost ) << i->first << "\t";
                        cout << i->second->error << endl;
                    }
                    else if ( i->second->prev.isEmpty() || i->second->now.isEmpty() ){
                        cout << setw( longestHost ) << i->first << "\t";
                        cout << "no data" << endl;
                    }
                    else {
                        BSONObj out = doRow( i->second->prev , i->second->now );
                        
                        if ( x++ == 0 && row++ % 5 == 0 ){
                            cout << setw( longestHost ) << "" << "\t";
                            printHeaders( out );
                        }

                        cout << setw( longestHost ) << i->first << "\t";
                        printData( out );
                    }
                }
            }

            return 0;
        }

        int _sleep;
        bool _http;
        bool _many;
    };

}

int main( int argc , char ** argv ) {
    mongo::Stat stat;
    return stat.main( argc , argv );
}

