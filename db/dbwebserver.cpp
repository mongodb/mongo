/* dbwebserver.cpp

   This is the administrative web page displayed on port 28017.
*/

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

#include "stdafx.h"
#include "../util/miniwebserver.h"
#include "../util/md5.hpp"
#include "db.h"
#include "repl.h"
#include "replset.h"
#include "instance.h"
#include "security.h"

#include <pcrecpp.h>
#include <boost/date_time/posix_time/posix_time.hpp>
#undef assert
#define assert xassert


namespace mongo {

    extern string bind_ip;
    extern const char *replInfo;

    bool getInitialSyncCompleted();

    time_t started = time(0);

    /*
        string toString() {
            stringstream ss;
            unsigned long long dt = last - start;
            ss << dt/1000;
            ss << '\t';
            ss << timeLocked/1000 << '\t';
            if( dt )
                ss << (timeLocked*100)/dt << '%';
            return ss.str();
        }
    */

    struct Timing {
        Timing() {
            start = timeLocked = 0;
        }
        unsigned long long start, timeLocked;
    };
    Timing tlast;
    const int NStats = 32;
    string lockStats[NStats];
    unsigned q = 0;

    void statsThread() {
        Client::initThread("stats");
        unsigned long long timeLastPass = 0;
        while ( 1 ) {
            {
                Timer lktm;
                dblock lk;
                Top::completeSnapshot();
                q = (q+1)%NStats;
                Timing timing;
                dbMutexInfo.timingInfo(timing.start, timing.timeLocked);
                unsigned long long now = curTimeMicros64();
                if ( timeLastPass ) {
                    unsigned long long dt = now - timeLastPass;
                    unsigned long long dlocked = timing.timeLocked - tlast.timeLocked;
                    {
                        stringstream ss;
                        ss << dt / 1000 << '\t';
                        ss << dlocked / 1000 << '\t';
                        if ( dt )
                            ss << (dlocked*100)/dt << '%';
                        string s = ss.str();
                        if ( cmdLine.cpu )
                            log() << "cpu: " << s << endl;
                        lockStats[q] = s;
                        ClientCursor::idleTimeReport( (unsigned) ((dt - dlocked)/1000) );
                    }
                }
                timeLastPass = now;
                tlast = timing;
            }
            sleepsecs(4);
        }
    }

    bool _bold;
    string bold(bool x) {
        _bold = x;
        return x ? "<b>" : "";
    }
    string bold() {
        return _bold ? "</b>" : "";
    }

    class DbWebServer : public MiniWebServer {
    public:
        // caller locks
        void doLockedStuff(stringstream& ss) {
            ss << "# databases: " << databases.size() << '\n';
            if ( cc().database() ) {
                ss << "curclient: " << cc().database()->name;
                ss << '\n';
            }
            ss << bold(ClientCursor::byLocSize()>10000) << "Cursors byLoc.size(): " << ClientCursor::byLocSize() << bold() << '\n';
            ss << "\n<b>replication</b>\n";
            ss << "master: " << master << '\n';
            ss << "slave:  " << slave << '\n';
            if ( replPair ) {
                ss << "replpair:\n";
                ss << replPair->getInfo();
            }
            bool seemCaughtUp = getInitialSyncCompleted();
            if ( !seemCaughtUp ) ss << "<b>";
            ss <<   "initialSyncCompleted: " << seemCaughtUp;
            if ( !seemCaughtUp ) ss << "</b>";
            ss << '\n';

            ss << "\n<b>DBTOP</b>\n";
            ss << "<table border=1><tr align='left'><th>Namespace</th><th>%</th><th>Reads</th><th>Writes</th><th>Calls</th><th>Time</th>";
            vector< Top::Usage > usage;
            Top::usage( usage );
            for( vector< Top::Usage >::iterator i = usage.begin(); i != usage.end(); ++i )
                ss << setprecision( 2 ) << fixed << "<tr><td>" << i->ns << "</td><td>" << i->pct << "</td><td>"
                   << i->reads << "</td><td>" << i->writes << "</td><td>" << i->calls << "</td><td>" << i->time << "</td></tr>\n";
            ss << "</table>";
            
            ss << "\n<b>dt\ttlocked</b>\n";
            unsigned i = q;
            while ( 1 ) {
                ss << lockStats[i] << '\n';
                i = (i-1)%NStats;
                if ( i == q )
                    break;
            }
        }

        void doUnlockedStuff(stringstream& ss) {
            /* this is in the header already ss << "port:      " << port << '\n'; */
            ss << mongodVersion() << "\n";
            ss << "git hash: " << gitVersion() << "\n";
            ss << "sys info: " << sysInfo() << "\n";
            ss << "\n";
            ss << "dblocked:  " << dbMutexInfo.isLocked() << " (initial)\n";
            ss << "uptime:    " << time(0)-started << " seconds\n";
            if ( replAllDead )
                ss << "<b>replication replAllDead=" << replAllDead << "</b>\n";
            ss << "\nassertions:\n";
            for ( int i = 0; i < 4; i++ ) {
                if ( lastAssert[i].isSet() ) {
                    ss << "<b>";
                    if ( i == 3 ) ss << "usererr";
                    else ss << i;
                    ss << "</b>" << ' ' << lastAssert[i].toString();
                }
            }

            ss << "\nreplInfo:  " << replInfo << '\n';

            {
                boostlock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) { 
                    Client *c = *i;
                    CurOp& co = *(c->curop());
                    ss << "currentOp (unlocked): " << co.infoNoauth() << "\n";
                }
            }
        }
        
        bool allowed( const char * rq , vector<string>& headers, const SockAddr &from ){
            
            if ( from.localhost() )
                return true;
            
            if ( db.findOne( "admin.system.users" , BSONObj() ).isEmpty() )
                return true;
            
            string auth = getHeader( rq , "Authorization" );

            if ( auth.size() > 0 && auth.find( "Digest " ) == 0 ){
                auth = auth.substr( 7 ) + ", ";

                map<string,string> parms;
                pcrecpp::StringPiece input( auth );
                
                string name, val;
                pcrecpp::RE re("(\\w+)=\"?(.*?)\"?, ");
                while ( re.Consume( &input, &name, &val) ){
                    parms[name] = val;
                }

                BSONObj user = db.findOne( "admin.system.users" , BSON( "user" << parms["username"] ) );
                if ( ! user.isEmpty() ){
                    string ha1 = user["pwd"].str();
                    string ha2 = md5simpledigest( (string)"GET" + ":" + parms["uri"] );
                    
                    string r = ha1 + ":" + parms["nonce"];
                    if ( parms["nc"].size() && parms["cnonce"].size() && parms["qop"].size() ){
                        r += ":";
                        r += parms["nc"];
                        r += ":";
                        r += parms["cnonce"];
                        r += ":";
                        r += parms["qop"];
                    }
                    r += ":";
                    r += ha2;
                    r = md5simpledigest( r );
                    
                    if ( r == parms["response"] )
                        return true;
                }

                
            }
            
            stringstream authHeader;
            authHeader 
                << "WWW-Authenticate: "
                << "Digest realm=\"mongo\", "
                << "nonce=\"abc\", " 
                << "algorithm=MD5, qop=\"auth\" "
                ;
            
            headers.push_back( authHeader.str() );
            return 0;
        }

        virtual void doRequest(
            const char *rq, // the full request
            string url,
            // set these and return them:
            string& responseMsg,
            int& responseCode,
            vector<string>& headers, // if completely empty, content-type: text/html will be added
            const SockAddr &from
        )
        {
            //out() << "url [" << url << "]" << endl;
            
            if ( url.size() > 1 ) {
                if ( ! allowed( rq , headers, from ) ){
                    responseCode = 401;
                    responseMsg = "not allowed\n";
                    return;
                }                
                handleRESTRequest( rq , url , responseMsg , responseCode , headers );
                return;
            }


            responseCode = 200;
            stringstream ss;
            ss << "<html><head><title>";

            string dbname;
            {
                stringstream z;
                z << "mongodb " << getHostName() << ':' << mongo::cmdLine.port << ' ';
                dbname = z.str();
            }
            ss << dbname << "</title></head><body><h2>" << dbname << "</h2><p>\n<pre>";

            doUnlockedStuff(ss);

            int n = 2000;
            Timer t;
            while ( 1 ) {
                if ( !dbMutexInfo.isLocked() ) {
                    {
                        dblock lk;
                        ss << "time to get dblock: " << t.millis() << "ms\n";
                        doLockedStuff(ss);
                    }
                    break;
                }
                sleepmillis(1);
                if ( --n < 0 ) {
                    ss << "\n<b>timed out getting dblock</b>\n";
                    break;
                }
            }

            ss << "</pre></body></html>";
            responseMsg = ss.str();

            // we want to return SavedContext from before the authentication was performed
            if ( ! allowed( rq , headers, from ) ){
                responseCode = 401;
                responseMsg = "not allowed\n";
                return;
            }            
        }

        void handleRESTRequest( const char *rq, // the full request
                                string url,
                                string& responseMsg,
                                int& responseCode,
                                vector<string>& headers // if completely empty, content-type: text/html will be added
                              ) {

            string::size_type first = url.find( "/" , 1 );
            if ( first == string::npos ) {
                responseCode = 400;
                return;
            }

            string method = parseMethod( rq );
            string dbname = url.substr( 1 , first - 1 );
            string coll = url.substr( first + 1 );
            string action = "";

            map<string,string> params;
            if ( coll.find( "?" ) != string::npos ) {
                parseParams( params , coll.substr( coll.find( "?" ) + 1 ) );
                coll = coll.substr( 0 , coll.find( "?" ) );
            }

            string::size_type last = coll.find_last_of( "/" );
            if ( last == string::npos ) {
                action = coll;
                coll = "_defaultCollection";
            }
            else {
                action = coll.substr( last + 1 );
                coll = coll.substr( 0 , last );
            }

            for ( string::size_type i=0; i<coll.size(); i++ )
                if ( coll[i] == '/' )
                    coll[i] = '.';

            string fullns = dbname + "." + coll;

            headers.push_back( (string)"x-action: " + action );
            headers.push_back( (string)"x-ns: " + fullns );
            headers.push_back( "Content-Type: text/plain;charset=utf-8" );

            stringstream ss;

            if ( method == "GET" ) {
                responseCode = 200;
                handleRESTQuery( fullns , action , params , responseCode , ss  );
            }
            else if ( method == "POST" ) {
                responseCode = 201;
                handlePost( fullns , body( rq ) , params , responseCode , ss  );
            }
            else {
                responseCode = 400;
                headers.push_back( "X_err: bad request" );
                ss << "don't know how to handle a [" << method << "]";
                out() << "don't know how to handle a [" << method << "]" << endl;
            }

            responseMsg = ss.str();
        }

        void handleRESTQuery( string ns , string action , map<string,string> & params , int & responseCode , stringstream & out ) {
            Timer t;

            int skip = _getOption( params["skip"] , 0 );
            int num = _getOption( params["limit"] , _getOption( params["count" ] , 1000 ) ); // count is old, limit is new

            int one = 0;
            if ( params["one"].size() > 0 && tolower( params["one"][0] ) == 't' ) {
                num = 1;
                one = 1;
            }

            BSONObjBuilder queryBuilder;

            for ( map<string,string>::iterator i = params.begin(); i != params.end(); i++ ) {
                if ( ! i->first.find( "filter_" ) == 0 )
                    continue;

                const char * field = i->first.substr( 7 ).c_str();
                const char * val = i->second.c_str();

                char * temp;

                // TODO: this is how i guess if something is a number.  pretty lame right now
                double number = strtod( val , &temp );
                if ( temp != val )
                    queryBuilder.append( field , number );
                else
                    queryBuilder.append( field , val );
            }

            BSONObj query = queryBuilder.obj();

            auto_ptr<DBClientCursor> cursor = db.query( ns.c_str() , query, num , skip );

            if ( one ) {
                if ( cursor->more() ) {
                    BSONObj obj = cursor->next();
                    out << obj.jsonString() << "\n";
                }
                else {
                    responseCode = 404;
                }
                return;
            }

            out << "{\n";
            out << "  \"offset\" : " << skip << ",\n";
            out << "  \"rows\": [\n";

            int howMany = 0;
            while ( cursor->more() ) {
                if ( howMany++ )
                    out << " ,\n";
                BSONObj obj = cursor->next();
                out << "    " << obj.jsonString();

            }
            out << "\n  ],\n\n";

            out << "  \"total_rows\" : " << howMany << " ,\n";
            out << "  \"query\" : " << query.jsonString() << " ,\n";
            out << "  \"millis\" : " << t.millis() << "\n";
            out << "}\n";
        }

        // TODO Generate id and revision per couch POST spec
        void handlePost( string ns, const char *body, map<string,string> & params, int & responseCode, stringstream & out ) {
            try {
                BSONObj obj = fromjson( body );
                db.insert( ns.c_str(), obj );
            } catch ( ... ) {
                responseCode = 400; // Bad Request.  Seems reasonable for now.
                out << "{ \"ok\" : false }";
                return;
            }

            responseCode = 201;
            out << "{ \"ok\" : true }";
        }

        int _getOption( string val , int def ) {
            if ( val.size() == 0 )
                return def;
            return atoi( val.c_str() );
        }

    private:
        static DBDirectClient db;
    };

    DBDirectClient DbWebServer::db;

    void webServerThread() {
        boost::thread thr(statsThread);
        Client::initThread("websvr");
        DbWebServer mini;
        int p = cmdLine.port + 1000;
        if ( mini.init(bind_ip, p) ) {
            registerListenerSocket( mini.socket() );
            log() << "web admin interface listening on port " << p << '\n';
            mini.run();
        }
        else { 
            log() << "warning: web admin interface failed to initialize on port " << p << endl;
        }
        cc().shutdown();
    }

} // namespace mongo
