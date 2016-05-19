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

#include <boost/scoped_ptr.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using boost::scoped_ptr;
using std::auto_ptr;
using std::list;
using std::set;
using std::endl;
using std::string;
using std::vector;

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
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        if (e.eoo())
            break;

        // for now, skip the "v" field so that v:0 indexes will be upgraded to v:1
        if (string("v") == e.fieldName()) {
            continue;
        }

        if (string("ns") == e.fieldName()) {
            uassert(10024, "bad ns field for index during dbcopy", e.type() == String);
            const char* p = strchr(e.valuestr(), '.');
            uassert(10025, "bad ns field for index during dbcopy [2]", p);
            string newname = newDbName + p;
            b.append("ns", newname);
        } else {
            b.append(e);
        }
    }

    BSONObj res = b.obj();

    return res;
}

Cloner::Cloner() {}

struct Cloner::Fun {
    Fun(OperationContext* txn, const string& dbName) : lastLog(0), txn(txn), _dbName(dbName) {}

    void operator()(DBClientCursorBatchIterator& i) {
        invariant(from_collection.coll() != "system.indexes");

        // XXX: can probably take dblock instead
        scoped_ptr<ScopedTransaction> scopedXact(new ScopedTransaction(txn, MODE_X));
        scoped_ptr<Lock::GlobalWrite> globalWriteLock(new Lock::GlobalWrite(txn->lockState()));
        uassert(ErrorCodes::NotMaster,
                str::stream() << "Not primary while cloning collection " << from_collection.ns()
                              << " to " << to_collection.ns(),
                !logForRepl ||
                    repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(_dbName));

        // Make sure database still exists after we resume from the temp release
        Database* db = dbHolder().openDb(txn, _dbName);

        bool createdCollection = false;
        Collection* collection = NULL;

        collection = db->getCollection(to_collection);
        if (!collection) {
            massert(17321,
                    str::stream() << "collection dropped during clone [" << to_collection.ns()
                                  << "]",
                    !createdCollection);
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                WriteUnitOfWork wunit(txn);
                Status s = userCreateNS(
                    txn, db, to_collection.toString(), from_options, logForRepl, false);
                verify(s.isOK());

                if (logForRepl) {
                    repl::logOp(txn,
                                "c",
                                (_dbName + ".$cmd").c_str(),
                                BSON("create" << to_collection.coll()));
                }
                wunit.commit();
                collection = db->getCollection(to_collection);
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", to_collection.ns());
        }

        while (i.moreInCurrentBatch()) {
            if (numSeen % 128 == 127) {
                time_t now = time(0);
                if (now - lastLog >= 60) {
                    // report progress
                    if (lastLog)
                        log() << "clone " << to_collection << ' ' << numSeen << endl;
                    lastLog = now;
                }

                if (_mayBeInterrupted) {
                    txn->checkForInterrupt();
                }

                if (_mayYield) {
                    scopedXact.reset();
                    globalWriteLock.reset();

                    txn->getCurOp()->yielded();

                    scopedXact.reset(new ScopedTransaction(txn, MODE_X));
                    globalWriteLock.reset(new Lock::GlobalWrite(txn->lockState()));

                    // Check if everything is still all right.
                    if (logForRepl) {
                        uassert(28592,
                                str::stream() << "Cannot write to db: " << _dbName
                                              << " after yielding",
                                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                                    _dbName));
                    }

                    // TODO: SERVER-16598 abort if original db or collection is gone.
                    db = dbHolder().get(txn, _dbName);
                    uassert(28593,
                            str::stream() << "Database " << _dbName << " dropped while cloning",
                            db != NULL);

                    collection = db->getCollection(to_collection);
                    uassert(28594,
                            str::stream() << "Collection " << to_collection.ns()
                                          << " dropped while cloning",
                            collection != NULL);
                }
            }

            BSONObj tmp = i.nextSafe();

            /* assure object is valid.  note this will slow us down a little. */
            const Status status = validateBSON(tmp.objdata(), tmp.objsize());
            if (!status.isOK()) {
                str::stream ss;
                ss << "Cloner: found corrupt document in " << from_collection.toString() << ": "
                   << status.reason();
                if (skipCorruptDocumentsWhenCloning) {
                    warning() << ss.ss.str() << "; skipping";
                    continue;
                }
                msgasserted(28531, ss);
            }

            verify(collection);
            ++numSeen;
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                WriteUnitOfWork wunit(txn);

                BSONObj js = tmp;

                StatusWith<RecordId> loc = collection->insertDocument(txn, js, true);
                if (!loc.isOK()) {
                    error() << "error: exception cloning object in " << from_collection << ' '
                            << loc.toString() << " obj:" << js;
                }
                uassertStatusOK(loc.getStatus());
                if (logForRepl)
                    repl::logOp(txn, "i", to_collection.ns().c_str(), js);

                wunit.commit();
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "cloner insert", to_collection.ns());
            RARELY if (time(0) - saveLast > 60) {
                log() << numSeen << " objects cloned so far from collection " << from_collection;
                saveLast = time(0);
            }
        }
    }

    time_t lastLog;
    OperationContext* txn;
    const string _dbName;

    int64_t numSeen;
    NamespaceString from_collection;
    BSONObj from_options;
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
                  const BSONObj& from_opts,
                  const NamespaceString& to_collection,
                  bool logForRepl,
                  bool masterSameProcess,
                  bool slaveOk,
                  bool mayYield,
                  bool mayBeInterrupted,
                  Query query) {
    LOG(2) << "\t\tcloning collection " << from_collection << " to " << to_collection << " on "
           << _conn->getServerAddress() << " with filter " << query.toString() << endl;

    Fun f(txn, toDBName);
    f.numSeen = 0;
    f.from_collection = from_collection;
    f.from_options = from_opts;
    f.to_collection = to_collection;
    f.saveLast = time(0);
    f.logForRepl = logForRepl;
    f._mayYield = mayYield;
    f._mayBeInterrupted = mayBeInterrupted;

    int options = QueryOption_NoCursorTimeout | (slaveOk ? QueryOption_SlaveOk : 0);
    {
        Lock::TempRelease tempRelease(txn->lockState());
        _conn->query(stdx::function<void(DBClientCursorBatchIterator&)>(f),
                     from_collection,
                     query,
                     0,
                     options);
    }

    uassert(ErrorCodes::NotMaster,
            str::stream() << "Not primary while cloning collection " << from_collection.ns()
                          << " to " << to_collection.ns() << " with filter " << query.toString(),
            !logForRepl ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(toDBName));
}

void Cloner::copyIndexes(OperationContext* txn,
                         const string& toDBName,
                         const NamespaceString& from_collection,
                         const BSONObj& from_opts,
                         const NamespaceString& to_collection,
                         bool logForRepl,
                         bool masterSameProcess,
                         bool slaveOk,
                         bool mayYield,
                         bool mayBeInterrupted) {
    LOG(2) << "\t\t copyIndexes " << from_collection << " to " << to_collection << " on "
           << _conn->getServerAddress();

    vector<BSONObj> indexesToBuild;

    {
        Lock::TempRelease tempRelease(txn->lockState());
        list<BSONObj> sourceIndexes =
            _conn->getIndexSpecs(from_collection, slaveOk ? QueryOption_SlaveOk : 0);
        for (list<BSONObj>::const_iterator it = sourceIndexes.begin(); it != sourceIndexes.end();
             ++it) {
            indexesToBuild.push_back(fixindex(to_collection.db().toString(), *it));
        }
    }

    uassert(ErrorCodes::NotMaster,
            str::stream() << "Not primary while copying indexes from " << from_collection.ns()
                          << " to " << to_collection.ns() << " (Cloner)",
            !logForRepl ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(toDBName));


    if (indexesToBuild.empty())
        return;

    // We are under lock here again, so reload the database in case it may have disappeared
    // during the temp release
    Database* db = dbHolder().openDb(txn, toDBName);

    Collection* collection = db->getCollection(to_collection);
    if (!collection) {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wunit(txn);
            Status s =
                userCreateNS(txn, db, to_collection.toString(), from_opts, logForRepl, false);
            invariant(s.isOK());
            collection = db->getCollection(to_collection);
            invariant(collection);
            if (logForRepl) {
                repl::logOp(
                    txn, "c", (toDBName + ".$cmd").c_str(), BSON("create" << to_collection.coll()));
            }
            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", to_collection.ns());
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
        const string targetSystemIndexesCollectionName = to_collection.getSystemIndexesCollection();
        const char* createIndexNs = targetSystemIndexesCollectionName.c_str();
        for (vector<BSONObj>::const_iterator it = indexesToBuild.begin();
             it != indexesToBuild.end();
             ++it) {
            repl::logOp(txn, "i", createIndexNs, *it);
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

    ScopedTransaction transaction(txn, MODE_IX);
    Lock::DBLock dbWrite(txn->lockState(), dbname, MODE_X);

    uassert(ErrorCodes::NotMaster,
            str::stream() << "Not primary while copying collection " << ns << " (Cloner)",
            !logForRepl ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbname));

    Database* db = dbHolder().openDb(txn, dbname);

    // config
    BSONObj filter = BSON("name" << nss.coll().toString());
    list<BSONObj> collList = _conn->getCollectionInfos(dbname, filter);
    BSONObj options;
    if (!collList.empty()) {
        invariant(collList.size() <= 1);
        BSONObj col = collList.front();
        if (col["options"].isABSONObj()) {
            options = col["options"].Obj();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wunit(txn);
            Status status = userCreateNS(txn, db, ns, options, logForRepl, false);
            if (!status.isOK()) {
                errmsg = status.toString();
                // aborts write unit of work
                return false;
            }
            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", ns);
    } else {
        LOG(1) << "No collection info found for ns:" << nss.toString()
               << ", host:" << _conn->getServerAddress();
    }

    // main data
    copy(txn,
         dbname,
         nss,
         options,
         nss,
         logForRepl,
         false,
         true,
         mayYield,
         mayBeInterrupted,
         Query(query).snapshot());

    /* TODO : copyIndexes bool does not seem to be implemented! */
    if (!shouldCopyIndexes) {
        log() << "ERROR copy collection shouldCopyIndexes not implemented? " << ns << endl;
    }

    // indexes
    copyIndexes(txn,
                dbname,
                NamespaceString(ns),
                options,
                NamespaceString(ns),
                logForRepl,
                false,
                true,
                mayYield,
                mayBeInterrupted);

    return true;
}

StatusWith<std::vector<BSONObj>> Cloner::filterCollectionsForClone(
    const CloneOptions& opts, const std::list<BSONObj>& initialCollections) {
    std::vector<BSONObj> finalCollections;
    for (auto&& collection : initialCollections) {
        LOG(2) << "\t cloner got " << collection;

        BSONElement collectionOptions = collection["options"];
        if (collectionOptions.isABSONObj()) {
            auto parseOptionsStatus = CollectionOptions().parse(collectionOptions.Obj());
            if (!parseOptionsStatus.isOK()) {
                return StatusWith<std::vector<mongo::BSONObj>>(parseOptionsStatus);
            }
        }

        std::string collectionName;
        auto status = bsonExtractStringField(collection, "name", &collectionName);
        if (!status.isOK()) {
            return StatusWith<std::vector<mongo::BSONObj>>(status);
        }

        const NamespaceString ns(opts.fromDB, collectionName.c_str());

        if (ns.isSystem()) {
            if (legalClientSystemNS(ns.ns(), true) == 0) {
                LOG(2) << "\t\t not cloning because system collection" << endl;
                continue;
            }
        }
        if (!ns.isNormal()) {
            LOG(2) << "\t\t not cloning because has $ ";
            continue;
        }
        if (opts.collsToIgnore.find(ns.ns()) != opts.collsToIgnore.end()) {
            LOG(2) << "\t\t ignoring collection " << ns;
            continue;
        } else {
            LOG(2) << "\t\t not ignoring collection " << ns;
        }

        finalCollections.push_back(collection.getOwned());
    }
    return StatusWith<std::vector<mongo::BSONObj>>(finalCollections);
}

Status Cloner::createCollectionsForDb(OperationContext* txn,
                                      const std::vector<BSONObj>& collections,
                                      const std::string& dbName) {
    Database* db = dbHolder().openDb(txn, dbName);
    for (auto&& collection : collections) {
        BSONObj options = collection.getObjectField("options");
        const NamespaceString nss(dbName, collection["name"].valuestr());

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            txn->checkForInterrupt();
            WriteUnitOfWork wunit(txn);

            // we defer building id index for performance - building it in batch is much faster
            Status createStatus = userCreateNS(txn, db, nss.ns(), options, false, false);
            if (!createStatus.isOK()) {
                return createStatus;
            }

            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", nss.ns());
    }
    return Status::OK();
}

bool Cloner::go(OperationContext* txn,
                const std::string& toDBName,
                const string& masterHost,
                const CloneOptions& opts,
                set<string>* clonedColls,
                std::vector<BSONObj> collectionsToClone,
                string& errmsg,
                int* errCode) {
    if (errCode) {
        *errCode = 0;
    }
    massert(10289,
            "useReplAuth is not written to replication log",
            !opts.useReplAuth || !opts.logForRepl);

    const ConnectionString cs = ConnectionString::parse(masterHost, errmsg);
    if (!cs.isValid()) {
        if (errCode)
            *errCode = ErrorCodes::FailedToParse;
        return false;
    }

    bool masterSameProcess = false;
    std::vector<HostAndPort> csServers = cs.getServers();
    for (std::vector<HostAndPort>::const_iterator iter = csServers.begin(); iter != csServers.end();
         ++iter) {
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
        } else if (!masterSameProcess) {
            auto_ptr<DBClientBase> con(cs.connect(errmsg));
            if (!con.get())
                return false;
            if (getGlobalAuthorizationManager()->isAuthEnabled() &&
                !authenticateInternalUser(con.get())) {
                return false;
            }

            _conn = con;
        } else {
            _conn.reset(new DBDirectClient(txn));
        }
    }

    // Gather the list of collections to clone
    std::vector<BSONObj> toClone;
    if (clonedColls) {
        clonedColls->clear();
    }

    if (opts.createCollections) {
        // getCollectionInfos may make a remote call, which may block indefinitely, so release
        // the global lock that we are entering with.
        Lock::TempRelease tempRelease(txn->lockState());

        std::list<BSONObj> initialCollections = _conn->getCollectionInfos(opts.fromDB);
        auto status = filterCollectionsForClone(opts, initialCollections);
        if (!status.isOK()) {
            error() << "filter collections for clone failed: " << status;
            return false;
        }
        toClone = status.getValue();
    } else {
        toClone = collectionsToClone;
    }

    uassert(ErrorCodes::NotMaster,
            str::stream() << "Not primary while cloning database " << opts.fromDB
                          << " (after getting list of collections to clone)",
            !opts.logForRepl ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(toDBName));

    if (opts.syncData) {
        if (opts.createCollections) {
            Status status = createCollectionsForDb(txn, toClone, toDBName);
            if (!status.isOK()) {
                error() << "create collections for db failed: " << status;
                return false;
            }
        }
        for (auto&& collection : toClone) {
            LOG(2) << "  really will clone: " << collection << endl;
            const char* collectionName = collection["name"].valuestr();
            BSONObj options = collection.getObjectField("options");

            const NamespaceString from_name(opts.fromDB, collectionName);
            const NamespaceString to_name(toDBName, collectionName);

            if (clonedColls) {
                clonedColls->insert(from_name.ns());
            }

            LOG(1) << "\t\t cloning " << from_name << " -> " << to_name << endl;
            Query q;
            if (opts.snapshot)
                q.snapshot();

            copy(txn,
                 toDBName,
                 from_name,
                 options,
                 to_name,
                 opts.logForRepl,
                 masterSameProcess,
                 opts.slaveOk,
                 opts.mayYield,
                 opts.mayBeInterrupted,
                 q);

            // Copy releases the lock, so we need to re-load the database. This should
            // probably throw if the database has changed in between, but for now preserve
            // the existing behaviour.
            Database* db = dbHolder().get(txn, toDBName);
            uassert(18645, str::stream() << "database " << toDBName << " dropped during clone", db);

            Collection* c = db->getCollection(to_name);
            if (c && !c->getIndexCatalog()->haveIdIndex(txn)) {
                // We need to drop objects with duplicate _ids because we didn't do a true
                // snapshot and this is before applying oplog operations that occur during the
                // initial sync.
                set<RecordId> dups;

                MultiIndexBlock indexer(txn, c);
                if (opts.mayBeInterrupted)
                    indexer.allowInterruption();

                uassertStatusOK(indexer.init(c->getIndexCatalog()->getDefaultIdIndexSpec()));
                uassertStatusOK(indexer.insertAllDocumentsInCollection(&dups));

                // This must be done before we commit the indexer. See the comment about
                // dupsAllowed in IndexCatalog::_unindexRecord and SERVER-17487.
                for (set<RecordId>::const_iterator it = dups.begin(); it != dups.end(); ++it) {
                    WriteUnitOfWork wunit(txn);
                    BSONObj id;

                    c->deleteDocument(txn, *it, true, true, opts.logForRepl ? &id : NULL);
                    if (opts.logForRepl)
                        repl::logDeleteOp(txn,
                                          c->ns().ns().c_str(),
                                          id,
                                          false,   // fromMigrate
                                          false);  // isInMigratingChunk; must be false as
                                                   // this code path is only used during initial
                                                   // sync, and a node cannot be donating a chunk
                                                   // and doing an initial sync simultaneously.
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
    if (opts.syncIndexes) {
        for (auto&& collection : toClone) {
            log() << "copying indexes for: " << collection;

            const char* collectionName = collection["name"].valuestr();

            NamespaceString from_name(opts.fromDB, collectionName);
            NamespaceString to_name(toDBName, collectionName);

            copyIndexes(txn,
                        toDBName,
                        from_name,
                        collection.getObjectField("options"),
                        to_name,
                        opts.logForRepl,
                        masterSameProcess,
                        opts.slaveOk,
                        opts.mayYield,
                        opts.mayBeInterrupted);
        }
    }

    return true;
}

}  // namespace mongo
