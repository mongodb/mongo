// dbclient.cpp - connect to a Mongo database as a database, from C++

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
#include "../db/pdfile.h"
#include "dbclient.h"
#include "../util/builder.h"
#include "../db/jsobj.h"
#include "../db/json.h"
#include "../db/instance.h"
#include "../util/md5.hpp"
#include "../db/dbmessage.h"
#include "../db/cmdline.h"

namespace mongo {

    Query& Query::where(const string &jscode, BSONObj scope) { 
        /* use where() before sort() and hint() and explain(), else this will assert. */
        assert( !obj.hasField("query") );
        BSONObjBuilder b;
        b.appendElements(obj);
        b.appendWhere(jscode, scope);
        obj = b.obj();
        return *this;
    }

    void Query::makeComplex() {
        if ( obj.hasElement( "query" ) )
            return;
        BSONObjBuilder b;
        b.append( "query", obj );
        obj = b.obj();
    }

    Query& Query::sort(const BSONObj& s) { 
        appendComplex( "orderby", s );
        return *this; 
    }

    Query& Query::hint(BSONObj keyPattern) {
        appendComplex( "$hint", keyPattern );
        return *this; 
    }

    Query& Query::explain() {
        appendComplex( "$explain", true );
        return *this; 
    }
    
    Query& Query::snapshot() {
        appendComplex( "$snapshot", true );
        return *this; 
    }
    
    Query& Query::minKey( const BSONObj &val ) {
        appendComplex( "$min", val );
        return *this; 
    }

    Query& Query::maxKey( const BSONObj &val ) {
        appendComplex( "$max", val );
        return *this; 
    }

    bool Query::isComplex() const{
        return obj.hasElement( "query" );
    }
        
    BSONObj Query::getFilter() const {
        if ( ! isComplex() )
            return obj;
        return obj.getObjectField( "query" );
    }
    BSONObj Query::getSort() const {
        if ( ! isComplex() )
            return BSONObj();
        return obj.getObjectField( "orderby" );
    }
    BSONObj Query::getHint() const {
        if ( ! isComplex() )
            return BSONObj();
        return obj.getObjectField( "$hint" );
    }
    bool Query::isExplain() const {
        return isComplex() && obj.getBoolField( "$explain" );
    }
    
    string Query::toString() const{
        return obj.toString();
    }

    /* --- dbclientcommands --- */

    inline bool DBClientWithCommands::isOk(const BSONObj& o) {
        return o.getIntField("ok") == 1;
    }

    inline bool DBClientWithCommands::runCommand(const string &dbname, const BSONObj& cmd, BSONObj &info) {
        string ns = dbname + ".$cmd";
        info = findOne(ns, cmd);
        return isOk(info);
    }

    /* note - we build a bson obj here -- for something that is super common like getlasterror you
              should have that object prebuilt as that would be faster.
    */
    bool DBClientWithCommands::simpleCommand(const string &dbname, BSONObj *info, const string &command) {
        BSONObj o;
        if ( info == 0 )
            info = &o;
        BSONObjBuilder b;
        b.append(command, 1);
        return runCommand(dbname, b.done(), *info);
    }

    unsigned long long DBClientWithCommands::count(const string &_ns, const BSONObj& query) { 
        NamespaceString ns(_ns);
        BSONObj cmd = BSON( "count" << ns.coll << "query" << query );
        BSONObj res;
        if( !runCommand(ns.db.c_str(), cmd, res) )
            uasserted(string("count fails:") + res.toString());
        return res.getIntField("n");
    }

    BSONObj getlasterrorcmdobj = fromjson("{getlasterror:1}");

    BSONObj DBClientWithCommands::getLastErrorDetailed() { 
        BSONObj info;
        runCommand("admin", getlasterrorcmdobj, info);
		return info;
    }

    string DBClientWithCommands::getLastError() { 
        BSONObj info = getLastErrorDetailed();
        BSONElement e = info["err"];
        if( e.eoo() ) return "";
        if( e.type() == Object ) return e.toString();
        return e.str();
    }

    BSONObj getpreverrorcmdobj = fromjson("{getpreverror:1}");

    BSONObj DBClientWithCommands::getPrevError() { 
        BSONObj info;
        runCommand("admin", getpreverrorcmdobj, info);
        return info;
    }

    BSONObj getnoncecmdobj = fromjson("{getnonce:1}");

    string DBClientWithCommands::createPasswordDigest( const string & username , const string & clearTextPassword ){
        md5digest d;
        {
            md5_state_t st;
            md5_init(&st);
            md5_append(&st, (const md5_byte_t *) username.data(), username.length());
            md5_append(&st, (const md5_byte_t *) ":mongo:", 7 );
            md5_append(&st, (const md5_byte_t *) clearTextPassword.data(), clearTextPassword.length());
            md5_finish(&st, d);
        }
        return digestToString( d );
    }

    bool DBClientWithCommands::auth(const string &dbname, const string &username, const string &password_text, string& errmsg, bool digestPassword) {
		//cout << "TEMP AUTH " << toString() << dbname << ' ' << username << ' ' << password_text << ' ' << digestPassword << endl;

		string password = password_text;
		if( digestPassword ) 
			password = createPasswordDigest( username , password_text );

        BSONObj info;
        string nonce;
        if( !runCommand(dbname, getnoncecmdobj, info) ) {
            errmsg = "getnonce fails - connection problem?";
            return false;
        }
        {
            BSONElement e = info.getField("nonce");
            assert( e.type() == String );
            nonce = e.valuestr();
        }

        BSONObj authCmd;
        BSONObjBuilder b;
        {

            b << "authenticate" << 1 << "nonce" << nonce << "user" << username;
            md5digest d;
            {
                md5_state_t st;
                md5_init(&st);
                md5_append(&st, (const md5_byte_t *) nonce.c_str(), nonce.size() );
                md5_append(&st, (const md5_byte_t *) username.data(), username.length());
                md5_append(&st, (const md5_byte_t *) password.c_str(), password.size() );
                md5_finish(&st, d);
            }
            b << "key" << digestToString( d );
            authCmd = b.done();
        }
        
        if( runCommand(dbname, authCmd, info) ) 
            return true;

        errmsg = info.toString();
        return false;
    }

    BSONObj ismastercmdobj = fromjson("{\"ismaster\":1}");

    bool DBClientWithCommands::isMaster(bool& isMaster, BSONObj *info) {
        BSONObj o;
        if ( info == 0 )	info = &o;
        bool ok = runCommand("admin", ismastercmdobj, *info);
        isMaster = (info->getIntField("ismaster") == 1);
        return ok;
    }

    bool DBClientWithCommands::createCollection(const string &ns, unsigned size, bool capped, int max, BSONObj *info) {
        BSONObj o;
        if ( info == 0 )	info = &o;
        BSONObjBuilder b;
        b.append("create", ns);
        if ( size ) b.append("size", size);
        if ( capped ) b.append("capped", true);
        if ( max ) b.append("max", max);
        string db = nsToClient(ns.c_str());
        return runCommand(db.c_str(), b.done(), *info);
    }

    bool DBClientWithCommands::copyDatabase(const string &fromdb, const string &todb, const string &fromhost, BSONObj *info) {
        BSONObj o;
        if ( info == 0 ) info = &o;
        BSONObjBuilder b;
        b.append("copydb", 1);
        b.append("fromhost", fromhost);
        b.append("fromdb", fromdb);
        b.append("todb", todb);
        return runCommand("admin", b.done(), *info);
    }

    bool DBClientWithCommands::setDbProfilingLevel(const string &dbname, ProfilingLevel level, BSONObj *info ) {
        BSONObj o;
        if ( info == 0 ) info = &o;

        if ( level ) {
            // Create system.profile collection.  If it already exists this does nothing.
            // TODO: move this into the db instead of here so that all
            //       drivers don't have to do this.
            string ns = dbname + ".system.profile";
            createCollection(ns.c_str(), 1024 * 1024, true, 0, info);
        }

        BSONObjBuilder b;
        b.append("profile", (int) level);
        return runCommand(dbname, b.done(), *info);
    }

    BSONObj getprofilingcmdobj = fromjson("{\"profile\":-1}");

    bool DBClientWithCommands::getDbProfilingLevel(const string &dbname, ProfilingLevel& level, BSONObj *info) {
        BSONObj o;
        if ( info == 0 ) info = &o;
        if ( runCommand(dbname, getprofilingcmdobj, *info) ) {
            level = (ProfilingLevel) info->getIntField("was");
            return true;
        }
        return false;
    }

    bool DBClientWithCommands::eval(const string &dbname, const string &jscode, BSONObj& info, BSONElement& retValue, BSONObj *args) {
        BSONObjBuilder b;
        b.appendCode("$eval", jscode.c_str());
        if ( args )
            b.appendArray("args", *args);
        bool ok = runCommand(dbname, b.done(), info);
        if ( ok )
            retValue = info.getField("retval");
        return ok;
    }

    bool DBClientWithCommands::eval(const string &dbname, const string &jscode) {
        BSONObj info;
        BSONElement retValue;
        return eval(dbname, jscode, info, retValue);
    }

    list<string> DBClientWithCommands::getDatabaseNames(){
        BSONObj info;
        uassert( "listdatabases failed" , runCommand( "admin" , BSON( "listDatabases" << 1 ) , info ) );
        uassert( "listDatabases.databases not array" , info["databases"].type() == Array );
        
        list<string> names;
        
        BSONObjIterator i( info["databases"].embeddedObjectUserCheck() );
        while ( i.more() ){
            names.push_back( i.next().embeddedObjectUserCheck()["name"].valuestr() );
        }

        return names;
    }

    list<string> DBClientWithCommands::getCollectionNames( const string& db ){
        list<string> names;
        
        string ns = db + ".system.namespaces";
        auto_ptr<DBClientCursor> c = query( ns.c_str() , BSONObj() );
        while ( c->more() ){
            string name = c->next()["name"].valuestr();
            if ( name.find( "$" ) != string::npos )
                continue;
            names.push_back( name );
        }
        return names;
    }

    void testSort() { 
        DBClientConnection c;
        string err;
        if ( !c.connect("localhost", err) ) {
            out() << "can't connect to server " << err << endl;
            return;
        }

        cout << "findOne returns:" << endl;
        cout << c.findOne("test.foo", QUERY( "x" << 3 ) ).toString() << endl;
        cout << c.findOne("test.foo", QUERY( "x" << 3 ).sort("name") ).toString() << endl;

    }

    /* TODO: unit tests should run this? */
    void testDbEval() {
        DBClientConnection c;
        string err;
        if ( !c.connect("localhost", err) ) {
            out() << "can't connect to server " << err << endl;
            return;
        }

        if( !c.auth("dwight", "u", "p", err) ) { 
            out() << "can't authenticate " << err << endl;
            return;
        }

        BSONObj info;
        BSONElement retValue;
        BSONObjBuilder b;
        b.append("0", 99);
        BSONObj args = b.done();
        bool ok = c.eval("dwight", "function() { return args[0]; }", info, retValue, &args);
        out() << "eval ok=" << ok << endl;
        out() << "retvalue=" << retValue.toString() << endl;
        out() << "info=" << info.toString() << endl;

        out() << endl;

        int x = 3;
        assert( c.eval("dwight", "function() { return 3; }", x) );

        out() << "***\n";

        BSONObj foo = fromjson("{\"x\":7}");
        out() << foo.toString() << endl;
        int res=0;
        ok = c.eval("dwight", "function(parm1) { return parm1.x; }", foo, res);
        out() << ok << " retval:" << res << endl;
    }

	void testPaired();

    /* --- dbclientconnection --- */

	bool DBClientConnection::auth(const string &dbname, const string &username, const string &password_text, string& errmsg, bool digestPassword) {
		string password = password_text;
		if( digestPassword ) 
			password = createPasswordDigest( username , password_text );

		if( autoReconnect ) {
			/* note we remember the auth info before we attempt to auth -- if the connection is broken, we will 
			   then have it for the next autoreconnect attempt. 
			*/
			pair<string,string> p = pair<string,string>(username, password);
			authCache[dbname] = p;
		}

		return DBClientBase::auth(dbname, username, password.c_str(), errmsg, false);
	}

    BSONObj DBClientBase::findOne(const string &ns, Query query, const BSONObj *fieldsToReturn, int queryOptions) {
        auto_ptr<DBClientCursor> c =
            this->query(ns, query, 1, 0, fieldsToReturn, queryOptions);

        massert( "DBClientBase::findOne: transport error", c.get() );

        if ( !c->more() )
            return BSONObj();

        return c->next().copy();
    }

    bool DBClientConnection::connect(const string &_serverAddress, string& errmsg) {
        serverAddress = _serverAddress;

        string ip;
        int port;
        size_t idx = serverAddress.find( ":" );
        if ( idx != string::npos ) {
            port = strtol( serverAddress.substr( idx + 1 ).c_str(), 0, 10 );
            ip = serverAddress.substr( 0 , idx );
            ip = hostbyname(ip.c_str());
        } else {
            port = CmdLine::DefaultDBPort;
            ip = hostbyname( serverAddress.c_str() );
        }
        massert( "Unable to parse hostname", !ip.empty() );

        // we keep around SockAddr for connection life -- maybe MessagingPort
        // requires that?
        server = auto_ptr<SockAddr>(new SockAddr(ip.c_str(), port));
        p = auto_ptr<MessagingPort>(new MessagingPort());

        if ( !p->connect(*server) ) {
            stringstream ss;
            ss << "couldn't connect to server " << serverAddress << " " << ip << ":" << port;
            errmsg = ss.str();
            failed = true;
            return false;
        }
        return true;
    }

    void DBClientConnection::_checkConnection() {
        if ( !failed )
            return;
        if ( lastReconnectTry && time(0)-lastReconnectTry < 2 )
            return;
        if ( !autoReconnect )
            return;

        lastReconnectTry = time(0);
        log() << "trying reconnect to " << serverAddress << endl;
        string errmsg;
        string tmp = serverAddress;
        failed = false;
        if ( !connect(tmp.c_str(), errmsg) ) { 
            log() << "reconnect " << serverAddress << " failed " << errmsg << endl;
			return;
		}

		log() << "reconnect " << serverAddress << " ok" << endl;
		for( map< string, pair<string,string> >::iterator i = authCache.begin(); i != authCache.end(); i++ ) { 
			const char *dbname = i->first.c_str();
			const char *username = i->second.first.c_str();
			const char *password = i->second.second.c_str();
			if( !DBClientBase::auth(dbname, username, password, errmsg, false) )
				log() << "reconnect: auth failed db:" << dbname << " user:" << username << ' ' << errmsg << '\n';
		}
    }

    auto_ptr<DBClientCursor> DBClientBase::query(const string &ns, Query query, int nToReturn,
            int nToSkip, const BSONObj *fieldsToReturn, int queryOptions) {
        auto_ptr<DBClientCursor> c( new DBClientCursor( this,
                                    ns, query.obj, nToReturn, nToSkip,
                                    fieldsToReturn, queryOptions ) );
        if ( c->init() )
            return c;
        return auto_ptr< DBClientCursor >( 0 );
    }

    auto_ptr<DBClientCursor> DBClientBase::getMore( const string &ns, long long cursorId, int nToReturn, int options ) {
        auto_ptr<DBClientCursor> c( new DBClientCursor( this, ns, cursorId, nToReturn, options ) );
        if ( c->init() )
            return c;
        return auto_ptr< DBClientCursor >( 0 );
    }

    void DBClientBase::insert( const string & ns , BSONObj obj ) {
        Message toSend;

        BufBuilder b;
        int opts = 0;
        b.append( opts );
        b.append( ns );
        obj.appendSelfToBufBuilder( b );

        toSend.setData( dbInsert , b.buf() , b.len() );

        say( toSend );
    }

    void DBClientBase::insert( const string & ns , const vector< BSONObj > &v ) {
        Message toSend;
        
        BufBuilder b;
        int opts = 0;
        b.append( opts );
        b.append( ns );
        for( vector< BSONObj >::const_iterator i = v.begin(); i != v.end(); ++i )
            i->appendSelfToBufBuilder( b );
        
        toSend.setData( dbInsert, b.buf(), b.len() );
        
        say( toSend );
    }

    void DBClientBase::remove( const string & ns , Query obj , bool justOne ) {
        Message toSend;

        BufBuilder b;
        int opts = 0;
        b.append( opts );
        b.append( ns );

        int flags = 0;
        if ( justOne || obj.obj.hasField( "_id" ) )
            flags |= 1;
        b.append( flags );

        obj.obj.appendSelfToBufBuilder( b );

        toSend.setData( dbDelete , b.buf() , b.len() );

        say( toSend );
    }

    void DBClientBase::update( const string & ns , Query query , BSONObj obj , bool upsert ) {

        BufBuilder b;
        b.append( (int)0 ); // reserverd
        b.append( ns );

        b.append( (int)upsert );

        query.obj.appendSelfToBufBuilder( b );
        obj.appendSelfToBufBuilder( b );

        Message toSend;
        toSend.setData( dbUpdate , b.buf() , b.len() );

        say( toSend );
    }

    bool DBClientBase::ensureIndex( const string &ns , BSONObj keys , bool unique, const string & name ) {
        BSONObjBuilder toSave;
        toSave.append( "ns" , ns );
        toSave.append( "key" , keys );

        string cacheKey(ns);
        cacheKey += "--";

        if ( name != "" ) {
            toSave.append( "name" , name );
            cacheKey += name;
        }
        else {
            stringstream ss;
            
            bool first = 1;
            for ( BSONObjIterator i(keys); i.more(); ) {
                BSONElement f = i.next();

                if ( first )
                    first = 0;
                else
                    ss << "_";

                ss << f.fieldName() << "_";
                if( f.isNumber() )
                    ss << f.numberInt();
            }

            toSave.append( "name" , ss.str() );
            cacheKey += ss.str();
        }
        
        if ( unique )
            toSave.appendBool( "unique", unique );

        if ( _seenIndexes.count( cacheKey ) )
            return 0;
        _seenIndexes.insert( cacheKey );

        insert( Namespace( ns.c_str() ).getSisterNS( "system.indexes"  ).c_str() , toSave.obj() );
        return 1;
    }

    void DBClientBase::resetIndexCache() {
        _seenIndexes.clear();
    }

    /* -- DBClientCursor ---------------------------------------------- */

    void assembleRequest( const string &ns, BSONObj query, int nToReturn, int nToSkip, const BSONObj *fieldsToReturn, int queryOptions, Message &toSend ) {
        CHECK_OBJECT( query , "assembleRequest query" );
        // see query.h for the protocol we are using here.
        BufBuilder b;
        int opts = queryOptions;
        b.append(opts);
        b.append(ns.c_str());
        b.append(nToSkip);
        b.append(nToReturn);
        query.appendSelfToBufBuilder(b);
        if ( fieldsToReturn )
            fieldsToReturn->appendSelfToBufBuilder(b);
        toSend.setData(dbQuery, b.buf(), b.len());
    }

    void DBClientConnection::say( Message &toSend ) {
        checkConnection();
        try { 
            port().say( toSend );
        } catch( SocketException & ) { 
            failed = true;
            throw;
        }
    }

    void DBClientConnection::sayPiggyBack( Message &toSend ) {
        port().piggyBack( toSend );
    }

    bool DBClientConnection::call( Message &toSend, Message &response, bool assertOk ) {
        /* todo: this is very ugly messagingport::call returns an error code AND can throw 
                 an exception.  we should make it return void and just throw an exception anytime 
                 it fails
        */
        try { 
            if ( !port().call(toSend, response) ) {
                failed = true;
                if ( assertOk )
                    massert("dbclient error communicating with server", false);
                return false;
            }
        }
        catch( SocketException & ) { 
            failed = true;
            throw;
        }
        return true;
    }

    void DBClientConnection::checkResponse( const char *data, int nReturned ) {
        /* check for errors.  the only one we really care about at
         this stage is "not master" */
        if ( clientPaired && nReturned ) {
            BSONObj o(data);
            BSONElement e = o.firstElement();
            if ( strcmp(e.fieldName(), "$err") == 0 &&
                    e.type() == String && strncmp(e.valuestr(), "not master", 10) == 0 ) {
                clientPaired->isntMaster();
            }
        }
    }

    bool DBClientCursor::init() {
        Message toSend;
        if ( !cursorId ) {
            assembleRequest( ns, query, nToReturn, nToSkip, fieldsToReturn, opts, toSend );
        } else {
            BufBuilder b;
            b.append( opts );
            b.append( ns.c_str() );
            b.append( nToReturn );
            b.append( cursorId );
            toSend.setData( dbGetMore, b.buf(), b.len() );
        }
        if ( !connector->call( toSend, *m, false ) )
            return false;
        dataReceived();
        return true;
    }

    void DBClientCursor::requestMore() {
        assert( cursorId && pos == nReturned );

        BufBuilder b;
        b.append(opts);
        b.append(ns.c_str());
        b.append(nToReturn);
        b.append(cursorId);

        Message toSend;
        toSend.setData(dbGetMore, b.buf(), b.len());
        auto_ptr<Message> response(new Message());
        connector->call( toSend, *response );

        m = response;
        dataReceived();
    }

    void DBClientCursor::dataReceived() {
        QueryResult *qr = (QueryResult *) m->data;
        resultFlags = qr->resultFlags();
        if ( qr->resultFlags() & QueryResult::ResultFlag_CursorNotFound ) {
            // cursor id no longer valid at the server.
            assert( qr->cursorId == 0 );
            cursorId = 0; // 0 indicates no longer valid (dead)
        }
        if ( cursorId == 0 || ! ( opts & Option_CursorTailable ) ) {
            // only set initially: we don't want to kill it on end of data
            // if it's a tailable cursor
            cursorId = qr->cursorId;
        }
        nReturned = qr->nReturned;
        pos = 0;
        data = qr->data();

        connector->checkResponse( data, nReturned );
        /* this assert would fire the way we currently work:
            assert( nReturned || cursorId == 0 );
        */
    }

    bool DBClientCursor::more() {
        if ( pos < nReturned )
            return true;

        if ( cursorId == 0 )
            return false;

        requestMore();
        return pos < nReturned;
    }

    BSONObj DBClientCursor::next() {
        assert( more() );
        pos++;
        BSONObj o(data);
        data += o.objsize();
        return o;
    }

    DBClientCursor::~DBClientCursor() {
        if ( cursorId && ownCursor_ ) {
            BufBuilder b;
            b.append( (int)0 ); // reserved
            b.append( (int)1 ); // number
            b.append( cursorId );

            Message m;
            m.setData( dbKillCursors , b.buf() , b.len() );

            connector->sayPiggyBack( m );
        }

    }

    /* --- class dbclientpaired --- */

    string DBClientPaired::toString() {
        stringstream ss;
        ss << "state: " << master << '\n';
        ss << "left:  " << left.toStringLong() << '\n';
        ss << "right: " << right.toStringLong() << '\n';
        return ss.str();
    }

#pragma warning(disable: 4355)
    DBClientPaired::DBClientPaired() :
		left(true, this), right(true, this)
    {
        master = NotSetL;
    }
#pragma warning(default: 4355)

    /* find which server, the left or right, is currently master mode */
    void DBClientPaired::_checkMaster() {
        for ( int retry = 0; retry < 2; retry++ ) {
            int x = master;
            for ( int pass = 0; pass < 2; pass++ ) {
                DBClientConnection& c = x == 0 ? left : right;
                try {
                    bool im;
                    BSONObj o;
                    c.isMaster(im, &o);
                    if ( retry )
                        log() << "checkmaster: " << c.toString() << ' ' << o.toString() << '\n';
                    if ( im ) {
                        master = (State) (x + 2);
                        return;
                    }
                }
                catch (AssertionException&) {
                    if ( retry )
                        log() << "checkmaster: caught exception " << c.toString() << '\n';
                }
                x = x^1;
            }
            sleepsecs(1);
        }

        uassert("checkmaster: no master found", false);
    }

    inline DBClientConnection& DBClientPaired::checkMaster() {
        if ( master > NotSetR ) {
            // a master is selected.  let's just make sure connection didn't die
            DBClientConnection& c = master == Left ? left : right;
            if ( !c.isFailed() )
                return c;
            // after a failure, on the next checkMaster, start with the other
            // server -- presumably it took over. (not critical which we check first,
            // just will make the failover slightly faster if we guess right)
            master = master == Left ? NotSetR : NotSetL;
        }

        _checkMaster();
        assert( master > NotSetR );
        return master == Left ? left : right;
    }

    DBClientConnection& DBClientPaired::slaveConn(){
        DBClientConnection& m = checkMaster();
        assert( ! m.isFailed() );
        return master == Left ? right : left;
    }

    bool DBClientPaired::connect(const string &serverHostname1, const string &serverHostname2) {
        string errmsg;
        bool l = left.connect(serverHostname1, errmsg);
        bool r = right.connect(serverHostname2, errmsg);
        master = l ? NotSetL : NotSetR;
        if ( !l && !r ) // it would be ok to fall through, but checkMaster will then try an immediate reconnect which is slow
            return false;
        try {
            checkMaster();
        }
        catch (AssertionException&) {
            return false;
        }
        return true;
    }

    bool DBClientPaired::connect(string hostpairstring) { 
        size_t comma = hostpairstring.find( "," );
        uassert("bad hostpairstring", comma != string::npos);
        return connect( hostpairstring.substr( 0 , comma ) , hostpairstring.substr( comma + 1 ) );
    }

	bool DBClientPaired::auth(const string &dbname, const string &username, const string &pwd, string& errmsg) { 
		DBClientConnection& m = checkMaster();
		if( !m.auth(dbname, username, pwd, errmsg) )
			return false;
		/* we try to authentiate with the other half of the pair -- even if down, that way the authInfo is cached. */
		string e;
		try {
			if( &m == &left ) 
				right.auth(dbname, username, pwd, e);
			else
				left.auth(dbname, username, pwd, e);
		}
		catch( AssertionException&) { 
		}
		return true;
	}

    auto_ptr<DBClientCursor> DBClientPaired::query(const string &a, Query b, int c, int d,
            const BSONObj *e, int f)
    {
        return checkMaster().query(a,b,c,d,e,f);
    }

    BSONObj DBClientPaired::findOne(const string &a, Query b, const BSONObj *c, int d) {
        return checkMaster().findOne(a,b,c,d);
    }

	void testPaired() { 
		DBClientPaired p;
		log() << "connect returns " << p.connect("localhost:27017", "localhost:27018") << endl;

		//DBClientConnection p(true);
		string errmsg;
		//		log() << "connect " << p.connect("localhost", errmsg) << endl;
		log() << "auth " << p.auth("dwight", "u", "p", errmsg) << endl;

		while( 1 ) { 
			sleepsecs(3);
			try { 
				log() << "findone returns " << p.findOne("dwight.foo", BSONObj()).toString() << endl;
				sleepsecs(3);
				BSONObj info;
				bool im;
				log() << "ismaster returns " << p.isMaster(im,&info) << " info: " << info.toString() << endl;
			}
			catch(...) { 
				cout << "caught exception" << endl;
			}
		}
	}

} // namespace mongo
