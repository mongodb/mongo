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

#include <boost/thread/thread.hpp>
#include <fstream>
#include <iostream>

#include "mongo/base/init.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/jsobjmanipulator.h"
#include "mongo/db/json.h"
#include "mongo/s/type_shard.h"
#include "mongo/tools/stat_util.h"
#include "mongo/tools/tool.h"
#include "mongo/tools/tool_options.h"
#include "mongo/util/net/httpclient.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/text.h"

namespace mongo {
    MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
                              MONGO_NO_PREREQUISITES,
                              ("default"))(InitializerContext* context) {

        options = moe::OptionSection( "options" );
        moe::OptionsParser parser;

        Status retStatus = addMongoStatOptions(&options);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = parser.run(options, context->args(), context->env(), &_params);
        if (!retStatus.isOK()) {
            std::ostringstream oss;
            oss << retStatus.toString() << "\n";
            printMongoStatHelp(options, &oss);
            return Status(ErrorCodes::FailedToParse, oss.str());
        }

        return Status::OK();
    }
} // namespace mongo

namespace mongo {

    class Stat : public Tool {
    public:

        Stat() : Tool( "stat" , "admin" ) {
            _http = false;
            _many = false;
            _autoreconnect = true;
        }

        virtual void printHelp( ostream & out ) {
            printMongoStatHelp(options, &out);
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
            _statUtil.setSeconds( getParam( "sleep" , 1 ) );
            _statUtil.setAll( hasParam( "all" ) );
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

            BSONObj prev = stats();
            if ( prev.isEmpty() )
                return -1;

            int maxLockedDbWidth = 0;

            while ( rowCount == 0 || rowNum < rowCount ) {
                sleepsecs((int)ceil(_statUtil.getSeconds()));
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

                    BSONObj out = _statUtil.doRow( prev , now );

                    // adjust width up as longer 'locked db' values appear
                    setMaxLockedDbWidth( &out, &maxLockedDbWidth ); 
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

        /* Get the size of the 'locked db' field from a row of stats. If 
         * smaller than the current column width, set to the max.  If
         * greater, set the maxWidth to that value.
         */
        void setMaxLockedDbWidth( const BSONObj* o, int* maxWidth ) {
            BSONElement e = o->getField("locked db");
            if ( e.isABSONObj() ) {
                BSONObj x = e.Obj();
                if ( x["width"].numberInt() < *maxWidth ) {
                    BSONElementManipulator manip( x["width"] );
                    manip.setNumber( *maxWidth );
                }
                else {
                    *maxWidth = x["width"].numberInt();
                }
            }
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

            BSONObj authParams;
        };

        static void serverThread( shared_ptr<ServerState> state , int sleepTime) {
            try {
                DBClientConnection conn( true );
                conn._logLevel = logger::LogSeverity::Debug(1);
                string errmsg;
                if ( ! conn.connect( state->host , errmsg ) )
                    state->error = errmsg;
                long long cycleNumber = 0;

                if (! (state->authParams["user"].str().empty()) )
                    conn.auth(state->authParams);

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
                            str::stream errorStream;
                            errorStream << "serverStatus failed";
                            BSONElement errorField = out["errmsg"];
                            if (errorField.type() == String)
                                errorStream << ": " << errorField.str();
                            scoped_lock lk( state->lock );
                            state->error = errorStream;
                            state->lastUpdate = time(0);
                        }

                        if ( out["shardCursorType"].type() == Object ||
                             out["process"].str() == "mongos" ) {
                            state->mongos = true;
                            if ( cycleNumber % 10 == 1 ) {
                                auto_ptr<DBClientCursor> c = conn.query( ShardType::ConfigNS , BSONObj() );
                                vector<BSONObj> shards;
                                while ( c->more() ) {
                                    shards.push_back( c->nextSafe().getOwned() );
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

                    sleepsecs( sleepTime );
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
            /* For each new thread, pass in a thread state object and the delta between samples */
            state->thr.reset( new boost::thread( boost::bind( serverThread,
                                                              state,
                                                              (int)ceil(_statUtil.getSeconds()) ) ) );
            state->authParams = BSON( "user" << _username <<
                                      "pwd" << _password <<
                                      "userSource" << getAuthenticationDatabase() <<
                                      "mechanism" << _authenticationMechanism );
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
                bool showPorts = false;
                if ( orig == "" )
                    orig = "localhost";

                if ( orig.find( ":" ) != string::npos || hasParam( "port" ) )
                    showPorts = true;

                StringSplitter ss( orig.c_str() , "," );
                while ( ss.more() ) {
                    string host = ss.next();
                    if ( showPorts && host.find( ":" ) == string::npos) {
                        // port supplied, but not for this host.  use default.
                        if ( hasParam( "port" ) )
                            host += ":" + _params["port"].as<string>();
                        else
                            host += ":27017";
                    }
                    _add( threads , host );
                }
            }

            sleepsecs(1);

            int row = 0;
            bool discover = hasParam( "discover" );
            bool showHeaders = ! hasParam( "noheaders" );
            int maxLockedDbWidth = 0;

            while ( 1 ) {
                sleepsecs( (int)ceil(_statUtil.getSeconds()) );

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
                        BSONObj out = _statUtil.doRow( i->second->prev , i->second->now );
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

                    // adjust width up as longer 'locked db' values appear
                    setMaxLockedDbWidth( &rows[i].data, &maxLockedDbWidth ); 
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
                if ( row++ % 5 == 0 && showHeaders && ! biggest.isEmpty() ) {
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

        StatUtil _statUtil;
        bool _http;
        bool _many;

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
    REGISTER_MONGO_TOOL(Stat);
}
