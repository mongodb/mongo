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

#include "stdafx.h"
#include "pdfile.h"
#include "../client/dbclient.h"
#include "../util/builder.h"
#include "jsobj.h"
#include "query.h"
#include "commands.h"
#include "db.h"
#include "instance.h"
#include "repl.h"

namespace mongo {

    void ensureHaveIdIndex(const char *ns);

    bool replAuthenticate(DBClientConnection *);

    class Cloner: boost::noncopyable {
        auto_ptr< DBClientWithCommands > conn;
        void copy(const char *from_ns, const char *to_ns, bool isindex, bool logForRepl,
                  bool masterSameProcess, bool slaveOk, Query q = Query());
        void replayOpLog( DBClientCursor *c, const BSONObj &query );
    public:
        Cloner() { }

        /* slaveOk     - if true it is ok if the source of the data is !ismaster.
           useReplAuth - use the credentials we normally use as a replication slave for the cloning
           snapshot    - use $snapshot mode for copying collections.  note this should not be used when it isn't required, as it will be slower.
                         for example repairDatabase need not use it.
        */
        bool go(const char *masterHost, string& errmsg, const string& fromdb, bool logForRepl, bool slaveOk, bool useReplAuth, bool snapshot);
        bool startCloneCollection( const char *fromhost, const char *ns, const BSONObj &query, string& errmsg, bool logForRepl, bool copyIndexes, int logSizeMb, long long &cursorId );
        bool finishCloneCollection( const char *fromhost, const char *ns, const BSONObj &query, long long cursorId, string &errmsg );
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
                uassert("bad ns field for index during dbcopy", e.type() == String);
                const char *p = strchr(e.valuestr(), '.');
                uassert("bad ns field for index during dbcopy [2]", p);
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

    /* copy the specified collection
       isindex - if true, this is system.indexes collection, in which we do some transformation when copying.
    */
    void Cloner::copy(const char *from_collection, const char *to_collection, bool isindex, bool logForRepl, bool masterSameProcess, bool slaveOk, Query query) {
        auto_ptr<DBClientCursor> c;
        {
            dbtemprelease r;
            c = conn->query( from_collection, query, 0, 0, 0, Option_NoCursorTimeout | ( slaveOk ? Option_SlaveOk : 0 ) );
        }
        
        list<BSONObj> storedForLater;
        
        assert( c.get() );
        long long n = 0;
        time_t saveLast = time( 0 );
        while ( 1 ) {
            {
                dbtemprelease r;
                if ( !c->more() )
                    break;
            }
            BSONObj tmp = c->next();

            /* assure object is valid.  note this will slow us down a little. */
            if ( !tmp.valid() ) {
                out() << "skipping corrupt object from " << from_collection << '\n';
                continue;
            }

            ++n;
            
            BSONObj js = tmp;
            if ( isindex ) {
                assert( strstr(from_collection, "system.indexes") );
                js = fixindex(tmp);
                storedForLater.push_back( js.getOwned() );
                continue;
            }

            try { 
                theDataFileMgr.insert(to_collection, js);
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

        if ( storedForLater.size() ){
            for ( list<BSONObj>::iterator i = storedForLater.begin(); i!=storedForLater.end(); i++ ){
                BSONObj js = *i;
                try { 
                    theDataFileMgr.insert(to_collection, js);
                    if ( logForRepl )
                        logOp("i", to_collection, js);
                }
                catch( UserException& e ) { 
                    log() << "warning: exception cloning object in " << from_collection << ' ' << e.what() << " obj:" << js.toString() << '\n';
                }
            }
        }
    }
    
    bool Cloner::go(const char *masterHost, string& errmsg, const string& fromdb, bool logForRepl, bool slaveOk, bool useReplAuth, bool snapshot) {

		massert( "useReplAuth is not written to replication log", !useReplAuth || !logForRepl );

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
		
            auto_ptr<DBClientCursor> c;
            {
                if ( !masterSameProcess ) {
                    auto_ptr< DBClientConnection > c( new DBClientConnection() );
                    if ( !c->connect( masterHost, errmsg ) )
                        return false;
                    if( !replAuthenticate(c.get()) )
                        return false;
                    
                    conn = c;
                } else {
                    conn.reset( new DBDirectClient() );
                }
                c = conn->query( ns.c_str(), BSONObj(), 0, 0, 0, slaveOk ? Option_SlaveOk : 0 );
            }

            if ( c.get() == 0 ) {
                errmsg = "query failed " + ns;
                return false;
            }
            
            while ( c->more() ){
                BSONObj collection = c->next();

                log(2) << "\t cloner got " << collection << endl;

                BSONElement e = collection.findElement("name");
                if ( e.eoo() ) {
                    string s = "bad system.namespaces object " + collection.toString();
                    massert(s.c_str(), false);
                }
                assert( !e.eoo() );
                assert( e.type() == String );
                const char *from_name = e.valuestr();

                if( strstr(from_name, ".system.") ) { 
                    /* system.users is cloned -- but nothing else from system. */
                    if( legalClientSystemNS( from_name , true ) == 0 ){
                        log(2) << "\t\t not cloning because system collection" << endl;
                        continue;
                    }
                }
                else if( strchr(from_name, '$') ) {
                    // don't clone index namespaces -- we take care of those separately below.
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

            {
                string err;
                const char *toname = to_name.c_str();
                userCreateNS(toname, options, err, logForRepl);
            }
            log(1) << "\t\t cloning " << from_name << " -> " << to_name << endl;
            Query q;
            if( snapshot ) 
                q.snapshot();
            copy(from_name, to_name.c_str(), false, logForRepl, masterSameProcess, slaveOk, q);
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

    bool Cloner::startCloneCollection( const char *fromhost, const char *ns, const BSONObj &query, string &errmsg, bool logForRepl, bool copyIndexes, int logSizeMb, long long &cursorId ) {
        char db[256];
        nsToClient( ns, db );

        NamespaceDetails *nsd = nsdetails( ns );
        if ( nsd ){
            /** note: its ok to clone into a collection, but only if the range you're copying 
                doesn't exist on this server */
            string err;
            if ( runCount( ns , BSON( "query" << query ) , err ) > 0 ){
                log() << "WARNING: data already exists for: " << ns << " in range : " << query << " deleting..." << endl;
                deleteObjects( ns , query , false , logForRepl , false );
            }
        }

        {
            dbtemprelease r;
            auto_ptr< DBClientConnection > c( new DBClientConnection() );
            if ( !c->connect( fromhost, errmsg ) )
                return false;
            if( !replAuthenticate(c.get()) )
                return false;
            conn = c;

            // Start temporary op log
            BSONObjBuilder cmdSpec;
            cmdSpec << "logCollection" << ns << "start" << 1;
            if ( logSizeMb != INT_MIN )
                cmdSpec << "logSizeMb" << logSizeMb;
            BSONObj info;
            if ( !conn->runCommand( db, cmdSpec.done(), info ) ) {
                errmsg = "logCollection failed: " + (string)info;
                return false;
            }
        }
        
        if ( ! nsd ) {
            BSONObj spec = conn->findOne( string( db ) + ".system.namespaces", BSON( "name" << ns ) );
            if ( !userCreateNS( ns, spec.getObjectField( "options" ), errmsg, true ) )
                return false;
        }

        copy( ns, ns, false, logForRepl, false, false, query );

        if ( copyIndexes ) {
            string indexNs = string( db ) + ".system.indexes";
            copy( indexNs.c_str(), indexNs.c_str(), true, logForRepl, false, false, BSON( "ns" << ns << "name" << NE << "_id_" ) );
        }
        
        auto_ptr< DBClientCursor > c;
        {
            dbtemprelease r;
            string logNS = "local.temp.oplog." + string( ns );
            c = conn->query( logNS.c_str(), Query(), 0, 0, 0, Option_CursorTailable );
        }
        if ( c->more() ) {
            replayOpLog( c.get(), query );
            cursorId = c->getCursorId();
            massert( "Expected valid tailing cursor", cursorId != 0 );
        } else {
            massert( "Did not expect valid cursor for empty query result", c->getCursorId() == 0 );
            cursorId = 0;
        }
        c->decouple();
        return true;
    }
    
    void Cloner::replayOpLog( DBClientCursor *c, const BSONObj &query ) {
        JSMatcher matcher( query );
        while( 1 ) {
            BSONObj op;
            {
                dbtemprelease t;
                if ( !c->more() )
                    break;
                op = c->next();
            }
            // For sharding v1.0, we don't allow shard key updates -- so just
            // filter each insert by value.
            if ( op.getStringField( "op" )[ 0 ] != 'i' || matcher.matches( op.getObjectField( "o" ) ) )
                ReplSource::applyOperation( op );
        }        
    }
    
    bool Cloner::finishCloneCollection( const char *fromhost, const char *ns, const BSONObj &query, long long cursorId, string &errmsg ) {
        char db[256];
        nsToClient( ns, db );

        auto_ptr< DBClientCursor > cur;
        {
            dbtemprelease r;
            auto_ptr< DBClientConnection > c( new DBClientConnection() );
            if ( !c->connect( fromhost, errmsg ) )
                return false;
            if( !replAuthenticate(c.get()) )
                return false;
            conn = c;            
            string logNS = "local.temp.oplog." + string( ns );
            if ( cursorId != 0 )
                cur = conn->getMore( logNS.c_str(), cursorId );
            else
                cur = conn->query( logNS.c_str(), Query() );
        }
        replayOpLog( cur.get(), query );
        {
            dbtemprelease t;
            BSONObj info;
            if ( !conn->runCommand( db, BSON( "logCollection" << ns << "validateComplete" << 1 ), info ) ) {
                errmsg = "logCollection failed: " + (string)info;
                return false;
            }
        }
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
        virtual bool slaveOk() {
            return false;
        }
        virtual void help( stringstream &help ) const {
            help << "clone this database from an instance of the db on another host\n";
            help << "example: { clone : \"host13\" }";
        }
        CmdClone() : Command("clone") { }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string from = cmdObj.getStringField("clone");
            if ( from.empty() )
                return false;
            /* replication note: we must logOp() not the command, but the cloned data -- if the slave
               were to clone it would get a different point-in-time and not match.
               */
            return cloneFrom(from.c_str(), errmsg, cc().database()->name, 
                             /*logForReplication=*/!fromRepl, /*slaveok*/false, /*usereplauth*/false, /*snapshot*/true);
        }
    } cmdclone;
    
    class CmdCloneCollection : public Command {
    public:
        virtual bool slaveOk() {
            return false;
        }
        CmdCloneCollection() : Command("cloneCollection") { }
        virtual void help( stringstream &help ) const {
            help << " example: { cloneCollection: <collection ns>, from: <hostname>, query: <query> }";
        }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string fromhost = cmdObj.getStringField("from");
            if ( fromhost.empty() ) {
                errmsg = "missing from spec";
                return false;
            }
            string collection = cmdObj.getStringField("cloneCollection");
            if ( collection.empty() ) {
                errmsg = "missing cloneCollection spec";
                return false;
            }
            BSONObj query = cmdObj.getObjectField("query");
            if ( query.isEmpty() )
                query = BSONObj();
            BSONElement copyIndexesSpec = cmdObj.getField("copyindexes");
            bool copyIndexes = copyIndexesSpec.isBoolean() ? copyIndexesSpec.boolean() : true;
            // Will not be used if doesn't exist.
            int logSizeMb = cmdObj.getIntField( "logSizeMb" );
            
            /* replication note: we must logOp() not the command, but the cloned data -- if the slave
             were to clone it would get a different point-in-time and not match.
             */
            setClient( collection.c_str() );
            
            log() << "cloneCollection.  db:" << ns << " collection:" << collection << " from: " << fromhost << " query: " << query << ( copyIndexes ? "" : ", not copying indexes" ) << endl;
            
            Cloner c;
            long long cursorId;
            if ( !c.startCloneCollection( fromhost.c_str(), collection.c_str(), query, errmsg, !fromRepl, copyIndexes, logSizeMb, cursorId ) )
                return false;
            return c.finishCloneCollection( fromhost.c_str(), collection.c_str(), query, cursorId, errmsg);
        }
    } cmdclonecollection;

    class CmdStartCloneCollection : public Command {
    public:
        virtual bool slaveOk() {
            return false;
        }
        CmdStartCloneCollection() : Command("startCloneCollection") { }
        virtual void help( stringstream &help ) const {
            help << " example: { startCloneCollection: <collection ns>, from: <hostname>, query: <query> }";
            help << ", returned object includes a finishToken field, the value of which may be passed to the finishCloneCollection command";
        }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string fromhost = cmdObj.getStringField("from");
            if ( fromhost.empty() ) {
                errmsg = "missing from spec";
                return false;
            }
            string collection = cmdObj.getStringField("startCloneCollection");
            if ( collection.empty() ) {
                errmsg = "missing startCloneCollection spec";
                return false;
            }
            BSONObj query = cmdObj.getObjectField("query");
            if ( query.isEmpty() )
                query = BSONObj();
            BSONElement copyIndexesSpec = cmdObj.getField("copyindexes");
            bool copyIndexes = copyIndexesSpec.isBoolean() ? copyIndexesSpec.boolean() : true;
            // Will not be used if doesn't exist.
            int logSizeMb = cmdObj.getIntField( "logSizeMb" );
            
            /* replication note: we must logOp() not the command, but the cloned data -- if the slave
             were to clone it would get a different point-in-time and not match.
             */
            setClient( collection.c_str() );
            
            log() << "startCloneCollection.  db:" << ns << " collection:" << collection << " from: " << fromhost << " query: " << query << endl;
            
            Cloner c;
            long long cursorId;
            bool res = c.startCloneCollection( fromhost.c_str(), collection.c_str(), query, errmsg, !fromRepl, copyIndexes, logSizeMb, cursorId );
            
            if ( res ) {
                BSONObjBuilder b;
                b << "fromhost" << fromhost;
                b << "collection" << collection;
                b << "query" << query;
                b.appendDate( "cursorId", cursorId );
                BSONObj token = b.done();
                result << "finishToken" << token;
            }
            return res;
        }
    } cmdstartclonecollection;
    
    class CmdFinishCloneCollection : public Command {
    public:
        virtual bool slaveOk() {
            return false;
        }
        CmdFinishCloneCollection() : Command("finishCloneCollection") { }
        virtual void help( stringstream &help ) const {
            help << " example: { finishCloneCollection: <finishToken> }";
        }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONObj fromToken = cmdObj.getObjectField("finishCloneCollection");
            if ( fromToken.isEmpty() ) {
                errmsg = "missing finishCloneCollection finishToken spec";
                return false;
            }
            string fromhost = fromToken.getStringField( "fromhost" );
            if ( fromhost.empty() ) {
                errmsg = "missing fromhost spec";
                return false;
            }
            string collection = fromToken.getStringField("collection");
            if ( collection.empty() ) {
                errmsg = "missing collection spec";
                return false;
            }
            BSONObj query = fromToken.getObjectField("query");
            if ( query.isEmpty() ) {
                query = BSONObj();
            }
            long long cursorId = 0;
            BSONElement cursorIdToken = fromToken.getField( "cursorId" );
            if ( cursorIdToken.type() == Date ) {
                cursorId = cursorIdToken.date();
            }
            
            setClient( collection.c_str() );
            
            log() << "finishCloneCollection.  db:" << ns << " collection:" << collection << " from: " << fromhost << " query: " << query << endl;
            
            Cloner c;
            return c.finishCloneCollection( fromhost.c_str(), collection.c_str(), query, cursorId, errmsg );
        }
    } cmdfinishclonecollection;

    /* Usage:
       admindb.$cmd.findOne( { copydb: 1, fromhost: <hostname>, fromdb: <db>, todb: <db> } );
    */
    class CmdCopyDb : public Command {
    public:
        CmdCopyDb() : Command("copydb") { }
        virtual bool adminOnly() {
            return true;
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual void help( stringstream &help ) const {
            help << "copy a database from antoher host to this host\n";
            help << "usage: {copydb: 1, fromhost: <hostname>, fromdb: <db>, todb: <db>}";
        }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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
            setClient(todb.c_str());
            bool res = cloneFrom(fromhost.c_str(), errmsg, fromdb, /*logForReplication=*/!fromRepl, /*slaveok*/false, /*replauth*/false, /*snapshot*/true);
            cc().clearns();
            return res;
        }
    } cmdcopydb;
    
    class CmdRenameCollection : public Command {
    public:
        CmdRenameCollection() : Command( "renameCollection" ) {}
        virtual bool adminOnly() {
            return true;
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual bool logTheOp() {
            return true; // can't log steps when doing fast rename within a db, so always log the op rather than individual steps comprising it.
        }
        virtual void help( stringstream &help ) const {
            help << " example: { renameCollection: foo.a, to: bar.b }";
        }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string source = cmdObj.getStringField( name.c_str() );
            string target = cmdObj.getStringField( "to" );
            if ( source.empty() || target.empty() ) {
                errmsg = "invalid command syntax";
                return false;
            }
            
            setClient( source.c_str() );
            NamespaceDetails *nsd = nsdetails( source.c_str() );
            uassert( "source namespace does not exist", nsd );
            bool capped = nsd->capped;
            long long size = 0;
            if ( capped )
                for( DiskLoc i = nsd->firstExtent; !i.isNull(); i = i.ext()->xnext )
                    size += i.ext()->length;
            
            setClient( target.c_str() );
            uassert( "target namespace exists", !nsdetails( target.c_str() ) );

            {
                char from[256];
                nsToClient( source.c_str(), from );
                char to[256];
                nsToClient( target.c_str(), to );
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
                theDataFileMgr.insert( target.c_str(), o );
            }
            
            char cl[256];
            nsToClient( source.c_str(), cl );
            string sourceIndexes = string( cl ) + ".system.indexes";
            nsToClient( target.c_str(), cl );
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
                theDataFileMgr.insert( targetIndexes.c_str(), n );
            }

            setClient( source.c_str() );
            dropCollection( source, errmsg, result );
            return true;
        }
    } cmdrenamecollection;

} // namespace mongo
