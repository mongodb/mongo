// dbclient.cpp - connect to a Mongo database as a database, from C++

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "../db/pdfile.h"
#include "dbclient.h"
#include "../bson/util/builder.h"
#include "../db/jsobj.h"
#include "../db/json.h"
#include "../db/instance.h"
#include "../util/md5.hpp"
#include "../db/dbmessage.h"
#include "../db/cmdline.h"
#include "connpool.h"
#include "../s/util.h"
#include "syncclusterconnection.h"

namespace mongo {

    DBClientBase* ConnectionString::connect( string& errmsg ) const {
        switch ( _type ){
        case MASTER: {
            DBClientConnection * c = new DBClientConnection(true);
            log(1) << "creating new connection to:" << _servers[0] << endl;
            if ( ! c->connect( _servers[0] , errmsg ) ) {
                delete c;
                return 0;
            }
            return c;
        }
            
        case PAIR: 
        case SET: {
            DBClientReplicaSet * set = new DBClientReplicaSet( _setName , _servers );
            if( ! set->connect() ){
                delete set;
                errmsg = "connect failed to set ";
                errmsg += toString();
                return 0;
            }
            return set;
        }
            
        case SYNC: {
            // TODO , don't copy
            list<HostAndPort> l;
            for ( unsigned i=0; i<_servers.size(); i++ )
                l.push_back( _servers[i] );
            return new SyncClusterConnection( l );
        }
            
        case INVALID:
            throw UserException( 13421 , "trying to connect to invalid ConnectionString" );
            break;
        }
        
        assert( 0 );
        return 0;
    }

    ConnectionString ConnectionString::parse( const string& host , string& errmsg ){
        
        string::size_type i = host.find( '/' );
        if ( i != string::npos ){
            // replica set
            return ConnectionString( SET , host.substr( i + 1 ) , host.substr( 0 , i ) );
        }

        int numCommas = DBClientBase::countCommas( host );
        
        if( numCommas == 0 ) 
            return ConnectionString( HostAndPort( host ) );
        
        if ( numCommas == 1 ) 
            return ConnectionString( PAIR , host );

        if ( numCommas == 2 )
            return ConnectionString( SYNC , host );
        
        errmsg = (string)"invalid hostname [" + host + "]";
        return ConnectionString(); // INVALID
    }

    Query& Query::where(const string &jscode, BSONObj scope) { 
        /* use where() before sort() and hint() and explain(), else this will assert. */
        assert( ! isComplex() );
        BSONObjBuilder b;
        b.appendElements(obj);
        b.appendWhere(jscode, scope);
        obj = b.obj();
        return *this;
    }

    void Query::makeComplex() {
        if ( isComplex() )
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

    bool Query::isComplex( bool * hasDollar ) const{
        if ( obj.hasElement( "query" ) ){
            if ( hasDollar )
                hasDollar[0] = false;
            return true;
        }

        if ( obj.hasElement( "$query" ) ){
            if ( hasDollar )
                hasDollar[0] = true;
            return true;
        }

        return false;
    }
        
    BSONObj Query::getFilter() const {
        bool hasDollar;
        if ( ! isComplex( &hasDollar ) )
            return obj;
        
        return obj.getObjectField( hasDollar ? "$query" : "query" );
    }
    BSONObj Query::getSort() const {
        if ( ! isComplex() )
            return BSONObj();
        BSONObj ret = obj.getObjectField( "orderby" );
        if (ret.isEmpty())
            ret = obj.getObjectField( "$orderby" );
        return ret;
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

    bool DBClientWithCommands::isOk(const BSONObj& o) {
        return o["ok"].trueValue();
    }

    enum QueryOptions DBClientWithCommands::availableOptions() {
        if ( !_haveCachedAvailableOptions ) {
            BSONObj ret;
            if ( runCommand( "admin", BSON( "availablequeryoptions" << 1 ), ret ) ) {
                _cachedAvailableOptions = ( enum QueryOptions )( ret.getIntField( "options" ) );
            }
            _haveCachedAvailableOptions = true;
        }
        return _cachedAvailableOptions;
    }
    
    inline bool DBClientWithCommands::runCommand(const string &dbname, const BSONObj& cmd, BSONObj &info, int options) {
        string ns = dbname + ".$cmd";
        info = findOne(ns, cmd, 0 , options);
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

    unsigned long long DBClientWithCommands::count(const string &_ns, const BSONObj& query, int options) { 
        NamespaceString ns(_ns);
        BSONObj cmd = BSON( "count" << ns.coll << "query" << query );
        BSONObj res;
        if( !runCommand(ns.db.c_str(), cmd, res, options) )
            uasserted(11010,string("count fails:") + res.toString());
        return res["n"].numberLong();
    }

    BSONObj getlasterrorcmdobj = fromjson("{getlasterror:1}");

    BSONObj DBClientWithCommands::getLastErrorDetailed() { 
        BSONObj info;
        runCommand("admin", getlasterrorcmdobj, info);
		return info;
    }

    string DBClientWithCommands::getLastError() { 
        BSONObj info = getLastErrorDetailed();
        return getLastErrorString( info );
    }
    
    string DBClientWithCommands::getLastErrorString( const BSONObj& info ){
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
        if ( info == 0 )	
            info = &o;
        bool ok = runCommand("admin", ismastercmdobj, *info);
        isMaster = info->getField("ismaster").trueValue();
        return ok;
    }

    bool DBClientWithCommands::createCollection(const string &ns, long long size, bool capped, int max, BSONObj *info) {
        BSONObj o;
        if ( info == 0 )	info = &o;
        BSONObjBuilder b;
        string db = nsToDatabase(ns.c_str());
        b.append("create", ns.c_str() + db.length() + 1);
        if ( size ) b.append("size", size);
        if ( capped ) b.append("capped", true);
        if ( max ) b.append("max", max);
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

    BSONObj DBClientWithCommands::mapreduce(const string &ns, const string &jsmapf, const string &jsreducef, BSONObj query, const string& outputcolname) { 
        BSONObjBuilder b;
        b.append("mapreduce", nsGetCollection(ns));
        b.appendCode("map", jsmapf.c_str());
        b.appendCode("reduce", jsreducef.c_str());
        if( !query.isEmpty() )
            b.append("query", query);
        if( !outputcolname.empty() )
            b.append("out", outputcolname);
        BSONObj info;
        runCommand(nsGetDB(ns), b.done(), info);
        return info;
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
        uassert( 10005 ,  "listdatabases failed" , runCommand( "admin" , BSON( "listDatabases" << 1 ) , info ) );
        uassert( 10006 ,  "listDatabases.databases not array" , info["databases"].type() == Array );
        
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

    bool DBClientWithCommands::exists( const string& ns ){
        list<string> names;
        
        string db = nsGetDB( ns ) + ".system.namespaces";
        BSONObj q = BSON( "name" << ns );
        return count( db.c_str() , q ) != 0;
    }

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

    BSONObj DBClientInterface::findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn, int queryOptions) {
        auto_ptr<DBClientCursor> c =
            this->query(ns, query, 1, 0, fieldsToReturn, queryOptions);

        uassert( 10276 ,  "DBClientBase::findOne: transport error", c.get() );

        if ( c->hasResultFlag( ResultFlag_ShardConfigStale ) )
            throw StaleConfigException( ns , "findOne has stale config" );

        if ( !c->more() )
            return BSONObj();

        return c->nextSafe().copy();
    }

    bool DBClientConnection::connect(const HostAndPort& server, string& errmsg){
        _server = server;
        _serverString = _server.toString();
        return _connect( errmsg );
    }

    bool DBClientConnection::_connect( string& errmsg ){
        _serverString = _server.toString();
        // we keep around SockAddr for connection life -- maybe MessagingPort
        // requires that?
        server.reset(new SockAddr(_server.host().c_str(), _server.port()));
        p.reset(new MessagingPort( _timeout, _logLevel ));

        if (server->getAddr() == "0.0.0.0"){
            failed = true;
            return false;
        }

        if ( !p->connect(*server) ) {
            stringstream ss;
            ss << "couldn't connect to server " << _serverString << '}';
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
        log(_logLevel) << "trying reconnect to " << _serverString << endl;
        string errmsg;
        failed = false;
        if ( ! _connect(errmsg) ) { 
            log(_logLevel) << "reconnect " << _serverString << " failed " << errmsg << endl;
			return;
		}

		log(_logLevel) << "reconnect " << _serverString << " ok" << endl;
		for( map< string, pair<string,string> >::iterator i = authCache.begin(); i != authCache.end(); i++ ) { 
			const char *dbname = i->first.c_str();
			const char *username = i->second.first.c_str();
			const char *password = i->second.second.c_str();
			if( !DBClientBase::auth(dbname, username, password, errmsg, false) )
				log(_logLevel) << "reconnect: auth failed db:" << dbname << " user:" << username << ' ' << errmsg << '\n';
		}
    }

    auto_ptr<DBClientCursor> DBClientBase::query(const string &ns, Query query, int nToReturn,
                                                 int nToSkip, const BSONObj *fieldsToReturn, int queryOptions , int batchSize ) {
        auto_ptr<DBClientCursor> c( new DBClientCursor( this,
                                                        ns, query.obj, nToReturn, nToSkip,
                                                        fieldsToReturn, queryOptions , batchSize ) );
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

    struct DBClientFunConvertor {
        void operator()( DBClientCursorBatchIterator &i ) {
            while( i.moreInCurrentBatch() ) {
                _f( i.nextSafe() );
            }
        }
        boost::function<void(const BSONObj &)> _f;
    };
    
    unsigned long long DBClientConnection::query( boost::function<void(const BSONObj&)> f, const string& ns, Query query, const BSONObj *fieldsToReturn, int queryOptions ) {
        DBClientFunConvertor fun;
        fun._f = f;
        boost::function<void(DBClientCursorBatchIterator &)> ptr( fun );
        return DBClientConnection::query( ptr, ns, query, fieldsToReturn, queryOptions );
    }
        
    unsigned long long DBClientConnection::query( boost::function<void(DBClientCursorBatchIterator &)> f, const string& ns, Query query, const BSONObj *fieldsToReturn, int queryOptions ) {
        // mask options
        queryOptions &= (int)( QueryOption_NoCursorTimeout | QueryOption_SlaveOk );
        unsigned long long n = 0;

        bool doExhaust = ( availableOptions() & QueryOption_Exhaust );
        if ( doExhaust ) {
            queryOptions |= (int)QueryOption_Exhaust;            
        }
        auto_ptr<DBClientCursor> c( this->query(ns, query, 0, 0, fieldsToReturn, queryOptions) );
        uassert( 13386, "socket error for mapping query", c.get() );
        
        if ( !doExhaust ) {
            while( c->more() ) {
                DBClientCursorBatchIterator i( *c );
                f( i );
                n += i.n();
            }
            return n;
        }

        try { 
            while( 1 ) { 
                while( c->moreInCurrentBatch() ) { 
                    DBClientCursorBatchIterator i( *c );
                    f( i );
                    n += i.n();
                }

                if( c->getCursorId() == 0 ) 
                    break;

                c->exhaustReceiveMore();
            }
        }
        catch(std::exception&) { 
            /* connection CANNOT be used anymore as more data may be on the way from the server.
               we have to reconnect.
               */
            failed = true;
            p->shutdown();
            throw;
        }

        return n;
    }

    void DBClientBase::insert( const string & ns , BSONObj obj ) {
        Message toSend;

        BufBuilder b;
        int opts = 0;
        b.appendNum( opts );
        b.appendStr( ns );
        obj.appendSelfToBufBuilder( b );

        toSend.setData( dbInsert , b.buf() , b.len() );

        say( toSend );
    }

    void DBClientBase::insert( const string & ns , const vector< BSONObj > &v ) {
        Message toSend;
        
        BufBuilder b;
        int opts = 0;
        b.appendNum( opts );
        b.appendStr( ns );
        for( vector< BSONObj >::const_iterator i = v.begin(); i != v.end(); ++i )
            i->appendSelfToBufBuilder( b );
        
        toSend.setData( dbInsert, b.buf(), b.len() );
        
        say( toSend );
    }

    void DBClientBase::remove( const string & ns , Query obj , bool justOne ) {
        Message toSend;

        BufBuilder b;
        int opts = 0;
        b.appendNum( opts );
        b.appendStr( ns );

        int flags = 0;
        if ( justOne )
            flags |= RemoveOption_JustOne;
        b.appendNum( flags );

        obj.obj.appendSelfToBufBuilder( b );

        toSend.setData( dbDelete , b.buf() , b.len() );

        say( toSend );
    }

    void DBClientBase::update( const string & ns , Query query , BSONObj obj , bool upsert , bool multi ) {

        BufBuilder b;
        b.appendNum( (int)0 ); // reserved
        b.appendStr( ns );

        int flags = 0;
        if ( upsert ) flags |= UpdateOption_Upsert;
        if ( multi ) flags |= UpdateOption_Multi;
        b.appendNum( flags );

        query.obj.appendSelfToBufBuilder( b );
        obj.appendSelfToBufBuilder( b );

        Message toSend;
        toSend.setData( dbUpdate , b.buf() , b.len() );

        say( toSend );
    }

    auto_ptr<DBClientCursor> DBClientWithCommands::getIndexes( const string &ns ){
        return query( Namespace( ns.c_str() ).getSisterNS( "system.indexes" ).c_str() , BSON( "ns" << ns ) );
    }
    
    void DBClientWithCommands::dropIndex( const string& ns , BSONObj keys ){
        dropIndex( ns , genIndexName( keys ) );
    }


    void DBClientWithCommands::dropIndex( const string& ns , const string& indexName ){
        BSONObj info;
        if ( ! runCommand( nsToDatabase( ns.c_str() ) , 
                           BSON( "deleteIndexes" << NamespaceString( ns ).coll << "index" << indexName ) , 
                           info ) ){
            log(_logLevel) << "dropIndex failed: " << info << endl;
            uassert( 10007 ,  "dropIndex failed" , 0 );
        }
        resetIndexCache();
    }
    
    void DBClientWithCommands::dropIndexes( const string& ns ){
        BSONObj info;
        uassert( 10008 ,  "dropIndexes failed" , runCommand( nsToDatabase( ns.c_str() ) , 
                                                    BSON( "deleteIndexes" << NamespaceString( ns ).coll << "index" << "*") , 
                                                    info ) );
        resetIndexCache();
    }

    void DBClientWithCommands::reIndex( const string& ns ){
        list<BSONObj> all;
        auto_ptr<DBClientCursor> i = getIndexes( ns );
        while ( i->more() ){
            all.push_back( i->next().getOwned() );
        }
        
        dropIndexes( ns );
        
        for ( list<BSONObj>::iterator i=all.begin(); i!=all.end(); i++ ){
            BSONObj o = *i;
            insert( Namespace( ns.c_str() ).getSisterNS( "system.indexes" ).c_str() , o );
        }
        
    }
    

    string DBClientWithCommands::genIndexName( const BSONObj& keys ){
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
        return ss.str();
    }

    bool DBClientWithCommands::ensureIndex( const string &ns , BSONObj keys , bool unique, const string & name ) {
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
            string nn = genIndexName( keys );
            toSave.append( "name" , nn );
            cacheKey += nn;
        }
        
        if ( unique )
            toSave.appendBool( "unique", unique );

        if ( _seenIndexes.count( cacheKey ) )
            return 0;
        _seenIndexes.insert( cacheKey );

        insert( Namespace( ns.c_str() ).getSisterNS( "system.indexes"  ).c_str() , toSave.obj() );
        return 1;
    }

    void DBClientWithCommands::resetIndexCache() {
        _seenIndexes.clear();
    }

    /* -- DBClientCursor ---------------------------------------------- */

#ifdef _DEBUG
#define CHECK_OBJECT( o , msg ) massert( 10337 ,  (string)"object not valid" + (msg) , (o).isValid() )
#else
#define CHECK_OBJECT( o , msg )
#endif

    void assembleRequest( const string &ns, BSONObj query, int nToReturn, int nToSkip, const BSONObj *fieldsToReturn, int queryOptions, Message &toSend ) {
        CHECK_OBJECT( query , "assembleRequest query" );
        // see query.h for the protocol we are using here.
        BufBuilder b;
        int opts = queryOptions;
        b.appendNum(opts);
        b.appendStr(ns);
        b.appendNum(nToSkip);
        b.appendNum(nToReturn);
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

    void DBClientConnection::recv( Message &m ) { 
        port().recv(m);
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
                    uassert( 10278 , "dbclient error communicating with server", false);
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
        if ( clientSet && nReturned ) {
            assert(data);
            BSONObj o(data);
            BSONElement e = o.firstElement();
            if ( strcmp(e.fieldName(), "$err") == 0 &&
                    e.type() == String && strncmp(e.valuestr(), "not master", 10) == 0 ) {
                clientSet->isntMaster();
            }
        }
    }

    void DBClientConnection::killCursor( long long cursorId ){
        BufBuilder b;
        b.appendNum( (int)0 ); // reserved
        b.appendNum( (int)1 ); // number
        b.appendNum( cursorId );
        
        Message m;
        m.setData( dbKillCursors , b.buf() , b.len() );
        
        sayPiggyBack( m );
    }

    /* --- class dbclientpaired --- */

    string DBClientReplicaSet::toString() {
        return getServerAddress();
    }

    DBClientReplicaSet::DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers )
        : _name( name ) , _currentMaster( 0 ), _servers( servers ){
        
        for ( unsigned i=0; i<_servers.size(); i++ )
            _conns.push_back( new DBClientConnection( true , this ) );
    }
    
    DBClientReplicaSet::~DBClientReplicaSet(){
        for ( unsigned i=0; i<_conns.size(); i++ )
            delete _conns[i];
        _conns.clear();
    }
    
    string DBClientReplicaSet::getServerAddress() const {
        StringBuilder ss;
        if ( _name.size() )
            ss << _name << "/";
    
        for ( unsigned i=0; i<_servers.size(); i++ ){
            if ( i > 0 )
                ss << ",";
            ss << _servers[i].toString();
        }
        return ss.str();
    }

    /* find which server, the left or right, is currently master mode */
    void DBClientReplicaSet::_checkMaster() {
        
        bool triedQuickCheck = false;
        
        log( _logLevel + 1) <<  "_checkMaster on: " << toString() << endl;
        for ( int retry = 0; retry < 2; retry++ ) {
            for ( unsigned i=0; i<_conns.size(); i++ ){
                DBClientConnection * c = _conns[i];
                try {
                    bool im;
                    BSONObj o;
                    c->isMaster(im, &o);
                    
                    if ( retry )
                        log(_logLevel) << "checkmaster: " << c->toString() << ' ' << o << '\n';
                    
                    string maybePrimary;
                    if ( o["hosts"].type() == Array ){
                        if ( o["primary"].type() == String )
                            maybePrimary = o["primary"].String();
                        
                        BSONObjIterator hi(o["hosts"].Obj());
                        while ( hi.more() ){
                            string toCheck = hi.next().String();
                            int found = -1;
                            for ( unsigned x=0; x<_servers.size(); x++ ){
                                if ( toCheck == _servers[x].toString() ){
                                    found = x;
                                    break;
                                }
                            }
                            
                            if ( found == -1 ){
                                HostAndPort h( toCheck );
                                _servers.push_back( h );
                                _conns.push_back( new DBClientConnection( true, this ) );
                                string temp;
                                _conns[ _conns.size() - 1 ]->connect( h , temp );
                                log( _logLevel ) << "updated set to: " << toString() << endl;
                            }
                            
                        }
                    }

                    if ( im ) {
                        _currentMaster = c;
                        return;
                    }
                    
                    if ( maybePrimary.size() && ! triedQuickCheck ){
                        for ( unsigned x=0; x<_servers.size(); x++ ){
                            if ( _servers[i].toString() != maybePrimary )
                                continue;
                            triedQuickCheck = true;
                            _conns[x]->isMaster( im , &o );
                            if ( im ){
                                _currentMaster = _conns[x];
                                return;
                            }
                        }
                    }
                }
                catch ( std::exception& e ) {
                    if ( retry )
                        log(_logLevel) << "checkmaster: caught exception " << c->toString() << ' ' << e.what() << endl;
                }
            }
            sleepsecs(1);
        }

        uassert( 10009 , "checkmaster: no master found", false);
    }

    DBClientConnection * DBClientReplicaSet::checkMaster() {
        if ( _currentMaster ){
            // a master is selected.  let's just make sure connection didn't die
            if ( ! _currentMaster->isFailed() )
                return _currentMaster;
            _currentMaster = 0;
        }

        _checkMaster();
        assert( _currentMaster );
        return _currentMaster;
    }

    DBClientConnection& DBClientReplicaSet::masterConn(){
        return *checkMaster();
    }

    DBClientConnection& DBClientReplicaSet::slaveConn(){
        DBClientConnection * m = checkMaster();
        assert( ! m->isFailed() );
        
        DBClientConnection * failedSlave = 0;

        for ( unsigned i=0; i<_conns.size(); i++ ){
            if ( m == _conns[i] )
                continue;
            failedSlave = _conns[i];
            if ( _conns[i]->isFailed() )
                continue;
            return *_conns[i];
        }

        assert(failedSlave);
        return *failedSlave;
    }

    bool DBClientReplicaSet::connect(){
        string errmsg;

        bool anyGood = false;
        for ( unsigned i=0; i<_conns.size(); i++ ){
            if ( _conns[i]->connect( _servers[i] , errmsg ) )
                anyGood = true;
        }
        
        if ( ! anyGood )
            return false;

        try {
            checkMaster();
        }
        catch (AssertionException&) {
            return false;
        }
        return true;
    }

	bool DBClientReplicaSet::auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword ) { 
		DBClientConnection * m = checkMaster();
		if( !m->auth(dbname, username, pwd, errmsg, digestPassword ) )
			return false;
        
		/* we try to authentiate with the other half of the pair -- even if down, that way the authInfo is cached. */
        for ( unsigned i=0; i<_conns.size(); i++ ){
            if ( _conns[i] == m )
                continue;
            try {
                string e;
                _conns[i]->auth( dbname , username , pwd , e , digestPassword );
            }
            catch ( AssertionException& ){
            }
        }

		return true;
	}

    auto_ptr<DBClientCursor> DBClientReplicaSet::query(const string &a, Query b, int c, int d,
                                                   const BSONObj *e, int f, int g){
        // TODO: if slave ok is set go to a slave
        return checkMaster()->query(a,b,c,d,e,f,g);
    }

    BSONObj DBClientReplicaSet::findOne(const string &a, const Query& b, const BSONObj *c, int d) {
        return checkMaster()->findOne(a,b,c,d);
    }

    bool DBClientReplicaSet::isMember( const DBConnector * conn ) const {
        if ( conn == this )
            return true;
        
        for ( unsigned i=0; i<_conns.size(); i++ )
            if ( _conns[i]->isMember( conn ) )
                return true;
        
        return false;
    }
    

    bool serverAlive( const string &uri ) {
        DBClientConnection c( false, 0, 20 ); // potentially the connection to server could fail while we're checking if it's alive - so use timeouts
        string err;
        if ( !c.connect( uri, err ) )
            return false;
        if ( !c.simpleCommand( "admin", 0, "ping" ) )
            return false;
        return true;
    }
    
} // namespace mongo
