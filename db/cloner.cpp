// cloner.cpp - copy a database (export/import basically)

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
#include "pdfile.h"
#include "../client/dbclient.h"
#include "../bson/util/builder.h"
#include "jsobj.h"
#include "query.h"
#include "commands.h"
#include "db.h"
#include "instance.h"
#include "repl.h"

namespace mongo {

    void ensureHaveIdIndex(const char *ns);

    bool replAuthenticate(DBClientBase *);

    class Cloner: boost::noncopyable {
        auto_ptr< DBClientWithCommands > conn;
        void copy(const char *from_ns, const char *to_ns, bool isindex, bool logForRepl,
                  bool masterSameProcess, bool slaveOk, Query q = Query());
        struct Fun;
    public:
        Cloner() { }
        
        /* slaveOk     - if true it is ok if the source of the data is !ismaster.
           useReplAuth - use the credentials we normally use as a replication slave for the cloning
           snapshot    - use $snapshot mode for copying collections.  note this should not be used when it isn't required, as it will be slower.
                         for example repairDatabase need not use it.
        */
        void setConnection( DBClientWithCommands *c ) { conn.reset( c ); }
        bool go(const char *masterHost, string& errmsg, const string& fromdb, bool logForRepl, bool slaveOk, bool useReplAuth, bool snapshot);

        bool copyCollection( const string& from , const string& ns , const BSONObj& query , string& errmsg , bool copyIndexes = true, bool logForRepl = true );
    };

    /* for index info object:
         { "name" : "name_1" , "ns" : "foo.index3" , "key" :  { "name" : 1.0 } }
       we need to fix up the value in the "ns" parameter so that the name prefix is correct on a
       copy to a new name.
    */
    BSONObj fixindex(BSONObj o) {
        BSONObjBuilder b;
        BSONObjIterator i(o);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( string("ns") == e.fieldName() ) {
                uassert( 10024 , "bad ns field for index during dbcopy", e.type() == String);
                const char *p = strchr(e.valuestr(), '.');
                uassert( 10025 , "bad ns field for index during dbcopy [2]", p);
                string newname = cc().database()->name + p;
                b.append("ns", newname);
            }
            else
                b.append(e);
        }
        BSONObj res= b.obj();

        /*    if( mod ) {
            out() << "before: " << o.toString() << endl;
            o.dump();
            out() << "after:  " << res.toString() << endl;
            res.dump();
            }*/

        return res;
    }

    struct Cloner::Fun {
        void operator()( DBClientCursorBatchIterator &i ) {
            mongolock l( true );
            if ( context ) {
                context->relocked();
            }
            
            while( i.moreInCurrentBatch() ) {
                if ( n % 128 == 127 /*yield some*/ ) {
                    dbtemprelease t;
                }
                
                BSONObj tmp = i.nextSafe();
            
                /* assure object is valid.  note this will slow us down a little. */
                if ( !tmp.valid() ) {
                    stringstream ss;
                    ss << "Cloner: skipping corrupt object from " << from_collection;
                    BSONElement e = tmp.firstElement();
                    try {
                        e.validate();
                        ss << " firstElement: " << e;
                    }
                    catch( ... ){
                        ss << " firstElement corrupt";
                    }
                    out() << ss.str() << endl;
                    continue;
                }
            
                ++n;
            
                BSONObj js = tmp;
                if ( isindex ) {
                    assert( strstr(from_collection, "system.indexes") );
                    js = fixindex(tmp);
                    storedForLater->push_back( js.getOwned() );
                    continue;
                }
            
                try { 
                    theDataFileMgr.insertWithObjMod(to_collection, js);
                    if ( logForRepl )
                        logOp("i", to_collection, js);
                }
                catch( UserException& e ) { 
                    log() << "warning: exception cloning object in " << from_collection << ' ' << e.what() << " obj:" << js.toString() << '\n';
                }
            
                RARELY if ( time( 0 ) - saveLast > 60 ) {
                    log() << n << " objects cloned so far from collection " << from_collection << endl;
                    saveLast = time( 0 );
                }
            }
        }
        int n;
        bool isindex;
        const char *from_collection;
        const char *to_collection;
        time_t saveLast;
        list<BSONObj> *storedForLater;     
        bool logForRepl;
        Client::Context *context;
    };
    
    /* copy the specified collection
       isindex - if true, this is system.indexes collection, in which we do some transformation when copying.
    */
    void Cloner::copy(const char *from_collection, const char *to_collection, bool isindex, bool logForRepl, bool masterSameProcess, bool slaveOk, Query query) {
        list<BSONObj> storedForLater;
        
        Fun f;
        f.n = 0;
        f.isindex = isindex;
        f.from_collection = from_collection;
        f.to_collection = to_collection;
        f.saveLast = time( 0 );
        f.storedForLater = &storedForLater;
        f.logForRepl = logForRepl;
        
        int options = QueryOption_NoCursorTimeout | ( slaveOk ? QueryOption_SlaveOk : 0 );
        {
            dbtemprelease r;
            f.context = r._context;
            DBClientConnection *remote = dynamic_cast< DBClientConnection* >( conn.get() );
            if ( remote ) {
                remote->query( boost::function<void(DBClientCursorBatchIterator &)>( f ), from_collection, query, 0, options );
            } else { // no exhaust mode for direct client, so we have this hack
                auto_ptr<DBClientCursor> c = conn->query( from_collection, query, 0, 0, 0, options );
                assert( c.get() );
                while( c->more() ) {
                    DBClientCursorBatchIterator i( *c );
                    f( i );
                }
            }
        }
        
        if ( storedForLater.size() ){
            for ( list<BSONObj>::iterator i = storedForLater.begin(); i!=storedForLater.end(); i++ ){
                BSONObj js = *i;
                try { 
                    theDataFileMgr.insertWithObjMod(to_collection, js);
                    if ( logForRepl )
                        logOp("i", to_collection, js);
                }
                catch( UserException& e ) { 
                    log() << "warning: exception cloning object in " << from_collection << ' ' << e.what() << " obj:" << js.toString() << '\n';
                }
            }
        }
    }

    bool copyCollectionFromRemote(const string& host, const string& ns, const BSONObj& query, string& errmsg, bool logForRepl) {
        Cloner c;
        return c.copyCollection(host, ns, query, errmsg , /*copyIndexes*/ true, logForRepl);
    }

    bool Cloner::copyCollection( const string& from , const string& ns , const BSONObj& query , string& errmsg , bool copyIndexes, bool logForRepl ) {
        auto_ptr<DBClientConnection> myconn;
        myconn.reset( new DBClientConnection() );
        if ( ! myconn->connect( from , errmsg ) )
            return false;

        conn.reset( myconn.release() );
        
        writelock lk(ns); // TODO: make this lower down
        Client::Context ctx(ns);

        { // config
            string temp = ctx.db()->name + ".system.namespaces";
            BSONObj config = conn->findOne( temp , BSON( "name" << ns ) );
            if ( config["options"].isABSONObj() )
                if ( ! userCreateNS( ns.c_str() , config["options"].Obj() , errmsg, true , 0 ) )
                    return false;
        }
        
        { // main data
            copy( ns.c_str() , ns.c_str() , /*isindex*/false , logForRepl , false , true , Query(query).snapshot() );
        }
        
        /* TODO : copyIndexes bool does not seem to be implemented! */
        if( !copyIndexes ) { 
            log() << "ERROR copy collection copyIndexes not implemented? " << ns << endl;
        }

        { // indexes
            string temp = ctx.db()->name + ".system.indexes";
            copy( temp.c_str() , temp.c_str() , /*isindex*/true , logForRepl , false , true , BSON( "ns" << ns ) );
        }
        return true;
    }
    
    extern bool inDBRepair;
    void ensureIdIndexForNewNs(const char *ns);

    bool Cloner::go(const char *masterHost, string& errmsg, const string& fromdb, bool logForRepl, bool slaveOk, bool useReplAuth, bool snapshot) {

		massert( 10289 ,  "useReplAuth is not written to replication log", !useReplAuth || !logForRepl );

        string todb = cc().database()->name;
        stringstream a,b;
        a << "localhost:" << cmdLine.port;
        b << "127.0.0.1:" << cmdLine.port;
        bool masterSameProcess = ( a.str() == masterHost || b.str() == masterHost );
        if ( masterSameProcess ) {
            if ( fromdb == todb && cc().database()->path == dbpath ) {
                // guard against an "infinite" loop
                /* if you are replicating, the local.sources config may be wrong if you get this */
                errmsg = "can't clone from self (localhost).";
                return false;
            }
        }
        /* todo: we can put these releases inside dbclient or a dbclient specialization.
           or just wait until we get rid of global lock anyway.
           */
        string ns = fromdb + ".system.namespaces";
        list<BSONObj> toClone;
        {  
            dbtemprelease r;
		
            // just using exhaust for collection copying right now
            auto_ptr<DBClientCursor> c;
            {
                if ( conn.get() ) {
                    // nothing to do
                } else if ( !masterSameProcess ) {
                    ConnectionString cs = ConnectionString::parse( masterHost, errmsg );
                    auto_ptr<DBClientBase> con( cs.connect( errmsg ));
                    if ( !con.get() )
                        return false;
                    if( !replAuthenticate(con.get()) )
                        return false;
                    
                    conn = con;
                } else {
                    conn.reset( new DBDirectClient() );
                }
                c = conn->query( ns.c_str(), BSONObj(), 0, 0, 0, slaveOk ? QueryOption_SlaveOk : 0 );
            }

            if ( c.get() == 0 ) {
                errmsg = "query failed " + ns;
                return false;
            }
            
            while ( c->more() ){
                BSONObj collection = c->next();

                log(2) << "\t cloner got " << collection << endl;

                BSONElement e = collection.getField("name");
                if ( e.eoo() ) {
                    string s = "bad system.namespaces object " + collection.toString();
                    massert( 10290 , s.c_str(), false);
                }
                assert( !e.eoo() );
                assert( e.type() == String );
                const char *from_name = e.valuestr();

                if( strstr(from_name, ".system.") ) { 
                    /* system.users and s.js is cloned -- but nothing else from system.
                     * system.indexes is handled specially at the end*/
                    if( legalClientSystemNS( from_name , true ) == 0 ){
                        log(2) << "\t\t not cloning because system collection" << endl;
                        continue;
                    }
                }
                if( ! nsDollarCheck( from_name ) ){
                    log(2) << "\t\t not cloning because has $ " << endl;
                    continue;
                }            
                toClone.push_back( collection.getOwned() );
            }
        }

        for ( list<BSONObj>::iterator i=toClone.begin(); i != toClone.end(); i++ ){
            {
                dbtemprelease r;
            }
            BSONObj collection = *i;
            log(2) << "  really will clone: " << collection << endl;
            const char * from_name = collection["name"].valuestr();
            BSONObj options = collection.getObjectField("options");
            
            /* change name "<fromdb>.collection" -> <todb>.collection */
            const char *p = strchr(from_name, '.');
            assert(p);
            string to_name = todb + p;

            bool wantIdIndex = false;
            {
                string err;
                const char *toname = to_name.c_str();
                /* we defer building id index for performance - building it in batch is much faster */ 
                userCreateNS(toname, options, err, logForRepl, &wantIdIndex);
            }
            log(1) << "\t\t cloning " << from_name << " -> " << to_name << endl;
            Query q;
            if( snapshot ) 
                q.snapshot();
            copy(from_name, to_name.c_str(), false, logForRepl, masterSameProcess, slaveOk, q);

            if( wantIdIndex ) {
                /* we need dropDups to be true as we didn't do a true snapshot and this is before applying oplog operations 
                   that occur during the initial sync.  inDBRepair makes dropDups be true.
                   */
                bool old = inDBRepair;
                try {
                    inDBRepair = true;
                    ensureIdIndexForNewNs(to_name.c_str());
                    inDBRepair = old;
                }
                catch(...) { 
                    inDBRepair = old;
                    throw;
                }
            }
        }

        // now build the indexes

        string system_indexes_from = fromdb + ".system.indexes";
        string system_indexes_to = todb + ".system.indexes";
        /* [dm]: is the ID index sometimes not called "_id_"?  There is other code in the system that looks for a "_id" prefix 
                 rather than this exact value.  we should standardize.  OR, remove names - which is in the bugdb.  Anyway, this 
                 is dubious here at the moment.
        */
        copy(system_indexes_from.c_str(), system_indexes_to.c_str(), true, logForRepl, masterSameProcess, slaveOk, BSON( "name" << NE << "_id_" ) );

        return true;
    }
    
    /* slaveOk     - if true it is ok if the source of the data is !ismaster.
       useReplAuth - use the credentials we normally use as a replication slave for the cloning
       snapshot    - use $snapshot mode for copying collections.  note this should not be used when it isn't required, as it will be slower.
                     for example repairDatabase need not use it.
    */
    bool cloneFrom(const char *masterHost, string& errmsg, const string& fromdb, bool logForReplication, 
				   bool slaveOk, bool useReplAuth, bool snapshot)
    {
        Cloner c;
        return c.go(masterHost, errmsg, fromdb, logForReplication, slaveOk, useReplAuth, snapshot);
    }
    
    /* Usage:
       mydb.$cmd.findOne( { clone: "fromhost" } );
    */
    class CmdClone : public Command {
    public:
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream &help ) const {
            help << "clone this database from an instance of the db on another host\n";
            help << "{ clone : \"host13\" }";
        }
        CmdClone() : Command("clone") { }
        virtual bool run(const string& dbname , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string from = cmdObj.getStringField("clone");
            if ( from.empty() )
                return false;
            /* replication note: we must logOp() not the command, but the cloned data -- if the slave
               were to clone it would get a different point-in-time and not match.
               */
            return cloneFrom(from.c_str(), errmsg, dbname, 
                             /*logForReplication=*/!fromRepl, /*slaveok*/false, /*usereplauth*/false, /*snapshot*/true);
        }
    } cmdclone;
    
    class CmdCloneCollection : public Command {
    public:
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return NONE; }
        CmdCloneCollection() : Command("cloneCollection") { }
        virtual void help( stringstream &help ) const {
            help << "{ cloneCollection: <namespace>, from: <host> [,query: <query_filter>] [,copyIndexes:<bool>] }"
                "\nCopies a collection from one server to another. Do not use on a single server as the destination "
                "is placed at the same db.collection (namespace) as the source.\n"
                "Warning: the local copy of 'ns' is emptied before the copying begins. Any existing data will be lost there."
                ;
        }
        virtual bool run(const string& dbname , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string fromhost = cmdObj.getStringField("from");
            if ( fromhost.empty() ) {
                errmsg = "missing 'from' parameter";
                return false;
            }
            {
                HostAndPort h(fromhost);
                if( h.isSelf() ) { 
                    errmsg = "can't cloneCollection from self";
                    return false;
                }
            }
            string collection = cmdObj.getStringField("cloneCollection");
            if ( collection.empty() ) {
                errmsg = "bad 'cloneCollection' value";
                return false;
            }
            BSONObj query = cmdObj.getObjectField("query");
            if ( query.isEmpty() )
                query = BSONObj();
            
            BSONElement copyIndexesSpec = cmdObj.getField("copyindexes");
            bool copyIndexes = copyIndexesSpec.isBoolean() ? copyIndexesSpec.boolean() : true;
            
            log() << "cloneCollection.  db:" << dbname << " collection:" << collection << " from: " << fromhost 
                  << " query: " << query << " " << ( copyIndexes ? "" : ", not copying indexes" ) << endl;
            
            Cloner c;
            return c.copyCollection( fromhost , collection , query, errmsg , copyIndexes );
        }
    } cmdclonecollection;


    thread_specific_ptr< DBClientConnection > authConn_;
    /* Usage:
     admindb.$cmd.findOne( { copydbgetnonce: 1, fromhost: <hostname> } );
     */
    class CmdCopyDbGetNonce : public Command {
    public:
        CmdCopyDbGetNonce() : Command("copydbgetnonce") { }
        virtual bool adminOnly() const {
            return true;
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream &help ) const {
            help << "get a nonce for subsequent copy db request from secure server\n";
            help << "usage: {copydbgetnonce: 1, fromhost: <hostname>}";
        }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string fromhost = cmdObj.getStringField("fromhost");
            if ( fromhost.empty() ) {
                /* copy from self */
                stringstream ss;
                ss << "localhost:" << cmdLine.port;
                fromhost = ss.str();
            }
            authConn_.reset( new DBClientConnection() );
            BSONObj ret;
            {
                dbtemprelease t;
                if ( !authConn_->connect( fromhost, errmsg ) )
                    return false;
                if( !authConn_->runCommand( "admin", BSON( "getnonce" << 1 ), ret ) ) {
                    errmsg = "couldn't get nonce " + ret.toString();
                    return false;
                }
            }
            result.appendElements( ret );
            return true;
        }
    } cmdcopydbgetnonce;

    /* Usage:
       admindb.$cmd.findOne( { copydb: 1, fromhost: <hostname>, fromdb: <db>, todb: <db>[, username: <username>, nonce: <nonce>, key: <key>] } );
    */
    class CmdCopyDb : public Command {
    public:
        CmdCopyDb() : Command("copydb") { }
        virtual bool adminOnly() const {
            return true;
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream &help ) const {
            help << "copy a database from another host to this host\n";
            help << "usage: {copydb: 1, fromhost: <hostname>, fromdb: <db>, todb: <db>[, username: <username>, nonce: <nonce>, key: <key>]}";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string fromhost = cmdObj.getStringField("fromhost");
            if ( fromhost.empty() ) {
                /* copy from self */
                stringstream ss;
                ss << "localhost:" << cmdLine.port;
                fromhost = ss.str();
            }
            string fromdb = cmdObj.getStringField("fromdb");
            string todb = cmdObj.getStringField("todb");
            if ( fromhost.empty() || todb.empty() || fromdb.empty() ) {
                errmsg = "parms missing - {copydb: 1, fromhost: <hostname>, fromdb: <db>, todb: <db>}";
                return false;
            }
            Cloner c;
            string username = cmdObj.getStringField( "username" );
            string nonce = cmdObj.getStringField( "nonce" );
            string key = cmdObj.getStringField( "key" );
            if ( !username.empty() && !nonce.empty() && !key.empty() ) {
                uassert( 13008, "must call copydbgetnonce first", authConn_.get() );
                BSONObj ret;
                {
                    dbtemprelease t;
                    if ( !authConn_->runCommand( fromdb, BSON( "authenticate" << 1 << "user" << username << "nonce" << nonce << "key" << key ), ret ) ) {
                        errmsg = "unable to login " + ret.toString();
                        return false;
                    }
                }
                c.setConnection( authConn_.release() );
            }
            Client::Context ctx(todb);
            bool res = c.go(fromhost.c_str(), errmsg, fromdb, /*logForReplication=*/!fromRepl, /*slaveok*/false, /*replauth*/false, /*snapshot*/true);
            return res;
        }
    } cmdcopydb;
    
    class CmdRenameCollection : public Command {
    public:
        CmdRenameCollection() : Command( "renameCollection" ) {}
        virtual bool adminOnly() const {
            return true;
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return WRITE; }
        virtual bool logTheOp() {
            return true; // can't log steps when doing fast rename within a db, so always log the op rather than individual steps comprising it.
        }
        virtual void help( stringstream &help ) const {
            help << " example: { renameCollection: foo.a, to: bar.b }";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string source = cmdObj.getStringField( name.c_str() );
            string target = cmdObj.getStringField( "to" );
            if ( source.empty() || target.empty() ) {
                errmsg = "invalid command syntax";
                return false;
            }
            
            bool capped = false;
            long long size = 0;
            {
                Client::Context ctx( source );
                NamespaceDetails *nsd = nsdetails( source.c_str() );
                uassert( 10026 ,  "source namespace does not exist", nsd );
                capped = nsd->capped;
                if ( capped )
                    for( DiskLoc i = nsd->firstExtent; !i.isNull(); i = i.ext()->xnext )
                        size += i.ext()->length;
            }
            
            Client::Context ctx( target );
            
            if ( nsdetails( target.c_str() ) ){
                uassert( 10027 ,  "target namespace exists", cmdObj["dropTarget"].trueValue() );
                BSONObjBuilder bb( result.subobjStart( "dropTarget" ) );
                dropCollection( target , errmsg , bb );
                bb.done();
                if ( errmsg.size() > 0 )
                    return false;
            }

            {
                char from[256];
                nsToDatabase( source.c_str(), from );
                char to[256];
                nsToDatabase( target.c_str(), to );
                if ( strcmp( from, to ) == 0 ) {
                    renameNamespace( source.c_str(), target.c_str() );
                    return true;
                }
            }

            BSONObjBuilder spec;
            if ( capped ) {
                spec.appendBool( "capped", true );
                spec.append( "size", double( size ) );
            }
            if ( !userCreateNS( target.c_str(), spec.done(), errmsg, false ) )
                return false;
            
            auto_ptr< DBClientCursor > c;
            DBDirectClient bridge;

            {
                c = bridge.query( source, BSONObj() );
            }
            while( 1 ) {
                {
                    if ( !c->more() )
                        break;
                }
                BSONObj o = c->next();
                theDataFileMgr.insertWithObjMod( target.c_str(), o );
            }
            
            char cl[256];
            nsToDatabase( source.c_str(), cl );
            string sourceIndexes = string( cl ) + ".system.indexes";
            nsToDatabase( target.c_str(), cl );
            string targetIndexes = string( cl ) + ".system.indexes";
            {
                c = bridge.query( sourceIndexes, QUERY( "ns" << source ) );
            }
            while( 1 ) {
                {
                    if ( !c->more() )
                        break;
                }
                BSONObj o = c->next();
                BSONObjBuilder b;
                BSONObjIterator i( o );
                while( i.moreWithEOO() ) {
                    BSONElement e = i.next();
                    if ( e.eoo() )
                        break;
                    if ( strcmp( e.fieldName(), "ns" ) == 0 ) {
                        b.append( "ns", target );
                    } else {
                        b.append( e );
                    }
                }
                BSONObj n = b.done();
                theDataFileMgr.insertWithObjMod( targetIndexes.c_str(), n );
            }

            {
                Client::Context ctx( source );
                dropCollection( source, errmsg, result );
            }
            return true;
        }
    } cmdrenamecollection;

} // namespace mongo
