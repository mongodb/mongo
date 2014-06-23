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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/storage_options.h"


namespace mongo {

    BSONElement getErrField(const BSONObj& o);

    /* for index info object:
         { "name" : "name_1" , "ns" : "foo.index3" , "key" :  { "name" : 1.0 } }
       we need to fix up the value in the "ns" parameter so that the name prefix is correct on a
       copy to a new name.
    */
    BSONObj fixindex(const string& newDbName, BSONObj o) {
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
                string newname = newDbName + p;
                b.append("ns", newname);
            }
            else
                b.append(e);
        }

        BSONObj res= b.obj();

        return res;
    }

    Cloner::Cloner() { }

    struct Cloner::Fun {
        Fun(OperationContext* txn, const string& dbName)
            :lastLog(0),
             txn(txn),
             _dbName(dbName)
        {}

        void operator()( DBClientCursorBatchIterator &i ) {
            // XXX: can probably take dblock instead
            Lock::GlobalWrite lk(txn->lockState());
            
            // Make sure database still exists after we resume from the temp release
            bool unused;
            Database* db = dbHolder().getOrCreate(txn, _dbName, unused);

            bool createdCollection = false;
            Collection* collection = NULL;

            if ( isindex == false ) {
                collection = db->getCollection( txn, to_collection );
                if ( !collection ) {
                    massert( 17321,
                             str::stream()
                             << "collection dropped during clone ["
                             << to_collection << "]",
                             !createdCollection );
                    createdCollection = true;
                    collection = db->createCollection( txn, to_collection );
                    verify( collection );
                }
            }

            while( i.moreInCurrentBatch() ) {
                if ( numSeen % 128 == 127 ) {
                    time_t now = time(0);
                    if( now - lastLog >= 60 ) {
                        // report progress
                        if( lastLog )
                            log() << "clone " << to_collection << ' ' << numSeen << endl;
                        lastLog = now;
                    }
                }

                BSONObj tmp = i.nextSafe();

                /* assure object is valid.  note this will slow us down a little. */
                const Status status = validateBSON(tmp.objdata(), tmp.objsize());
                if (!status.isOK()) {
                    log() << "Cloner: skipping corrupt object from " << from_collection
                          << ": " << status.reason();
                    continue;
                }

                ++numSeen;

                BSONObj js = tmp;
                if ( isindex ) {
                    verify(nsToCollectionSubstring(from_collection) == "system.indexes");
                    js = fixindex(db->name(), tmp);
                    indexesToBuild->push_back( js.getOwned() );
                    continue;
                }

                verify(nsToCollectionSubstring(from_collection) != "system.indexes");

                StatusWith<DiskLoc> loc = collection->insertDocument( txn, js, true );
                if ( !loc.isOK() ) {
                    error() << "error: exception cloning object in " << from_collection
                            << ' ' << loc.toString() << " obj:" << js;
                }
                uassertStatusOK( loc.getStatus() );
                if (logForRepl)
                    repl::logOp(txn, "i", to_collection, js);

                txn->recoveryUnit()->commitIfNeeded();

                RARELY if ( time( 0 ) - saveLast > 60 ) {
                    log() << numSeen << " objects cloned so far from collection " << from_collection;
                    saveLast = time( 0 );
                }
            }
        }

        time_t lastLog;
        OperationContext* txn;
        const string _dbName;

        int64_t numSeen;
        bool isindex;
        const char *from_collection;
        const char *to_collection;
        time_t saveLast;
        list<BSONObj> *indexesToBuild;  // deferred query results (e.g. index insert/build)
        bool logForRepl;
        bool _mayYield;
        bool _mayBeInterrupted;
    };

    /* copy the specified collection
       isindex - if true, this is system.indexes collection, in which we do some transformation when copying.
    */
    void Cloner::copy(OperationContext* txn,
                      const string& toDBName,
                      const char *from_collection,
                      const char *to_collection,
                      bool isindex,
                      bool logForRepl,
                      bool masterSameProcess,
                      bool slaveOk,
                      bool mayYield,
                      bool mayBeInterrupted,
                      Query query) {

        list<BSONObj> indexesToBuild;
        LOG(2) << "\t\tcloning collection " << from_collection << " to " << to_collection << " on " << _conn->getServerAddress() << " with filter " << query.toString() << endl;

        Fun f(txn, toDBName);
        f.numSeen = 0;
        f.isindex = isindex;
        f.from_collection = from_collection;
        f.to_collection = to_collection;
        f.saveLast = time( 0 );
        f.indexesToBuild = &indexesToBuild;
        f.logForRepl = logForRepl;
        f._mayYield = mayYield;
        f._mayBeInterrupted = mayBeInterrupted;

        int options = QueryOption_NoCursorTimeout | ( slaveOk ? QueryOption_SlaveOk : 0 );
        {
            Lock::TempRelease tempRelease(txn->lockState());
            _conn->query(stdx::function<void(DBClientCursorBatchIterator &)>(f), from_collection,
                         query, 0, options);
        }

        // We are under lock here again, so reload the database in case it may have disappeared
        // during the temp release
        bool unused;
        Database* db = dbHolder().getOrCreate(txn, toDBName, unused);

        if ( indexesToBuild.size() ) {
            for (list<BSONObj>::const_iterator i = indexesToBuild.begin();
                 i != indexesToBuild.end();
                 ++i) {

                BSONObj spec = *i;
                string ns = spec["ns"].String(); // this was fixed when pulled off network
                Collection* collection = db->getCollection( txn, ns );
                if ( !collection ) {
                    collection = db->createCollection( txn, ns );
                    verify( collection );
                }

                Status status = collection->getIndexCatalog()->createIndex(txn, spec, mayBeInterrupted);
                if ( status.code() == ErrorCodes::IndexAlreadyExists ) {
                    // no-op
                }
                else if ( !status.isOK() ) {
                    error() << "error creating index when cloning spec: " << spec
                            << " error: " << status.toString();
                    uassertStatusOK( status );
                }

                if (logForRepl)
                    repl::logOp(txn, "i", to_collection, spec);

                txn->recoveryUnit()->commitIfNeeded();

            }
        }
    }

    /**
     * validate the cloner query was successful
     * @param cur   Cursor the query was executed on
     * @param errCode out  Error code encountered during the query
     * @param errmsg out  Error message encountered during the query
     */
    bool validateQueryResults(const auto_ptr<DBClientCursor>& cur,
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

    bool Cloner::copyCollection(OperationContext* txn,
                                const string& ns,
                                const BSONObj& query,
                                string& errmsg,
                                bool mayYield,
                                bool mayBeInterrupted,
                                bool copyIndexes,
                                bool logForRepl) {

        const NamespaceString nss(ns);
        Lock::DBWrite dbWrite(txn->lockState(), nss.db());

        const string dbName = nss.db().toString();

        bool unused;
        Database* db = dbHolder().getOrCreate(txn, dbName, unused);

        // config
        string temp = dbName + ".system.namespaces";
        BSONObj config = _conn->findOne(temp , BSON("name" << ns));
        if (config["options"].isABSONObj()) {
            Status status = userCreateNS(txn, db, ns, config["options"].Obj(), logForRepl, 0);
            if ( !status.isOK() ) {
                errmsg = status.toString();
                return false;
            }
        }

        // main data
        copy(txn, dbName,
             ns.c_str(), ns.c_str(), false, logForRepl, false, true, mayYield, mayBeInterrupted,
             Query(query).snapshot());

        /* TODO : copyIndexes bool does not seem to be implemented! */
        if(!copyIndexes) {
            log() << "ERROR copy collection copyIndexes not implemented? " << ns << endl;
        }

        // indexes
        temp = dbName + ".system.indexes";
        copy(txn, dbName, temp.c_str(), temp.c_str(), true, logForRepl, false, true, mayYield,
             mayBeInterrupted, BSON( "ns" << ns ));

        txn->recoveryUnit()->commitIfNeeded();
        return true;
    }

    extern bool inDBRepair;

    bool Cloner::go(OperationContext* txn,
                    const std::string& toDBName,
                    const string& masterHost,
                    const CloneOptions& opts,
                    set<string>* clonedColls,
                    string& errmsg,
                    int* errCode) {

        if ( errCode ) {
            *errCode = 0;
        }
        massert( 10289 ,  "useReplAuth is not written to replication log", !opts.useReplAuth || !opts.logForRepl );

#if !defined(_WIN32) && !defined(__sunos__)
        // isSelf() only does the necessary comparisons on os x and linux (SERVER-14165)
        bool masterSameProcess = HostAndPort(masterHost).isSelf();
#else
        stringstream a,b;
        a << "localhost:" << serverGlobalParams.port;
        b << "127.0.0.1:" << serverGlobalParams.port;
        bool masterSameProcess = (a.str() == masterHost || b.str() == masterHost);
#endif

        if (masterSameProcess) {
            if (opts.fromDB == toDBName) {
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
                if (!repl::replAuthenticate(con.get()))
                    return false;
                
                _conn = con;
            }
            else {
                _conn.reset(new DBDirectClient());
            }
        }

        string systemNamespacesNS = opts.fromDB + ".system.namespaces";

        list<BSONObj> toClone;
        if ( clonedColls ) clonedColls->clear();
        if ( opts.syncData ) {
            /* todo: we can put these releases inside dbclient or a dbclient specialization.
               or just wait until we get rid of global lock anyway.
               */
            Lock::TempRelease tempRelease(txn->lockState());

            // just using exhaust for collection copying right now

            // todo: if snapshot (bool param to this func) is true, we need to snapshot this query?
            //       only would be relevant if a thousands of collections -- maybe even then it is hard
            //       to exceed a single cursor batch.
            //       for repl it is probably ok as we apply oplog section after the clone (i.e. repl
            //       doesnt not use snapshot=true).
            auto_ptr<DBClientCursor> cursor = _conn->query(systemNamespacesNS, BSONObj(), 0, 0, 0,
                                                      opts.slaveOk ? QueryOption_SlaveOk : 0);

            if (!validateQueryResults(cursor, errCode, errmsg)) {
                errmsg = str::stream() << "index query on ns " << systemNamespacesNS
                                       << " failed: " << errmsg;
                return false;
            }

            while ( cursor->more() ) {
                BSONObj collection = cursor->next();

                LOG(2) << "\t cloner got " << collection << endl;

                BSONElement collectionOptions = collection["options"];
                if ( collectionOptions.isABSONObj() ) {
                    Status parseOptionsStatus = CollectionOptions().parse(collectionOptions.Obj());
                    if ( !parseOptionsStatus.isOK() ) {
                        errmsg = str::stream() << "invalid collection options: " << collection
                                               << ", reason: " << parseOptionsStatus.reason();
                        return false;
                    }
                }

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

                if ( clonedColls ) clonedColls->insert( from_name );
                toClone.push_back( collection.getOwned() );
            }
        }

        for ( list<BSONObj>::iterator i=toClone.begin(); i != toClone.end(); i++ ) {
            BSONObj collection = *i;
            LOG(2) << "  really will clone: " << collection << endl;
            const char * from_name = collection["name"].valuestr();
            BSONObj options = collection.getObjectField("options");

            /* change name "<fromdb>.collection" -> <todb>.collection */
            const char *p = strchr(from_name, '.');
            verify(p);
            const string to_name = toDBName + p;

            // Copy releases the lock, so we need to re-load the database. This should probably
            // throw if the database has changed in between, but for now preserve the existing
            // behaviour.
            bool unused;
            Database* db = dbHolder().getOrCreate(txn, toDBName, unused);

            /* we defer building id index for performance - building it in batch is much faster */
            Status createStatus = userCreateNS( txn, db, to_name, options,
                                                opts.logForRepl, false );
            if ( !createStatus.isOK() ) {
                errmsg = str::stream() << "failed to create collection \"" << to_name << "\": "
                                       << createStatus.reason();
                return false;
            }

            LOG(1) << "\t\t cloning " << from_name << " -> " << to_name << endl;
            Query q;
            if( opts.snapshot )
                q.snapshot();

            copy(txn,
                 toDBName,
                 from_name,
                 to_name.c_str(),
                 false,
                 opts.logForRepl,
                 masterSameProcess,
                 opts.slaveOk,
                 opts.mayYield,
                 opts.mayBeInterrupted,
                 q);

            {
                /* we need dropDups to be true as we didn't do a true snapshot and this is before applying oplog operations
                   that occur during the initial sync.  inDBRepair makes dropDups be true.
                   */
                bool old = inDBRepair;
                try {
                    inDBRepair = true;
                    Collection* c = db->getCollection( txn, to_name );
                    if ( c )
                        c->getIndexCatalog()->ensureHaveIdIndex(txn);
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
            string system_indexes_to = toDBName + ".system.indexes";
            
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
            copy(txn, toDBName, system_indexes_from.c_str(), system_indexes_to.c_str(), true,
                 opts.logForRepl, masterSameProcess, opts.slaveOk, opts.mayYield, opts.mayBeInterrupted, query );
        }
        return true;
    }

} // namespace mongo
