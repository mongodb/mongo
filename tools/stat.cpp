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
#include "../util/net/httpclient.h"
#include "../util/text.h"
#include "tool.h"
#include <fstream>
#include <iostream>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace mongo {

    class Stat : public Tool {
    public:

        Stat() : Tool( "stat" , REMOTE_SERVER , "admin" ) {
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
            ("discover" , "discover nodes and display stats for all" )
            ("all" , "all optional fields" )
            ;

            addPositionArg( "sleep" , 1 );

            _autoreconnect = true;
        }

        virtual void printExtraHelp( ostream & out ) {
            out << "View live MongoDB performance statistics.\n" << endl;
            out << "usage: " << _name << " [options] [sleep time]" << endl;
            out << "sleep time: time to wait (in seconds) between calls" << endl;
        }

        virtual void printExtraHelpAfter( ostream & out ) {
            out << "\n";
            out << " Fields\n";
            out << "   inserts  \t- # of inserts per second (* means replicated op)\n";
            out << "   query    \t- # of queries per second\n";
            out << "   update   \t- # of updates per second\n";
            out << "   delete   \t- # of deletes per second\n";
            out << "   getmore  \t- # of get mores (cursor batch) per second\n";
            out << "   command  \t- # of commands per second, on a slave its local|replicated\n";
            out << "   flushes  \t- # of fsync flushes per second\n";
            out << "   mapped   \t- amount of data mmaped (total data size) megabytes\n";
            out << "   vsize    \t- virtual size of process in megabytes\n";
            out << "   res      \t- resident size of process in megabytes\n";
            out << "   faults   \t- # of pages faults per sec (linux only)\n";
            out << "   locked   \t- percent of time in global write lock\n";
            out << "   idx miss \t- percent of btree page misses (sampled)\n";
            out << "   qr|qw    \t- queue lengths for clients waiting (read|write)\n";
            out << "   ar|aw    \t- active clients (read|write)\n";
            out << "   netIn    \t- network traffic in - bits\n";
            out << "   netOut   \t- network traffic out - bits\n";
            out << "   conn     \t- number of open connections\n";
            out << "   set      \t- replica set name\n";
            out << "   repl     \t- replication type \n";
            out << "            \t    PRI - primary (master)\n";
            out << "            \t    SEC - secondary\n";
            out << "            \t    REC - recovering\n";
            out << "            \t    UNK - unknown\n";
            out << "            \t    SLV - slave\n";
            out << "            \t    RTR - mongos process (\"router\")\n";
        }

        BSONObj stats() {
            if ( _http ) {
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

                if ( c.get( url , &r ) != 200 ) {
                    cout << "error (http): " << r.getEntireResponse() << endl;
                    return BSONObj();
                }

                BSONObj x = fromjson( r.getBody() );
                BSONElement e = x["serverStatus"];
                if ( e.type() != Object ) {
                    cout << "BROKEN: " << x << endl;
                    return BSONObj();
                }
                return e.embeddedObjectUserCheck();
            }
            BSONObj out;
            if ( ! conn().simpleCommand( _db , &out , "serverStatus" ) ) {
                cout << "error: " << out << endl;
                return BSONObj();
            }
            return out.getOwned();
        }

        double diff( const string& name , const BSONObj& a , const BSONObj& b ) {
            BSONElement x = a.getFieldDotted( name.c_str() );
            BSONElement y = b.getFieldDotted( name.c_str() );
            if ( ! x.isNumber() || ! y.isNumber() )
                return -1;
            return ( y.number() - x.number() ) / _sleep;
        }

        double percent( const char * outof , const char * val , const BSONObj& a , const BSONObj& b ) {
            double x = ( b.getFieldDotted( val ).number() - a.getFieldDotted( val ).number() );
            double y = ( b.getFieldDotted( outof ).number() - a.getFieldDotted( outof ).number() );
            if ( y == 0 )
                return 0;
            double p = x / y;
            p = (double)((int)(p * 1000)) / 10;
            return p;
        }

        template<typename T>
        void _append( BSONObjBuilder& result , const string& name , unsigned width , const T& t ) {
            if ( name.size() > width )
                width = name.size();
            result.append( name , BSON( "width" << (int)width << "data" << t ) );
        }

        void _appendMem( BSONObjBuilder& result , const string& name , unsigned width , double sz ) {
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

        void _appendNet( BSONObjBuilder& result , const string& name , double diff ) {
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

        /**
         * BSON( <field> -> BSON( width : ### , data : XXX ) )
         */
        BSONObj doRow( const BSONObj& a , const BSONObj& b ) {
            BSONObjBuilder result;

            bool isMongos =  b["shardCursorType"].type() == Object; // TODO: should have a better check

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
                if ( ax["page_faults"].type() || ax["page_faults"].type() )
                    _append( result , "faults" , 6 , (int)diff( "page_faults" , ax , bx ) );
            }

            if (!isMongos) {
                _append( result , "locked %" , 8 , percent( "globalLock.totalTime" , "globalLock.lockTime" , a , b ) );
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

        virtual void preSetup() {
            if ( hasParam( "http" ) ) {
                _http = true;
                _noconnection = true;
            }

            if ( hasParam( "host" ) &&
                    getParam( "host" ).find( ',' ) != string::npos ) {
                _noconnection = true;
                _many = true;
            }

            if ( hasParam( "discover" ) ) {
                _many = true;
            }
        }

        int run() {
            _sleep = getParam( "sleep" , _sleep );
            _all = hasParam( "all" );
            if ( _many )
                return runMany();
            return runNormal();
        }

        static void printHeaders( const BSONObj& o ) {
            BSONObjIterator i(o);
            while ( i.more() ) {
                BSONElement e = i.next();
                BSONObj x = e.Obj();
                cout << setw( x["width"].numberInt() ) << e.fieldName() << ' ';
            }
            cout << endl;
        }

        static void printData( const BSONObj& o , const BSONObj& headers ) {

            BSONObjIterator i(headers);
            while ( i.more() ) {
                BSONElement e = i.next();
                BSONObj h = e.Obj();
                int w = h["width"].numberInt();

                BSONElement data;
                {
                    BSONElement temp = o[e.fieldName()];
                    if ( temp.isABSONObj() )
                        data = temp.Obj()["data"];
                }

                if ( data.type() == String )
                    cout << setw(w) << data.String();
                else if ( data.type() == NumberDouble )
                    cout << setw(w) << setprecision(3) << data.number();
                else if ( data.type() == NumberInt )
                    cout << setw(w) << data.numberInt();
                else if ( data.eoo() )
                    cout << setw(w) << "";
                else
                    cout << setw(w) << "???";

                cout << ' ';
            }
            cout << endl;
        }

        int runNormal() {
            bool showHeaders = ! hasParam( "noheaders" );
            int rowCount = getParam( "rowcount" , 0 );
            int rowNum = 0;

            auth();

            BSONObj prev = stats();
            if ( prev.isEmpty() )
                return -1;

            while ( rowCount == 0 || rowNum < rowCount ) {
                sleepsecs(_sleep);
                BSONObj now;
                try {
                    now = stats();
                }
                catch ( std::exception& e ) {
                    cout << "can't get data: " << e.what() << endl;
                    continue;
                }

                if ( now.isEmpty() )
                    return -2;

                try {

                    BSONObj out = doRow( prev , now );

                    if ( showHeaders && rowNum % 10 == 0 ) {
                        printHeaders( out );
                    }

                    printData( out , out );

                }
                catch ( AssertionException& e ) {
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
            ServerState() : lock( "Stat::ServerState" ) {}
            string host;
            scoped_ptr<boost::thread> thr;

            mongo::mutex lock;

            BSONObj prev;
            BSONObj now;
            time_t lastUpdate;
            vector<BSONObj> shards;

            string error;
            bool mongos;

            string username;
            string password;
        };

        static void serverThread( shared_ptr<ServerState> state ) {
            try {
                DBClientConnection conn( true );
                conn._logLevel = 1;
                string errmsg;
                if ( ! conn.connect( state->host , errmsg ) )
                    state->error = errmsg;
                long long cycleNumber = 0;
                
                conn.auth("admin", state->username, state->password, errmsg);

                while ( ++cycleNumber ) {
                    try {
                        BSONObj out;
                        if ( conn.simpleCommand( "admin" , &out , "serverStatus" ) ) {
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

                        if ( out["shardCursorType"].type() == Object ) {
                            state->mongos = true;
                            if ( cycleNumber % 10 == 1 ) {
                                auto_ptr<DBClientCursor> c = conn.query( "config.shards" , BSONObj() );
                                vector<BSONObj> shards;
                                while ( c->more() ) {
                                    shards.push_back( c->next().getOwned() );
                                }
                                scoped_lock lk( state->lock );
                                state->shards = shards;
                            }
                        }
                    }
                    catch ( std::exception& e ) {
                        scoped_lock lk( state->lock );
                        state->error = e.what();
                    }

                    sleepsecs( 1 );
                }


            }
            catch ( std::exception& e ) {
                cout << "serverThread (" << state->host << ") fatal error : " << e.what() << endl;
            }
            catch ( ... ) {
                cout << "serverThread (" << state->host << ") fatal error" << endl;
            }
        }

        typedef map<string,shared_ptr<ServerState> >  StateMap;

        bool _add( StateMap& threads , string host ) {
            shared_ptr<ServerState>& state = threads[host];
            if ( state )
                return false;

            state.reset( new ServerState() );
            state->host = host;
            state->thr.reset( new boost::thread( boost::bind( serverThread , state ) ) );
            state->username = _username;
            state->password = _password;

            return true;
        }

        /**
         * @param hosts [ "a.foo.com" , "b.foo.com" ]
         */
        bool _addAll( StateMap& threads , const BSONObj& hosts ) {
            BSONObjIterator i( hosts );
            bool added = false;
            while ( i.more() ) {
                bool me = _add( threads , i.next().String() );
                added = added || me;
            }
            return added;
        }

        bool _discover( StateMap& threads , const string& host , const shared_ptr<ServerState>& ss ) {

            BSONObj info = ss->now;

            bool found = false;

            if ( info["repl"].isABSONObj() ) {
                BSONObj x = info["repl"].Obj();
                if ( x["hosts"].isABSONObj() )
                    if ( _addAll( threads , x["hosts"].Obj() ) )
                        found = true;
                if ( x["passives"].isABSONObj() )
                    if ( _addAll( threads , x["passives"].Obj() ) )
                        found = true;
            }

            if ( ss->mongos ) {
                for ( unsigned i=0; i<ss->shards.size(); i++ ) {
                    BSONObj x = ss->shards[i];

                    string errmsg;
                    ConnectionString cs = ConnectionString::parse( x["host"].String() , errmsg );
                    if ( errmsg.size() ) {
                        cerr << errmsg << endl;
                        continue;
                    }

                    vector<HostAndPort> v = cs.getServers();
                    for ( unsigned i=0; i<v.size(); i++ ) {
                        if ( _add( threads , v[i].toString() ) )
                            found = true;
                    }
                }
            }

            return found;
        }

        int runMany() {
            StateMap threads;
            
            {
                string orig = getParam( "host" );
                if ( orig == "" )
                    orig = "localhost";
                
                if ( orig.find( ":" ) == string::npos ) {
                    if ( hasParam( "port" ) )
                        orig += ":" + _params["port"].as<string>();
                    else 
                        orig += ":27017";
                }
                
                StringSplitter ss( orig.c_str() , "," );
                while ( ss.more() ) {
                    string host = ss.next();
                    _add( threads , host );
                }
            }

            sleepsecs(1);

            int row = 0;
            bool discover = hasParam( "discover" );

            while ( 1 ) {
                sleepsecs( _sleep );

                // collect data
                vector<Row> rows;
                for ( map<string,shared_ptr<ServerState> >::iterator i=threads.begin(); i!=threads.end(); ++i ) {
                    scoped_lock lk( i->second->lock );

                    if ( i->second->error.size() ) {
                        rows.push_back( Row( i->first , i->second->error ) );
                    }
                    else if ( i->second->prev.isEmpty() || i->second->now.isEmpty() ) {
                        rows.push_back( Row( i->first ) );
                    }
                    else {
                        BSONObj out = doRow( i->second->prev , i->second->now );
                        rows.push_back( Row( i->first , out ) );
                    }

                    if ( discover && ! i->second->now.isEmpty() ) {
                        if ( _discover( threads , i->first , i->second ) )
                            break;
                    }
                }

                // compute some stats
                unsigned longestHost = 0;
                BSONObj biggest;
                for ( unsigned i=0; i<rows.size(); i++ ) {
                    if ( rows[i].host.size() > longestHost )
                        longestHost = rows[i].host.size();
                    if ( rows[i].data.nFields() > biggest.nFields() )
                        biggest = rows[i].data;
                }

                {
                    // check for any headers not in biggest

                    // TODO: we put any new headers at end,
                    //       ideally we would interleave

                    set<string> seen;

                    BSONObjBuilder b;

                    {
                        // iterate biggest
                        BSONObjIterator i( biggest );
                        while ( i.more() ) {
                            BSONElement e = i.next();
                            seen.insert( e.fieldName() );
                            b.append( e );
                        }
                    }

                    // now do the rest
                    for ( unsigned j=0; j<rows.size(); j++ ) {
                        BSONObjIterator i( rows[j].data );
                        while ( i.more() ) {
                            BSONElement e = i.next();
                            if ( seen.count( e.fieldName() ) )
                                continue;
                            seen.insert( e.fieldName() );
                            b.append( e );
                        }

                    }

                    biggest = b.obj();

                }

                // display data

                cout << endl;

                //    header
                if ( row++ % 5 == 0 && ! biggest.isEmpty() ) {
                    cout << setw( longestHost ) << "" << "\t";
                    printHeaders( biggest );
                }

                //    rows
                for ( unsigned i=0; i<rows.size(); i++ ) {
                    cout << setw( longestHost ) << rows[i].host << "\t";
                    if ( rows[i].err.size() )
                        cout << rows[i].err << endl;
                    else if ( rows[i].data.isEmpty() )
                        cout << "no data" << endl;
                    else
                        printData( rows[i].data , biggest );
                }

            }

            return 0;
        }

        int _sleep;
        bool _http;
        bool _many;
        bool _all;

        struct Row {
            Row( string h , string e ) {
                host = h;
                err = e;
            }

            Row( string h ) {
                host = h;
            }

            Row( string h , BSONObj d ) {
                host = h;
                data = d;
            }
            string host;
            string err;
            BSONObj data;
        };
    };

}

int main( int argc , char ** argv ) {
    mongo::Stat stat;
    return stat.main( argc , argv );
}

