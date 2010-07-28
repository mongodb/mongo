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
        
        bool allowed( const char * rq , vector<string>& headers, const SockAddr &from ) {
            if ( from.isLocalHost() )
                return true;

            if ( ! webHaveAdminUsers() )
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

                BSONObj user = webGetAdminUser( parms["username"] );
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
                
                if ( ! allowed( rq , headers, from ) ) {
                    responseCode = 401;
                    headers.push_back( "Content-Type: text/plain" );
                    responseMsg = "not allowed\n";
                    return;
                }              

                {
                    DbWebHandler * handler = DbWebHandler::findHandler( url );
                    if ( handler ){
                        if ( handler->requiresREST( url ) && ! cmdLine.rest )
                            _rejectREST( responseMsg , responseCode , headers );
                        else
                            handler->handle( rq , url , responseMsg , responseCode , headers , from );
                        return;
                    }
                }


                if ( ! cmdLine.rest ) {
                    _rejectREST( responseMsg , responseCode , headers );
                    return;
                }
                
                responseCode = 404;
                headers.push_back( "Content-Type: text/html" );
                responseMsg = "<html><body>unknown url</body></html>\n";
                return;
            }

            responseCode = 200;
            stringstream ss;
            string dbname;
            {
                stringstream z;
                z << "mongod " << prettyHostName();
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

        void _rejectREST( string& responseMsg , int& responseCode, vector<string>& headers ){
                                responseCode = 403;
                    stringstream ss;
                    ss << "REST is not enabled.  use --rest to turn on.\n";
                    ss << "check that port " << _port << " is secured for the network too.\n";
                    responseMsg = ss.str();
                    headers.push_back( "Content-Type: text/plain" );
        }

    protected:
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
    
    class StatusHandler : public DbWebHandler {
    public:
        StatusHandler() : DbWebHandler( "_status" , 1 , false ){}
        
        virtual void handle( const char *rq, string url, 
                             string& responseMsg, int& responseCode,
                             vector<string>& headers,  const SockAddr &from ){
            headers.push_back( "Content-Type: application/json" );
            responseCode = 200;
            
            static vector<string> commands;
            if ( commands.size() == 0 ){
                commands.push_back( "serverStatus" );
                commands.push_back( "buildinfo" );
            }
            
            BSONObj params;
            if ( url.find( "?" ) != string::npos ) {
                MiniWebServer::parseParams( params , url.substr( url.find( "?" ) + 1 ) );
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

    } statusHandler;

    class CommandListHandler : public DbWebHandler {
    public:
        CommandListHandler() : DbWebHandler( "_commands" , 1 , true ){}
        
        virtual void handle( const char *rq, string url, 
                             string& responseMsg, int& responseCode,
                             vector<string>& headers,  const SockAddr &from ){
            headers.push_back( "Content-Type: text/html" );
            responseCode = 200;
            
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
            
            responseMsg = ss.str();
        }
    } commandListHandler;

    class CommandsHandler : public DbWebHandler {
    public:
        CommandsHandler() : DbWebHandler( "DUMMY COMMANDS" , 2 , true ){}
        
        bool _cmd( const string& url , string& cmd , bool& text ) const {
            const char * x = url.c_str();
            
            if ( x[0] != '/' ){
                // this should never happen
                return false;
            }
            
            if ( strchr( x + 1 , '/' ) )
                return false;
            
            x++;

            const char * end = strstr( x , "?text" );
            if ( end ){
                text = true;
                cmd = string( x , end - x );
            }
            else {
                text = false;
                cmd = string(x);
            }
             
            return true;
        }

        Command * _cmd( const string& cmd ) const {
            const map<string,Command*> *m = Command::webCommands();
            if( ! m )
                return 0;
            
            map<string,Command*>::const_iterator i = m->find(cmd);
            if ( i == m->end() )
                return 0;
            
            return i->second;
        }

        virtual bool handles( const string& url ) const { 
            string cmd;
            bool text;
            if ( ! _cmd( url , cmd , text ) )
                return false;

            return _cmd( cmd );
        }
        
        virtual void handle( const char *rq, string url, 
                             string& responseMsg, int& responseCode,
                             vector<string>& headers,  const SockAddr &from ){
            
            string cmd;
            bool text;
            assert( _cmd( url , cmd , text ) );
            Command * c = _cmd( cmd );
            assert( c );

            BSONObj cmdObj = BSON( cmd << 1 );
            Client& client = cc();
            
            BSONObjBuilder result;
            execCommand(c, client, 0, "admin.", cmdObj , result, false);
            
            responseCode = 200;
            
            string j = result.done().jsonString(JS, text );
            responseMsg = j;
            
            if( text ){
                headers.push_back( "Content-Type: text/plain" );
                responseMsg += '\n';
            }
            else {
                headers.push_back( "Content-Type: application/json" );
            }

        }
        
    } commandsHandler;

    // --- external ----

    string prettyHostName() { 
        stringstream s;
        s << getHostName();
        if( mongo::cmdLine.port != CmdLine::DefaultDBPort ) 
            s << ':' << mongo::cmdLine.port;
        return s.str();
    }

    void webServerThread() {
        Client::initThread("websvr");
        const int p = cmdLine.port + 1000;
        DbWebServer mini(cmdLine.bind_ip, p);
        log() << "web admin interface listening on port " << p << endl;
        mini.initAndListen();
        cc().shutdown();
    }

} // namespace mongo
