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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    BSONElement getErrField(const BSONObj& o);

    bool replAuthenticate(DBClientBase *, bool);

    /** Selectively release the mutex based on a parameter. */
    class dbtempreleaseif {
    public:
        dbtempreleaseif( bool release ) : _impl( release ? new dbtemprelease() : 0 ) {}
    private:
        shared_ptr< dbtemprelease > _impl;
    };
    
    void mayInterrupt( bool mayBeInterrupted ) {
     	if ( mayBeInterrupted ) {
         	killCurrentOp.checkForInterrupt( false );   
        }
    }

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

            // for now, skip the "v" field so that v:0 indexes will be upgraded to v:1
            if ( string("v") == e.fieldName() ) {
                continue;
            }

            if ( string("ns") == e.fieldName() ) {
                uassert( 10024 , "bad ns field for index during dbcopy", e.type() == String);
                const char *p = strchr(e.valuestr(), '.');
                uassert( 10025 , "bad ns field for index during dbcopy [2]", p);
                string newname = cc().database()->name() + p;
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

    Cloner::Cloner() { }

    struct Cloner::Fun {
        Fun() : lastLog(0) { }
        time_t lastLog;
        void operator()( DBClientCursorBatchIterator &i ) {
            Lock::GlobalWrite lk;
            if ( context ) {
                context->relocked();
            }

            while( i.moreInCurrentBatch() ) {
                if ( n % 128 == 127 /*yield some*/ ) {
                    time_t now = time(0);
                    if( now - lastLog >= 60 ) { 
                        // report progress
                        if( lastLog )
                            log() << "clone " << to_collection << ' ' << n << endl;
                        lastLog = now;
                    }
                    mayInterrupt( _mayBeInterrupted );
                    dbtempreleaseif t( _mayYield );
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
                    catch( ... ) {
                        ss << " firstElement corrupt";
                    }
                    out() << ss.str() << endl;
                    continue;
                }

                ++n;

                BSONObj js = tmp;
                if ( isindex ) {
                    verify(nsToCollectionSubstring(from_collection) == "system.indexes");
                    js = fixindex(tmp);
                    storedForLater->push_back( js.getOwned() );
                    continue;
                }

                try {
                    DiskLoc loc = theDataFileMgr.insertWithObjMod(to_collection, js);
                    loc.assertOk();
                    if ( logForRepl )
                        logOp("i", to_collection, js);

                    getDur().commitIfNeeded();
                }
                catch( UserException& e ) {
                    error() << "error: exception cloning object in " << from_collection << ' ' << e.what() << " obj:" << js.toString() << '\n';
                    throw;
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
        list<BSONObj> *storedForLater;  // deferred query results (e.g. index insert/build)
        bool logForRepl;
        Client::Context *context;
        bool _mayYield;
        bool _mayBeInterrupted;
    };

    /* copy the specified collection
       isindex - if true, this is system.indexes collection, in which we do some transformation when copying.
    */
    void Cloner::copy(const char *from_collection, const char *to_collection, bool isindex,
                      bool logForRepl, bool masterSameProcess, bool slaveOk, bool mayYield,
                      bool mayBeInterrupted, Query query) {

        list<BSONObj> storedForLater;
        LOG(2) << "\t\tcloning collection " << from_collection << " to " << to_collection << " on " << _conn->getServerAddress() << " with filter " << query.toString() << endl;

        Fun f;
        f.n = 0;
        f.isindex = isindex;
        f.from_collection = from_collection;
        f.to_collection = to_collection;
        f.saveLast = time( 0 );
        f.storedForLater = &storedForLater;
        f.logForRepl = logForRepl;
        f._mayYield = mayYield;
        f._mayBeInterrupted = mayBeInterrupted;

        int options = QueryOption_NoCursorTimeout | ( slaveOk ? QueryOption_SlaveOk : 0 );
        {
            f.context = cc().getContext();
            mayInterrupt( mayBeInterrupted );
            dbtempreleaseif r( mayYield );
            _conn->query(boost::function<void(DBClientCursorBatchIterator &)>(f), from_collection,
                         query, 0, options);
        }

        if ( storedForLater.size() ) {
            for (list<BSONObj>::const_iterator i = storedForLater.begin();
                 i != storedForLater.end();
                 ++i) {
                BSONObj js = *i;
                scoped_lock precalcLock(theDataFileMgr._precalcedMutex);
                try {
                    theDataFileMgr.insertWithObjMod(to_collection, js);
                    theDataFileMgr.setPrecalced(NULL);

                    if ( logForRepl )
                        logOp("i", to_collection, js);

                    getDur().commitIfNeeded();
                }
                catch( UserException& e ) {
                    theDataFileMgr.setPrecalced(NULL);
                    error() << "error: exception cloning object in " << from_collection << ' ' << e.what() << " obj:" << js.toString() << '\n';
                    throw;
                }
                catch(const DBException&) {
                    theDataFileMgr.setPrecalced(NULL);
                    throw;
                }
            }
        }
    }

    bool Cloner::validateQueryResults(const auto_ptr<DBClientCursor>& cur,
                                      int32_t* errCode,
                                      string& errmsg) {
        if ( cur.get() == 0 )
            return false;
        if ( cur->more() ) {
            BSONObj first = cur->next();
            BSONElement errField = getErrField(first);
            if(!errField.eoo()) {
                errmsg = errField.str();
                if (errCode)
                    *errCode = first.getIntField("code");
                return false;
            }
            cur->putBack(first);
        }
        return true;
    }

    bool Cloner::copyCollectionFromRemote(const string& host, const string& ns, string& errmsg) {
        Cloner cloner;

        DBClientConnection *tmpConn = new DBClientConnection();
        // cloner owns _conn in auto_ptr
        cloner.setConnection(tmpConn);
        uassert(15908, errmsg, tmpConn->connect(host, errmsg) && replAuthenticate(tmpConn, false));

        return cloner.copyCollection(ns, BSONObj(), errmsg, true, false, true, false);
    }

    bool Cloner::copyCollection(const string& ns, const BSONObj& query, string& errmsg,
                                bool mayYield, bool mayBeInterrupted, bool copyIndexes,
                                bool logForRepl) {

        Client::WriteContext ctx(ns);

        // config
        string temp = ctx.ctx().db()->name() + ".system.namespaces";
        BSONObj config = _conn->findOne(temp , BSON("name" << ns));
        if (config["options"].isABSONObj())
            if (!userCreateNS(ns.c_str(), config["options"].Obj(), errmsg, logForRepl, 0))
                return false;

        // main data
        copy(ns.c_str(), ns.c_str(), false, logForRepl, false, true, mayYield, mayBeInterrupted,
             Query(query).snapshot());

        /* TODO : copyIndexes bool does not seem to be implemented! */
        if(!copyIndexes) {
            log() << "ERROR copy collection copyIndexes not implemented? " << ns << endl;
        }

        // indexes
        temp = ctx.ctx().db()->name() + ".system.indexes";
        copy(temp.c_str(), temp.c_str(), true, logForRepl, false, true, mayYield, mayBeInterrupted,
             BSON( "ns" << ns ));

        getDur().commitIfNeeded();
        return true;
    }

    extern bool inDBRepair;
    void ensureIdIndexForNewNs(const char *ns);

    bool Cloner::go(const char *masterHost, string& errmsg, const string& fromdb, bool logForRepl, bool slaveOk, bool useReplAuth, bool snapshot, bool mayYield, bool mayBeInterrupted, int *errCode) {

        CloneOptions opts;

        opts.fromDB = fromdb;
        opts.logForRepl = logForRepl;
        opts.slaveOk = slaveOk;
        opts.useReplAuth = useReplAuth;
        opts.snapshot = snapshot;
        opts.mayYield = mayYield;
        opts.mayBeInterrupted = mayBeInterrupted;

        set<string> clonedColls;
        return go( masterHost, opts, clonedColls, errmsg, errCode );

    }

    bool Cloner::go(const char *masterHost, const CloneOptions& opts, set<string>& clonedColls,
                    string& errmsg, int* errCode) {
        if ( errCode ) {
            *errCode = 0;
        }
        massert( 10289 ,  "useReplAuth is not written to replication log", !opts.useReplAuth || !opts.logForRepl );

        string todb = cc().database()->name();
        stringstream a,b;
        a << "localhost:" << cmdLine.port;
        b << "127.0.0.1:" << cmdLine.port;
        bool masterSameProcess = ( a.str() == masterHost || b.str() == masterHost );
        if ( masterSameProcess ) {
            if ( opts.fromDB == todb && cc().database()->path() == dbpath ) {
                // guard against an "infinite" loop
                /* if you are replicating, the local.sources config may be wrong if you get this */
                errmsg = "can't clone from self (localhost).";
                return false;
            }
        }

        {
            // setup connection
            if (_conn.get()) {
                // nothing to do
            }
            else if ( !masterSameProcess ) {
                ConnectionString cs = ConnectionString::parse( masterHost, errmsg );
                auto_ptr<DBClientBase> con( cs.connect( errmsg ));
                if ( !con.get() )
                    return false;
                if( !replAuthenticate(con.get(), false) )
                    return false;
                
                _conn = con;
            }
            else {
                _conn.reset(new DBDirectClient());
            }
        }

        string ns = opts.fromDB + ".system.namespaces";
        string idxns = opts.fromDB + ".system.indexes";
        list<BSONObj> toClone;
        clonedColls.clear();
        if ( opts.syncData ) {
            /* todo: we can put these releases inside dbclient or a dbclient specialization.
               or just wait until we get rid of global lock anyway.
               */
            mayInterrupt( opts.mayBeInterrupted );
            dbtempreleaseif r( opts.mayYield );

            // just using exhaust for collection copying right now
            
            // todo: if snapshot (bool param to this func) is true, we need to snapshot this query?
            //       only would be relevant if a thousands of collections -- maybe even then it is hard
            //       to exceed a single cursor batch.
            //       for repl it is probably ok as we apply oplog section after the clone (i.e. repl 
            //       doesnt not use snapshot=true).
            auto_ptr<DBClientCursor> cursor = _conn->query(ns.c_str(), BSONObj(), 0, 0, 0,
                                                      opts.slaveOk ? QueryOption_SlaveOk : 0);

            if (!validateQueryResults(cursor, errCode, errmsg)) {
                errmsg = "index query on ns " + ns + " failed: " + errmsg;
                return false;
            }

            while ( cursor->more() ) {
                BSONObj collection = cursor->next();

                LOG(2) << "\t cloner got " << collection << endl;

                BSONElement e = collection.getField("name");
                if ( e.eoo() ) {
                    string s = "bad system.namespaces object " + collection.toString();
                    massert( 10290 , s.c_str(), false);
                }
                verify( !e.eoo() );
                verify( e.type() == String );
                const char *from_name = e.valuestr();

                if( strstr(from_name, ".system.") ) {
                    /* system.users and s.js is cloned -- but nothing else from system.
                     * system.indexes is handled specially at the end*/
                    if( legalClientSystemNS( from_name , true ) == 0 ) {
                        LOG(2) << "\t\t not cloning because system collection" << endl;
                        continue;
                    }
                }
                if( ! NamespaceString::normal( from_name ) ) {
                    LOG(2) << "\t\t not cloning because has $ " << endl;
                    continue;
                }

                if( opts.collsToIgnore.find( string( from_name ) ) != opts.collsToIgnore.end() ){
                    LOG(2) << "\t\t ignoring collection " << from_name << endl;
                    continue;
                }
                else {
                    LOG(2) << "\t\t not ignoring collection " << from_name << endl;
                }

                clonedColls.insert( from_name );
                toClone.push_back( collection.getOwned() );
            }
        }

        for ( list<BSONObj>::iterator i=toClone.begin(); i != toClone.end(); i++ ) {
            {
                mayInterrupt( opts.mayBeInterrupted );
                dbtempreleaseif r( opts.mayYield );
            }
            BSONObj collection = *i;
            LOG(2) << "  really will clone: " << collection << endl;
            const char * from_name = collection["name"].valuestr();
            BSONObj options = collection.getObjectField("options");

            /* change name "<fromdb>.collection" -> <todb>.collection */
            const char *p = strchr(from_name, '.');
            verify(p);
            string to_name = todb + p;

            bool wantIdIndex = false;
            {
                string err;
                const char *toname = to_name.c_str();
                /* we defer building id index for performance - building it in batch is much faster */
                userCreateNS(toname, options, err, opts.logForRepl, &wantIdIndex);
            }
            LOG(1) << "\t\t cloning " << from_name << " -> " << to_name << endl;
            Query q;
            if( opts.snapshot )
                q.snapshot();
            copy(from_name, to_name.c_str(), false, opts.logForRepl, masterSameProcess, opts.slaveOk, opts.mayYield, opts.mayBeInterrupted, q);

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
        
        if ( opts.syncIndexes ) {
            string system_indexes_from = opts.fromDB + ".system.indexes";
            string system_indexes_to = todb + ".system.indexes";
            
            /* [dm]: is the ID index sometimes not called "_id_"?  There is other code in the system that looks for a "_id" prefix
               rather than this exact value.  we should standardize.  OR, remove names - which is in the bugdb.  Anyway, this
               is dubious here at the moment.
            */
            
            // build a $nin query filter for the collections we *don't* want
            BSONArrayBuilder barr;
            barr.append( opts.collsToIgnore );
            BSONArray arr = barr.arr();
            
            // Also don't copy the _id_ index
            BSONObj query = BSON( "name" << NE << "_id_" << "ns" << NIN << arr );
            
            // won't need a snapshot of the query of system.indexes as there can never be very many.
            copy(system_indexes_from.c_str(), system_indexes_to.c_str(), true, opts.logForRepl, masterSameProcess, opts.slaveOk, opts.mayYield, opts.mayBeInterrupted, query );
        }
        return true;
    }

    // same as above, but ignores the collection names
    bool Cloner::go(const char *masterHost, const CloneOptions& opts, string& errmsg, 
                    int *errCode) {
        set<string> unusedCollections;
        return go(masterHost, opts, unusedCollections, errmsg, errCode);
    }

    bool Cloner::cloneFrom(const char *masterHost, string& errmsg, const string& fromdb,
                           bool logForReplication, bool slaveOk, bool useReplAuth, bool snapshot,
                           bool mayYield, bool mayBeInterrupted, int *errCode) {
        Cloner cloner;
        return cloner.go(masterHost, errmsg, fromdb, logForReplication, slaveOk, useReplAuth, snapshot,
                    mayYield, mayBeInterrupted, errCode);
    }

    bool Cloner::cloneFrom(const string& masterHost, const CloneOptions& options,
                           string& errmsg, int* errCode, set<string>* clonedCollections) {
        scoped_ptr< set<string> > myset;
        if (!clonedCollections) {
            myset.reset(new set<string>());
            clonedCollections = myset.get();
        }
        
        Cloner cloner;
        return cloner.go(masterHost.c_str(), options, *clonedCollections, errmsg, errCode);
    }

    /* Usage:
       mydb.$cmd.findOne( { clone: "fromhost" } );
       Note: doesn't work with authentication enabled, except as internal operation or for
       old-style users for backwards compatibility.
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
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // Note: privileges required are currently only granted to old-style users for backwards
            // compatibility, and to internal connections (used in movePrimary).
            ActionSet actions;
            actions.addAction(ActionType::clone);
            out->push_back(Privilege(dbname, actions));
        }
        CmdClone() : Command("clone") { }
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string from = cmdObj.getStringField("clone");
            if ( from.empty() )
                return false;

            CloneOptions opts;
            opts.fromDB = dbname;
            opts.logForRepl = ! fromRepl;

            // See if there's any collections we should ignore
            if( cmdObj["collsToIgnore"].type() == Array ){
                BSONObjIterator it( cmdObj["collsToIgnore"].Obj() );

                while( it.more() ){
                    BSONElement e = it.next();
                    if( e.type() == String ){
                        opts.collsToIgnore.insert( e.String() );
                    }
                }
            }

            Cloner cloner;
            set<string> clonedColls;
            bool rval = cloner.go(from.c_str(), opts, clonedColls, errmsg);

            BSONArrayBuilder barr;
            barr.append( clonedColls );

            result.append( "clonedColls", barr.arr() );

            return rval;

        }
    } cmdClone;

    class CmdCloneCollection : public Command {
    public:
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return NONE; }
        CmdCloneCollection() : Command("cloneCollection") { }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // Will fail if source instance has auth on.
            string collection = cmdObj.getStringField("cloneCollection");
            uassert(16709, "bad 'cloneCollection' value", !collection.empty());

            ActionSet actions;
            actions.addAction(ActionType::cloneCollectionTarget);
            out->push_back(Privilege(collection, actions));
        }
        virtual void help( stringstream &help ) const {
            help << "{ cloneCollection: <collection>, from: <host> [,query: <query_filter>] [,copyIndexes:<bool>] }"
                 "\nCopies a collection from one server to another. Do not use on a single server as the destination "
                 "is placed at the same db.collection (namespace) as the source.\n"
                 ;
        }
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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

            Cloner cloner;
            auto_ptr<DBClientConnection> myconn;
            myconn.reset( new DBClientConnection() );
            if ( ! myconn->connect( fromhost , errmsg ) )
                return false;

            cloner.setConnection( myconn.release() );

            return cloner.copyCollection(collection, query, errmsg, true, false, copyIndexes);
        }
    } cmdCloneCollection;


    // SERVER-4328 todo review for concurrency
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
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual void help( stringstream &help ) const {
            help << "get a nonce for subsequent copy db request from secure server\n";
            help << "usage: {copydbgetnonce: 1, fromhost: <hostname>}";
        }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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
    } cmdCopyDBGetNonce;

    /* Usage:
       admindb.$cmd.findOne( { copydb: 1, fromhost: <hostname>, fromdb: <db>, todb: <db>[, username: <username>, nonce: <nonce>, key: <key>] } );
       Note: doesn't work with authentication enabled, except as old-style users.
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
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // Note: privileges required are currently only granted to old-style users for backwards
            // compatibility, since we can't properly handle auth checking for the read from the
            // source DB.
            ActionSet actions;
            actions.addAction(ActionType::copyDBTarget);
            out->push_back(Privilege(dbname, actions)); // NOTE: dbname is always admin
        }
        virtual void help( stringstream &help ) const {
            help << "copy a database from another host to this host\n";
            help << "usage: {copydb: 1, fromhost: <hostname>, fromdb: <db>, todb: <db>[, slaveOk: <bool>, username: <username>, nonce: <nonce>, key: <key>]}";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            bool slaveOk = cmdObj["slaveOk"].trueValue();
            string fromhost = cmdObj.getStringField("fromhost");
            bool fromSelf = fromhost.empty();
            if ( fromSelf ) {
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

            // SERVER-4328 todo lock just the two db's not everything for the fromself case
            scoped_ptr<Lock::ScopedLock> lk( fromSelf ? 
                                             static_cast<Lock::ScopedLock*>( new Lock::GlobalWrite() ) : 
                                             static_cast<Lock::ScopedLock*>( new Lock::DBWrite( todb ) ) );

            Cloner cloner;
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
                cloner.setConnection( authConn_.release() );
            } else {
                DBClientConnection* conn = new DBClientConnection();
                cloner.setConnection(conn);
                if (!conn->connect(fromhost, errmsg)) {
                    return false;
                }
            }
            Client::Context ctx(todb);
            bool res = cloner.go(fromhost.c_str(), errmsg, fromdb, /*logForReplication=*/!fromRepl, slaveOk, /*replauth*/false, /*snapshot*/true, /*mayYield*/true, /*mayBeInterrupted*/ false);
            return res;
        }
    } cmdCopyDB;

} // namespace mongo
