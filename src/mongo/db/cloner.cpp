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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/cloner.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/log.h"

namespace mongo {

    MONGO_EXPORT_SERVER_PARAMETER(skipCorruptDocumentsWhenCloning, bool, false);

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
            else {
                b.append(e);
            }
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
            invariant(from_collection.coll() != "system.indexes");

            // XXX: can probably take dblock instead
            Lock::GlobalWrite lk(txn->lockState());

            // Make sure database still exists after we resume from the temp release
            Database* db = dbHolder().openDb(txn, _dbName);

            bool createdCollection = false;
            Collection* collection = NULL;

            collection = db->getCollection( txn, to_collection );
            if ( !collection ) {
                massert( 17321,
                         str::stream()
                         << "collection dropped during clone ["
                         << to_collection.ns() << "]",
                         !createdCollection );
                WriteUnitOfWork wunit(txn);
                createdCollection = true;
                collection = db->createCollection( txn, to_collection.ns() );
                verify( collection );
                if (logForRepl) {
                    repl::logOp(txn,
                                "c",
                                (_dbName + ".$cmd").c_str(),
                                BSON("create" << to_collection.coll()));
                }
                wunit.commit();
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
                    str::stream ss;
                    ss << "Cloner: found corrupt document in " << from_collection.toString()
                          << ": " << status.reason();
                    if (skipCorruptDocumentsWhenCloning) {
                        warning() << ss.ss.str() << "; skipping";
                        continue;
                    }
                    msgasserted(28531, ss);
                }

                ++numSeen;
                WriteUnitOfWork wunit(txn);

                BSONObj js = tmp;

                StatusWith<DiskLoc> loc = collection->insertDocument( txn, js, true );
                if ( !loc.isOK() ) {
                    error() << "error: exception cloning object in " << from_collection
                            << ' ' << loc.toString() << " obj:" << js;
                }
                uassertStatusOK( loc.getStatus() );
                if (logForRepl)
                    repl::logOp(txn, "i", to_collection.ns().c_str(), js);

                wunit.commit();

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
        NamespaceString from_collection;
        NamespaceString to_collection;
        time_t saveLast;
        bool logForRepl;
        bool _mayYield;
        bool _mayBeInterrupted;
    };

    /* copy the specified collection
    */
    void Cloner::copy(OperationContext* txn,
                      const string& toDBName,
                      const NamespaceString& from_collection,
                      const NamespaceString& to_collection,
                      bool logForRepl,
                      bool masterSameProcess,
                      bool slaveOk,
                      bool mayYield,
                      bool mayBeInterrupted,
                      Query query) {
        LOG(2) << "\t\tcloning collection " << from_collection << " to " << to_collection << " on " << _conn->getServerAddress() << " with filter " << query.toString() << endl;

        Fun f(txn, toDBName);
        f.numSeen = 0;
        f.from_collection = from_collection;
        f.to_collection = to_collection;
        f.saveLast = time( 0 );
        f.logForRepl = logForRepl;
        f._mayYield = mayYield;
        f._mayBeInterrupted = mayBeInterrupted;

        int options = QueryOption_NoCursorTimeout | ( slaveOk ? QueryOption_SlaveOk : 0 );
        {
            Lock::TempRelease tempRelease(txn->lockState());
            _conn->query(stdx::function<void(DBClientCursorBatchIterator &)>(f), from_collection,
                         query, 0, options);
        }
    }

    void Cloner::copyIndexes(OperationContext* txn,
                             const string& toDBName,
                             const NamespaceString& from_collection,
                             const NamespaceString& to_collection,
                             bool logForRepl,
                             bool masterSameProcess,
                             bool slaveOk,
                             bool mayYield,
                             bool mayBeInterrupted) {

        LOG(2) << "\t\t copyIndexes " << from_collection << " to " << to_collection
               << " on " << _conn->getServerAddress();

        vector<BSONObj> indexesToBuild;

        {
            Lock::TempRelease tempRelease(txn->lockState());
            list<BSONObj> sourceIndexes = _conn->getIndexSpecs( from_collection,
                                                                slaveOk ? QueryOption_SlaveOk : 0 );
            for (list<BSONObj>::const_iterator it = sourceIndexes.begin();
                    it != sourceIndexes.end(); ++it) {
                indexesToBuild.push_back(fixindex(to_collection.db().toString(), *it));
            }
        }

        if (indexesToBuild.empty())
            return;

        // We are under lock here again, so reload the database in case it may have disappeared
        // during the temp release
        Database* db = dbHolder().openDb(txn, toDBName);

        Collection* collection = db->getCollection( txn, to_collection );
        if ( !collection ) {
            WriteUnitOfWork wunit(txn);
            collection = db->createCollection( txn, to_collection.ns() );
            invariant(collection);
            if (logForRepl) {
                repl::logOp(txn,
                            "c",
                            (toDBName + ".$cmd").c_str(),
                            BSON("create" << to_collection.coll()));
            }
            wunit.commit();
        }

        // TODO pass the MultiIndexBlock when inserting into the collection rather than building the
        // indexes after the fact. This depends on holding a lock on the collection the whole time
        // from creation to completion without yielding to ensure the index and the collection
        // matches. It also wouldn't work on non-empty collections so we would need both
        // implementations anyway as long as that is supported.
        MultiIndexBlock indexer(txn, collection);
        if (mayBeInterrupted)
            indexer.allowInterruption();

        indexer.removeExistingIndexes(&indexesToBuild);
        if (indexesToBuild.empty())
            return;

        uassertStatusOK(indexer.init(indexesToBuild));
        uassertStatusOK(indexer.insertAllDocumentsInCollection());

        WriteUnitOfWork wunit(txn);
        indexer.commit();
        if (logForRepl) {
            for (vector<BSONObj>::const_iterator it = indexesToBuild.begin();
                    it != indexesToBuild.end(); ++it) {
                repl::logOp(txn, "i", to_collection.ns().c_str(), *it);
            }
        }
        wunit.commit();
    }

    bool Cloner::copyCollection(OperationContext* txn,
                                const string& ns,
                                const BSONObj& query,
                                string& errmsg,
                                bool mayYield,
                                bool mayBeInterrupted,
                                bool shouldCopyIndexes,
                                bool logForRepl) {

        const NamespaceString nss(ns);
        const string dbname = nss.db().toString();

        Lock::DBLock dbWrite(txn->lockState(), dbname, MODE_X);

        Database* db = dbHolder().openDb(txn, dbname);

        // config
        BSONObj filter = BSON("name" << nss.coll().toString());
        list<BSONObj> collList = _conn->getCollectionInfos( dbname, filter);
        if (!collList.empty()) {
            invariant(collList.size() <= 1);
            BSONObj col = collList.front();
            if (col["options"].isABSONObj()) {
                WriteUnitOfWork wunit(txn);
                Status status = userCreateNS(txn, db, ns, col["options"].Obj(), logForRepl, 0);
                if ( !status.isOK() ) {
                    errmsg = status.toString();
                    return false;
                }
                wunit.commit();
            }
        }

        // main data
        copy(txn, dbname,
             nss, nss,
             logForRepl, false, true, mayYield, mayBeInterrupted,
             Query(query).snapshot());

        /* TODO : copyIndexes bool does not seem to be implemented! */
        if(!shouldCopyIndexes) {
            log() << "ERROR copy collection shouldCopyIndexes not implemented? " << ns << endl;
        }

        // indexes
        copyIndexes(txn, dbname,
                    NamespaceString(ns), NamespaceString(ns),
                    logForRepl, false, true, mayYield,
                    mayBeInterrupted);

        return true;
    }

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

        const ConnectionString cs = ConnectionString::parse(masterHost, errmsg);
        if (!cs.isValid()) {
            if (errCode)
                *errCode = ErrorCodes::FailedToParse;
            return false;
        }

        bool masterSameProcess = false;
        std::vector<HostAndPort> csServers = cs.getServers();
        for (std::vector<HostAndPort>::const_iterator iter = csServers.begin();
             iter != csServers.end(); ++iter) {

            if (!repl::isSelf(*iter))
                continue;

            masterSameProcess = true;
            break;
        }

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
                auto_ptr<DBClientBase> con( cs.connect( errmsg ));
                if ( !con.get() )
                    return false;
                if (getGlobalAuthorizationManager()->isAuthEnabled() &&
                    !authenticateInternalUser(con.get())) {

                    return false;
                }

                _conn = con;
            }
            else {
                _conn.reset(new DBDirectClient(txn));
            }
        }

        list<BSONObj> toClone;
        if ( clonedColls ) clonedColls->clear();
        {
            /* todo: we can put these releases inside dbclient or a dbclient specialization.
               or just wait until we get rid of global lock anyway.
               */
            Lock::TempRelease tempRelease(txn->lockState());

            list<BSONObj> raw = _conn->getCollectionInfos( opts.fromDB );
            for ( list<BSONObj>::iterator it = raw.begin(); it != raw.end(); ++it ) {
                BSONObj collection = *it;

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
                    string s = "bad collection object " + collection.toString();
                    massert( 10290 , s.c_str(), false);
                }
                verify( !e.eoo() );
                verify( e.type() == String );

                NamespaceString ns( opts.fromDB, e.valuestr() );

                if( ns.isSystem() ) {
                    /* system.users and s.js is cloned -- but nothing else from system.
                     * system.indexes is handled specially at the end*/
                    if( legalClientSystemNS( ns.ns() , true ) == 0 ) {
                        LOG(2) << "\t\t not cloning because system collection" << endl;
                        continue;
                    }
                }
                if( !ns.isNormal() ) {
                    LOG(2) << "\t\t not cloning because has $ ";
                    continue;
                }

                if( opts.collsToIgnore.find( ns.ns() ) != opts.collsToIgnore.end() ){
                    LOG(2) << "\t\t ignoring collection " << ns;
                    continue;
                }
                else {
                    LOG(2) << "\t\t not ignoring collection " << ns;
                }

                if ( clonedColls ) clonedColls->insert( ns.ns() );
                toClone.push_back( collection.getOwned() );
            }
        }

        if ( opts.syncData ) {
            for ( list<BSONObj>::iterator i=toClone.begin(); i != toClone.end(); i++ ) {
                BSONObj collection = *i;
                LOG(2) << "  really will clone: " << collection << endl;
                const char* collectionName = collection["name"].valuestr();
                BSONObj options = collection.getObjectField("options");

                NamespaceString from_name( opts.fromDB, collectionName );
                NamespaceString to_name( toDBName, collectionName );

                Database* db;
                {
                    WriteUnitOfWork wunit(txn);
                    // Copy releases the lock, so we need to re-load the database. This should
                    // probably throw if the database has changed in between, but for now preserve
                    // the existing behaviour.
                    db = dbHolder().openDb(txn, toDBName);

                    // we defer building id index for performance - building it in batch is much
                    // faster
                    Status createStatus = userCreateNS( txn, db, to_name.ns(), options,
                                                        opts.logForRepl, false );
                    if ( !createStatus.isOK() ) {
                        errmsg = str::stream() << "failed to create collection \""
                                               << to_name.ns() << "\": "
                                               << createStatus.reason();
                        return false;
                    }
                    wunit.commit();
                }

                LOG(1) << "\t\t cloning " << from_name << " -> " << to_name << endl;
                Query q;
                if( opts.snapshot )
                    q.snapshot();

                copy(txn,
                     toDBName,
                     from_name,
                     to_name,
                     opts.logForRepl,
                     masterSameProcess,
                     opts.slaveOk,
                     opts.mayYield,
                     opts.mayBeInterrupted,
                     q);

                db = dbHolder().get(txn, toDBName);
                uassert(18645,
                        str::stream() << "database " << toDBName << " dropped during clone",
                        db);

                Collection* c = db->getCollection( txn, to_name );
                if ( c && !c->getIndexCatalog()->haveIdIndex( txn ) ) {
                    // We need to drop objects with duplicate _ids because we didn't do a true
                    // snapshot and this is before applying oplog operations that occur during the
                    // initial sync.
                    set<DiskLoc> dups;

                    MultiIndexBlock indexer(txn, c);
                    if (opts.mayBeInterrupted)
                        indexer.allowInterruption();

                    uassertStatusOK(indexer.init(c->getIndexCatalog()->getDefaultIdIndexSpec()));
                    uassertStatusOK(indexer.insertAllDocumentsInCollection(&dups));

                    for (set<DiskLoc>::const_iterator it = dups.begin(); it != dups.end(); ++it) {
                        WriteUnitOfWork wunit(txn);
                        BSONObj id;

                        c->deleteDocument(txn, *it, true, true, opts.logForRepl ? &id : NULL);
                        if (opts.logForRepl)
                            repl::logOp(txn, "d", c->ns().ns().c_str(), id);
                        wunit.commit();
                    }

                    if (!dups.empty()) {
                        log() << "index build dropped: " << dups.size() << " dups";
                    }

                    WriteUnitOfWork wunit(txn);
                    indexer.commit();
                    if (opts.logForRepl) {
                        repl::logOp(txn,
                                    "i",
                                    c->ns().getSystemIndexesCollection().c_str(),
                                    c->getIndexCatalog()->getDefaultIdIndexSpec());
                    }
                    wunit.commit();
                }
            }
        }

        // now build the secondary indexes
        if ( opts.syncIndexes ) {
            for ( list<BSONObj>::iterator i=toClone.begin(); i != toClone.end(); i++ ) {
                BSONObj collection = *i;
                log() << "copying indexes for: " << collection;

                const char* collectionName = collection["name"].valuestr();

                NamespaceString from_name( opts.fromDB, collectionName );
                NamespaceString to_name( toDBName, collectionName );

                copyIndexes(txn,
                            toDBName,
                            from_name,
                            to_name,
                            opts.logForRepl,
                            masterSameProcess,
                            opts.slaveOk,
                            opts.mayYield,
                            opts.mayBeInterrupted );
            }
        }

        return true;
    }

} // namespace mongo
