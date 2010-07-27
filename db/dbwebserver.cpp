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

#include "pch.h"
#include "../util/miniwebserver.h"
#include "../util/mongoutils/html.h"
#include "../util/md5.hpp"
#include "db.h"
#include "repl.h"
#include "replpair.h"
#include "instance.h"
#include "security.h"
#include "stats/snapshots.h"
#include "background.h"
#include "commands.h"
#include "../util/version.h"
#include "../util/ramlog.h"
#include <pcrecpp.h>
#include "dbwebserver.h"
#include <boost/date_time/posix_time/posix_time.hpp>
#undef assert
#define assert MONGO_assert

namespace mongo {

    using namespace mongoutils::html;
    using namespace bson;

    extern void fillRsLog(stringstream&);
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

    bool _bold;
    string bold(bool x) {
        _bold = x;
        return x ? "<b>" : "";
    }
    string bold() {
        return _bold ? "</b>" : "";
    }

    bool execCommand( Command * c ,
                      Client& client , int queryOptions , 
                      const char *ns, BSONObj& cmdObj , 
                      BSONObjBuilder& result, 
                      bool fromRepl );

    class DbWebServer : public MiniWebServer {
    public:
        DbWebServer(const string& ip, int port) : MiniWebServer(ip, port), ramlog(new RamLog()) {
            Logstream::get().addGlobalTee( ramlog );
        }

    private:
        // caller locks
        void doLockedStuff(stringstream& ss) {
            ss << "# databases: " << dbHolder.size() << '\n';

            if( ClientCursor::byLocSize()>500 )
                ss << bold(ClientCursor::byLocSize()>10000) << "Cursors byLoc.size(): " << ClientCursor::byLocSize() << bold() << '\n';

            ss << "\nreplication: ";
            if( *replInfo )
                ss << "\nreplInfo:  " << replInfo << "\n\n";
            if( replSet ) {
                ss << a("", "see replSetGetStatus link top of page") << "--replSet </a>" << cmdLine.replSet << '\n';
            }
            else {
                ss << "\nmaster: " << replSettings.master << '\n';
                ss << "slave:  " << replSettings.slave << '\n';
                if ( replPair ) {
                    ss << "replpair:\n";
                    ss << replPair->getInfo();
                }
                bool seemCaughtUp = getInitialSyncCompleted();
                if ( !seemCaughtUp ) ss << "<b>";
                ss <<   "initialSyncCompleted: " << seemCaughtUp;
                if ( !seemCaughtUp ) ss << "</b>";
                ss << '\n';
            }
            
            auto_ptr<SnapshotDelta> delta = statsSnapshots.computeDelta();
            if ( delta.get() ){
                ss << "\n<b>dbtop</b> (occurences|percent of elapsed)\n";
                ss << "<table border=1 cellpadding=2 cellspacing=0>";
                ss << "<tr align='left'><th>";
                ss << a("http://www.mongodb.org/display/DOCS/Developer+FAQ#DeveloperFAQ-What%27sa%22namespace%22%3F", "namespace") << 
                    "NS</a></th>"
                    "<th colspan=2>total</th>"
                    "<th colspan=2>Reads</th>"
                    "<th colspan=2>Writes</th>"
                    "<th colspan=2>Queries</th>"
                    "<th colspan=2>GetMores</th>"
                    "<th colspan=2>Inserts</th>"
                    "<th colspan=2>Updates</th>"
                    "<th colspan=2>Removes</th>";
                ss << "</tr>\n";
                
                display( ss , (double) delta->elapsed() , "GLOBAL" , delta->globalUsageDiff() );
                
                Top::UsageMap usage = delta->collectionUsageDiff();
                for ( Top::UsageMap::iterator i=usage.begin(); i != usage.end(); i++ ){
                    display( ss , (double) delta->elapsed() , i->first , i->second );
                }
                
                ss << "</table>";
            }

            statsSnapshots.outputLockInfoHTML( ss );

            BackgroundOperation::dump(ss);

            ss << "</pre><h4>Log:</h4>";

            ramlog->toHTML( ss );
        }

        void display( stringstream& ss , double elapsed , const Top::UsageData& usage ){
            ss << "<td>";
            ss << usage.count;
            ss << "</td><td>";
            double per = 100 * ((double)usage.time)/elapsed;
            ss << setprecision(4) << fixed << per << "%";
            ss << "</td>";
        }

        void display( stringstream& ss , double elapsed , const string& ns , const Top::CollectionData& data ){
            if ( ns != "GLOBAL" && data.total.count == 0 )
                return;
            ss << "<tr><th>" << ns << "</th>";
            
            display( ss , elapsed , data.total );

            display( ss , elapsed , data.readLock );
            display( ss , elapsed , data.writeLock );

            display( ss , elapsed , data.queries );
            display( ss , elapsed , data.getmore );
            display( ss , elapsed , data.insert );
            display( ss , elapsed , data.update );
            display( ss , elapsed , data.remove );
            
            ss << "</tr>\n";
        }

        void tablecell( stringstream& ss , bool b ){
            ss << "<td>" << (b ? "<b>X</b>" : "") << "</td>";
        }

        template< typename T> 
        void tablecell( stringstream& ss , const T& t ){
            ss << "<td>" << t << "</td>";
        }
        
        void doUnlockedStuff(stringstream& ss) {
            /* this is in the header already ss << "port:      " << port << '\n'; */
            ss << mongodVersion() << '\n';
            ss << "git hash: " << gitVersion() << '\n';
            ss << "sys info: " << sysInfo() << '\n';
            ss << "uptime: " << time(0)-started << " seconds\n";
            if ( replAllDead )
                ss << "<b>replication replAllDead=" << replAllDead << "</b>\n";
            ss << a("", "information on caught assertion exceptions");
            ss << "assertions:</a>\n";
            for ( int i = 0; i < 4; i++ ) {
                if ( lastAssert[i].isSet() ) {
                    if ( i == 3 ) ss << "uassert";
                    else if( i == 2 ) ss << "massert";
                    else if( i == 0 ) ss << "assert";
                    else if( i == 1 ) ss << "warnassert";
                    else ss << i;
                    ss << ' ' << lastAssert[i].toString();
                }
            }

            ss << "\n<table border=1 cellpadding=2 cellspacing=0>";
            ss << "<tr align='left'>"
               << th( a("", "Connections to the database, both internal and external.", "Client") )
               << th( a("http://www.mongodb.org/display/DOCS/Viewing+and+Terminating+Current+Operation", "", "OpId") )
               << "<th>Active</th>" 
               << "<th>LockType</th>"
               << "<th>Waiting</th>"
               << "<th>SecsRunning</th>"
               << "<th>Op</th>"
               << th( a("http://www.mongodb.org/display/DOCS/Developer+FAQ#DeveloperFAQ-What%27sa%22namespace%22%3F", "", "Namespace") )
               << "<th>Query</th>"
               << "<th>client</th>"
               << "<th>msg</th>"
               << "<th>progress</th>"

               << "</tr>\n";
            {
                scoped_lock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) { 
                    Client *c = *i;
                    CurOp& co = *(c->curop());
                    ss << "<tr><td>" << c->desc() << "</td>";
                    
                    tablecell( ss , co.opNum() );
                    tablecell( ss , co.active() );
                    tablecell( ss , co.getLockType() );
                    tablecell( ss , co.isWaitingForLock() );
                    if ( co.active() )
                        tablecell( ss , co.elapsedSeconds() );
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getOp() );
                    tablecell( ss , co.getNS() );
                    if ( co.haveQuery() )
                        tablecell( ss , co.query() );
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getRemoteString() );

                    tablecell( ss , co.getMessage() );
                    tablecell( ss , co.getProgressMeter().toString() );


                    ss << "</tr>\n";
                }
            }
            ss << "</table>\n";
        }
        
    private:
        string hostname() { 
            stringstream s;
            s << getHostName();
            if( mongo::cmdLine.port != CmdLine::DefaultDBPort ) 
                s << ':' << mongo::cmdLine.port;
            return s.str();
        }

        string _replSetOplog(string parms) { 
            stringstream s;
            string t = "Replication oplog";
            s << start(t);
            s << p(t);

            if( theReplSet == 0 ) { 
                if( cmdLine.replSet.empty() ) 
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://www.mongodb.org/display/DOCS/Replica+Set+Configuration#InitialSetup", "", "initiated") 
                           + ".<br>" + ReplSet::startupStatusMsg);
                }
            }
            else {
                try {
                    theReplSet->getOplogDiagsAsHtml(stringToNum(parms.c_str()), s);
                }
                catch(std::exception& e) { 
                    s << "error querying oplog: " << e.what() << '\n'; 
                }
            }

            s << _end();
            return s.str();
        }

        /* /_replSet show replica set status in html format */
        string _replSet() { 
            stringstream s;
            s << start("Replica Set Status " + hostname());
            s << p( a("/", "back", "Home") + " | " + 
                    a("/local/system.replset/?html=1", "", "View Replset Config") + " | " +
                    a("/replSetGetStatus?text", "", "replSetGetStatus") + " | " +
                    a("http://www.mongodb.org/display/DOCS/Replica+Sets", "", "Docs")
                  );

            if( theReplSet == 0 ) { 
                if( cmdLine.replSet.empty() ) 
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://www.mongodb.org/display/DOCS/Replica+Set+Configuration#InitialSetup", "", "initiated") 
                           + ".<br>" + ReplSet::startupStatusMsg);
                }
            }
            else {
                try {
                    theReplSet->summarizeAsHtml(s);
                }
                catch(...) { s << "error summarizing replset status\n"; }
            }
            s << p("Recent replset log activity:");
            fillRsLog(s);
            s << _end();
            return s.str();
        }

        bool allowed( const char * rq , vector<string>& headers, const SockAddr &from ) {
            if ( from.isLocalHost() )
                return true;

            {
                readlocktryassert rl("admin.system.users", 10000);
                if( Helpers::isEmpty("admin.system.users") )
                    return true;
            }

            Client::GodScope gs;

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
                    
                    stringstream r;
                    r << ha1 << ':' << parms["nonce"];
                    if ( parms["nc"].size() && parms["cnonce"].size() && parms["qop"].size() ){
                        r << ':';
                        r << parms["nc"];
                        r << ':';
                        r << parms["cnonce"];
                        r << ':';
                        r << parms["qop"];
                    }
                    r << ':';
                    r << ha2;
                    string r1 = md5simpledigest( r.str() );
                    
                    if ( r1 == parms["response"] )
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

        string _commands() {
            stringstream ss;
            ss << start("Commands List");
            ss << p( a("/", "back", "Home") );
            ss << p( "<b>MongoDB List of <a href=\"http://www.mongodb.org/display/DOCS/Commands\">Commands</a></b>\n" );
            const map<string, Command*> *m = Command::commandsByBestName();
            ss << "S:slave-only  N:no-lock  R:read-lock  W:write-lock  A:admin-only<br>\n";
            ss << table();
            ss << "<tr><th>Command</th><th>Attributes</th><th>Help</th></tr>\n";
            for( map<string, Command*>::const_iterator i = m->begin(); i != m->end(); i++ ) 
                i->second->htmlHelp(ss);
            ss << _table() << _end();
            return ss.str();
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
            if ( url.size() > 1 ) {

                {
                    DbWebHandler * handler = DbWebHandler::findHandler( url );
                    if ( handler ){
                        if ( handler->requiresREST( url ) && ! cmdLine.rest )
                            _rejectREST( responseMsg , responseCode , headers );
                        // TODO: auth here
                        else
                            handler->handle( rq , url , responseMsg , responseCode , headers , from );
                        return;
                    }
                }
                if ( ! allowed( rq , headers, from ) ) {
                    responseCode = 401;
                    headers.push_back( "Content-Type: text/plain" );
                    responseMsg = "not allowed\n";
                    return;
                }              

                if ( url.find( "/_status" ) == 0 ){
                    headers.push_back( "Content-Type: application/json" );
                    generateServerStatus( url , responseMsg );
                    responseCode = 200;
                    return;
                }

                if ( ! cmdLine.rest ) {
                    _rejectREST( responseMsg , responseCode , headers );
                    return;
                }

                if( startsWith(url, "/_replSet") ) {
                    string s = str::after(url, "/_replSetOplog?");
                    if( !s.empty() )
                        responseMsg = _replSetOplog(s);
                    else
                        responseMsg = _replSet();
                    responseCode = 200;
                    return;
                }

                if( startsWith(url, "/_commands") ) {
                    responseMsg = _commands();
                    responseCode = 200;
                    return;
                }

                /* run a command from the web ui */
                const char *p = url.c_str();
                if( *p == '/' ) {
                    const char *h = strstr(p, "?text");
                    string cmd = p+1;
                    if( h && h > p+1 ) 
                        cmd = string(p+1, h-p-1);
                    const map<string,Command*> *m = Command::webCommands();
                    if( m && m->count(cmd) ) {
                        Command *c = m->find(cmd)->second;
                        Client& client = cc();
                        BSONObjBuilder result;
                        BSONObjBuilder b;
                        b.append(c->name, 1);
                        BSONObj cmdObj = b.obj();
                        execCommand(c, client, 0, "admin.", cmdObj, result, false);
                        responseCode = 200;
                        string j = result.done().jsonString(JS, h != 0 ? 1 : 0);
                        if( h == 0 ) { 
                            headers.push_back( "Content-Type: application/json" );
                        }
                        else { 
                            headers.push_back( "Content-Type: text/plain" );
                        }
                        responseMsg = j;
                        if( h ) 
                            responseMsg += '\n';
                        return;

                    }
                }

                DEV log() << "handle REST request " << url << endl;
                handleRESTRequest( rq , url , responseMsg , responseCode , headers );
                return;
            }

            responseCode = 200;
            stringstream ss;
            string dbname;
            {
                stringstream z;
                z << "mongod " << hostname();
                dbname = z.str();
            }
            ss << start(dbname) << h2(dbname);
            ss << "<a href=\"/_commands\">List all commands</a> | \n";
            ss << "<a href=\"/_replSet\">Replica set status</a>\n";
            ss << "<pre>";
            //ss << "<a href=\"/_status\">_status</a>";
            {
                const map<string, Command*> *m = Command::webCommands();
                if( m ) {
                    ss << a("", "These read-only context-less commands can be executed from the web interface.  Results are json format, unless ?text is appended in which case the result is output as text for easier human viewing", "Commands") << ": ";
                    for( map<string, Command*>::const_iterator i = m->begin(); i != m->end(); i++ ) { 
                        stringstream h;
                        i->second->help(h);
                        string help = h.str();
                        ss << "<a href=\"/" << i->first << "?text\"";
                        if( help != "no help defined" )
                            ss << " title=\"" << help << '"';
                        ss << ">" << i->first << "</a> ";
                    }
                    ss << '\n';
                }
            }
            ss << '\n';
            ss << "HTTP <a "
                "title=\"click for documentation on this http interface\""
                "href=\"http://www.mongodb.org/display/DOCS/Http+Interface\">admin port</a>:" << _port << "\n";

            doUnlockedStuff(ss);

            ss << "<a "
                  "href=\"http://www.mongodb.org/pages/viewpage.action?pageId=7209296\" " 
                  "title=\"snapshot: was the db in the write lock when this page was generated?\">";
            ss << "write locked:</a> " << (dbMutex.info().isLocked() ? "true" : "false") << "\n";
            {
                Timer t;
                readlocktry lk( "" , 300 );
                if ( lk.got() ){
                    ss << "time to get readlock: " << t.millis() << "ms\n";
                    doLockedStuff(ss);
                }
                else {
                    ss << "\n<b>timed out getting dblock</b>\n";
                }
            }
            

            ss << "</pre>\n</body></html>\n";
            responseMsg = ss.str();

            // we want to return SavedContext from before the authentication was performed
            if ( ! allowed( rq , headers, from ) ){
                responseCode = 401;
                responseMsg = "not allowed\n";
                return;
            }            
        }

        void generateServerStatus( string url , string& responseMsg ){
            static vector<string> commands;
            if ( commands.size() == 0 ){
                commands.push_back( "serverStatus" );
                commands.push_back( "buildinfo" );
            }

            BSONObj params;
            if ( url.find( "?" ) != string::npos ) {
                parseParams( params , url.substr( url.find( "?" ) + 1 ) );
            }
            
            BSONObjBuilder buf(1024);
            
            for ( unsigned i=0; i<commands.size(); i++ ){
                string cmd = commands[i];

                Command * c = Command::findCommand( cmd );
                assert( c );
                assert( c->locktype() == 0 );
                
                BSONObj co;
                {
                    BSONObjBuilder b;
                    b.append( cmd , 1 );
                    
                    if ( cmd == "serverStatus" && params["repl"].type() ){
                        b.append( "repl" , atoi( params["repl"].valuestr() ) );
                    }
                    
                    co = b.obj();
                }
                
                string errmsg;
                
                BSONObjBuilder sub;
                if ( ! c->run( "admin.$cmd" , co , errmsg , sub , false ) )
                    buf.append( cmd , errmsg );
                else
                    buf.append( cmd , sub.obj() );
            }
            
            responseMsg = buf.obj().jsonString();
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

            BSONObj params;
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

            string fullns = urlDecode(dbname + "." + coll);

            headers.push_back( (string)"x-action: " + action );
            headers.push_back( (string)"x-ns: " + fullns );

            bool html = false;


            stringstream ss;

            if ( method == "GET" ) {
                responseCode = 200;
                html = handleRESTQuery( fullns , action , params , responseCode , ss  );
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

            if( html ) 
                headers.push_back("Content-Type: text/html;charset=utf-8");
            else
                headers.push_back("Content-Type: text/plain;charset=utf-8");

            responseMsg = ss.str();
        }

        bool handleRESTQuery( string ns , string action , BSONObj & params , int & responseCode , stringstream & out ) {
            Timer t;

            int html = _getOption( params["html"] , 0 ); 
            int skip = _getOption( params["skip"] , 0 );
            int num  = _getOption( params["limit"] , _getOption( params["count" ] , 1000 ) ); // count is old, limit is new

            int one = 0;
            if ( params["one"].type() == String && tolower( params["one"].valuestr()[0] ) == 't' ) {
                num = 1;
                one = 1;
            }

            BSONObjBuilder queryBuilder;

            BSONObjIterator i(params);
            while ( i.more() ){
                BSONElement e = i.next();
                string name = e.fieldName();
                if ( ! name.find( "filter_" ) == 0 )
                    continue;

                string field = name.substr(7);
                const char * val = e.valuestr();

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
            uassert( 13085 , "query failed for dbwebserver" , cursor.get() );

            if ( one ) {
                if ( cursor->more() ) {
                    BSONObj obj = cursor->next();
                    out << obj.jsonString(Strict,html?1:0) << '\n';
                }
                else {
                    responseCode = 404;
                }
                return html != 0;
            }

            if( html )  {
                string title = string("query ") + ns;
                out << start(title) 
                    << p(title)
                    << "<pre>";
            } else {
                out << "{\n";
                out << "  \"offset\" : " << skip << ",\n";
                out << "  \"rows\": [\n";
            }

            int howMany = 0;
            while ( cursor->more() ) {
                if ( howMany++ && html == 0 )
                    out << " ,\n";
                BSONObj obj = cursor->next();
                if( html ) {
                    if( out.tellp() > 4 * 1024 * 1024 ) {
                        out << "Stopping output: more than 4MB returned and in html mode\n";
                        break;
                    }
                    out << obj.jsonString(Strict, html?1:0) << "\n\n";
                }
                else {
                    if( out.tellp() > 50 * 1024 * 1024 ) // 50MB limit - we are using ram
                        break;
                    out << "    " << obj.jsonString();
                }
            }

            if( html ) { 
                out << "</pre>\n";
                if( howMany == 0 ) out << p("Collection is empty");
                out << _end();
            }
            else {
                out << "\n  ],\n\n";
                out << "  \"total_rows\" : " << howMany << " ,\n";
                out << "  \"query\" : " << query.jsonString() << " ,\n";
                out << "  \"millis\" : " << t.millis() << '\n';
                out << "}\n";
            }

            return html != 0;
        }

        // TODO Generate id and revision per couch POST spec
        void handlePost( string ns, const char *body, BSONObj& params, int & responseCode, stringstream & out ) {
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

        int _getOption( BSONElement e , int def ) {
            if ( e.isNumber() )
                return e.numberInt();
            if ( e.type() == String )
                return atoi( e.valuestr() );
            return def;
        }
        
        void _rejectREST( string& responseMsg , int& responseCode, vector<string>& headers ){
                                responseCode = 403;
                    stringstream ss;
                    ss << "REST is not enabled.  use --rest to turn on.\n";
                    ss << "check that port " << _port << " is secured for the network too.\n";
                    responseMsg = ss.str();
                    headers.push_back( "Content-Type: text/plain" );
        }

    private:
        static DBDirectClient db;
        RamLog * ramlog;
    };

    // -- handler framework ---

    DbWebHandler::DbWebHandler( const string& name , double priority , bool requiresREST )
        : _name(name) , _priority(priority) , _requiresREST(requiresREST){

        { // setup strings
            _defaultUrl = "/";
            _defaultUrl += name;

            stringstream ss;
            ss << name << " priority: " << priority << " rest: " << requiresREST;
            _toString = ss.str();
        }
        
        { // add to handler list
            if ( ! _handlers )
                _handlers = new vector<DbWebHandler*>();
            _handlers->push_back( this );
            sort( _handlers->begin() , _handlers->end() );
        }
    }

    DbWebHandler * DbWebHandler::findHandler( const string& url ){
        if ( ! _handlers )
            return 0;
        
        for ( unsigned i=0; i<_handlers->size(); i++ ){
            DbWebHandler * h = (*_handlers)[i];
            if ( h->handles( url ) )
                return h;
        }

        return 0;
    }
    
    vector<DbWebHandler*> * DbWebHandler::_handlers = 0;

    // --- basic handlers ---

    class FavIconHandler : public DbWebHandler {
    public:
        FavIconHandler() : DbWebHandler( "favicon.ico" , 0 , false ){}

        virtual void handle( const char *rq, string url, 
                             string& responseMsg, int& responseCode,
                             vector<string>& headers,  const SockAddr &from ){
            responseCode = 404;
            headers.push_back( "Content-Type: text/plain" );
            responseMsg = "no favicon\n";
        }

    } faviconHandler;
    
    // --- external ----

    DBDirectClient DbWebServer::db;

    void webServerThread() {
        Client::initThread("websvr");
        const int p = cmdLine.port + 1000;
        DbWebServer mini(cmdLine.bind_ip, p);
        log() << "web admin interface listening on port " << p << endl;
        mini.initAndListen();
        cc().shutdown();
    }

} // namespace mongo
