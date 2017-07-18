/**
*    @file rs_rollback.cpp
*
*    Copyright (C) 2008-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rs_rollback.h"

#include <algorithm>
#include <memory>

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/session_catalog.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::shared_ptr;
using std::unique_ptr;
using std::endl;
using std::list;
using std::map;
using std::multimap;
using std::set;
using std::string;
using std::pair;

namespace repl {

using namespace rollback_internal;

bool DocID::operator<(const DocID& other) const {
    int comp = strcmp(ns, other.ns);
    if (comp < 0)
        return true;
    if (comp > 0)
        return false;

    const StringData::ComparatorInterface* stringComparator = nullptr;
    BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore, stringComparator);
    return eltCmp.evaluate(_id < other._id);
}

bool DocID::operator==(const DocID& other) const {
    // Since this is only used for tests, going with the simple impl that reuses operator< which is
    // used in the real code.
    return !(*this < other || other < *this);
}

void FixUpInfo::removeAllDocsToRefetchFor(const std::string& collection) {
    docsToRefetch.erase(docsToRefetch.lower_bound(DocID::minFor(collection.c_str())),
                        docsToRefetch.upper_bound(DocID::maxFor(collection.c_str())));
}

// TODO: This function will not be fully functional until the entire
// rollback via refetch for non-WT project is complete. See SERVER-30171.
void FixUpInfo::removeRedundantOperations() {
    // These loops and their bodies can be done in any order. The final result of the FixUpInfo
    // members will be the same.

    //    for (const auto& collection : collectionsToDrop) {
    //        removeAllDocsToRefetchFor(collection);
    //        indexesToDrop.erase(collection);
    //        collectionsToResyncMetadata.erase(collection);
    //    }

    for (const auto& collection : collectionsToResyncData) {
        removeAllDocsToRefetchFor(collection);
        // indexesToDrop.erase(collection);
        // collectionsToResyncMetadata.erase(collection);
        // collectionsToDrop.erase(collection);
    }
}

bool FixUpInfo::removeRedundantIndexCommands(UUID uuid, std::string indexName) {
    auto indexes = indexesToCreate.find(uuid);
    if (indexes != indexesToCreate.end()) {
        invariant(indexesToCreate.count(uuid) == 1);
        invariant((*indexes).second.count(indexName) == 1);
        (*indexes).second.erase(indexName);
        if ((*indexes).second.empty()) {
            indexesToCreate.erase(uuid);
        }
        log() << "Rollback: Index " << indexName
              << " was previously dropped. The createIndexes command is canceled out.";
        return true;
    }
    return false;
}

Status rollback_internal::updateFixUpInfoFromLocalOplogEntry(FixUpInfo& fixUpInfo,
                                                             const BSONObj& ourObj) {

    // Checks that the oplog entry is smaller than 512 MB. We do not roll back if the
    // oplog entry is larger than 512 MB.
    if (ourObj.objsize() > 512 * 1024 * 1024)
        throw RSFatalException(str::stream() << "Rollback too large, oplog size: "
                                             << ourObj.objsize());

    auto oplogEntry = OplogEntry(ourObj);
    NamespaceString nss = oplogEntry.getNamespace();
    auto uuid = oplogEntry.getUuid();

    if (oplogEntry.getOpType() == OpTypeEnum::kNoop)
        return Status::OK();

    DocID doc;
    doc.ownedObj = oplogEntry.raw;
    doc.ns = oplogEntry.raw.getStringField("ns");

    if (oplogEntry.getNamespace().isEmpty()) {
        throw RSFatalException(str::stream() << "Local op on rollback has no ns: "
                                             << redact(oplogEntry.toBSON()));
    }

    BSONObj obj =
        oplogEntry.raw.getObjectField(oplogEntry.getOpType() == OpTypeEnum::kUpdate ? "o2" : "o");

    if (obj.isEmpty()) {
        throw RSFatalException(str::stream() << "Local op on rollback has no object field: "
                                             << redact(oplogEntry.toBSON()));
    }

    // If the operation being rolled back has a txnNumber, then the corresponding entry in the
    // session transaction table needs to be refetched.
    auto operationSessionInfo = oplogEntry.getOperationSessionInfo();
    auto txnNumber = operationSessionInfo.getTxnNumber();
    if (txnNumber) {
        auto sessionId = operationSessionInfo.getSessionId();
        invariant(sessionId);
        invariant(oplogEntry.getStatementId());

        DocID txnDoc;
        BSONObjBuilder txnBob;
        txnBob.append("_id", sessionId->toBSON());
        txnDoc.ownedObj = txnBob.obj();
        txnDoc._id = txnDoc.ownedObj.firstElement();
        // TODO: SERVER-29667
        // Once collection uuids replace namespace strings for rollback, this will need to be
        // changed to the uuid of the session transaction table collection.
        txnDoc.ns = NamespaceString::kSessionTransactionsTableNamespace.ns().c_str();

        fixUpInfo.docsToRefetch.insert(txnDoc);
        fixUpInfo.refetchTransactionDocs = true;
    }

    if (oplogEntry.getOpType() == OpTypeEnum::kCommand) {

        // The first element of the object is the name of the command
        // and the collection it is acting on, e.x. {renameCollection: "test.x"}.
        BSONElement first = obj.firstElement();

        switch (oplogEntry.getCommandType()) {
            case OplogEntry::CommandType::kCreate: {
                // Example create collection oplog entry
                // {
                //     ts: ...,
                //     h: ...,
                //     op: "c",
                //     ns: "foo.$cmd",
                //     ui: BinData(...),
                //     o: {
                //            create: "abc", ...
                //        }
                //     ...
                // }

                fixUpInfo.collectionsToDrop.insert(*uuid);
                return Status::OK();
            }
            case OplogEntry::CommandType::kDrop: {
                // Example drop collection oplog entry
                // {
                //     ts: ...,
                //     h: ...,
                //     op: "c",
                //     ns: "foo.$cmd",
                //     ui: BinData(...),
                //     o: {
                //            drop: "abc"
                //        }
                //     ...
                // }
                // TODO: Delete this when UUIDs are enabled. (SERVER-29815)
                NamespaceString collectionNamespace(nss.getSisterNS(first.valuestr()));
                if (!uuid) {
                    fixUpInfo.collectionsToResyncData.insert(collectionNamespace.ns());
                    return Status::OK();
                }
                fixUpInfo.collectionsToRollBackPendingDrop.emplace(
                    *uuid, std::make_pair(oplogEntry.getOpTime(), collectionNamespace));
                return Status::OK();
            }
            case OplogEntry::CommandType::kDropIndexes: {
                // Example drop indexes objects
                //     o: {
                //            dropIndexes: "x",
                //            index: "x_1"
                //        }
                //     o2:{
                //            v: 2,
                //            key: { x: 1 },
                //            name: "x_1",
                //            ns: "foo.x"
                //        }

                string ns = nss.db().toString() + '.' + first.valuestr();

                string indexName;
                auto status = bsonExtractStringField(obj, "index", &indexName);
                if (!status.isOK()) {
                    severe()
                        << "Missing index name in dropIndexes operation on rollback, document: "
                        << redact(doc.ownedObj);
                    throw RSFatalException(
                        "Missing index name in dropIndexes operation on rollback.");
                }

                BSONObj obj2 = *(oplogEntry.getObject2());

                // Inserts the index name and the index spec of the index to be created into the map
                // of index name and index specs that need to be created for the given collection.
                fixUpInfo.indexesToCreate[*uuid].insert(
                    std::pair<std::string, BSONObj>(indexName, obj2));

                return Status::OK();
            }
            case OplogEntry::CommandType::kCreateIndexes: {
                // Example create indexes obj
                // o:{
                //       createIndex: x,
                //       v: 2,
                //       key: { x: 1 },
                //       name: "x_1",
                //   }

                string indexName;
                auto status = bsonExtractStringField(obj, "name", &indexName);
                if (!status.isOK()) {
                    severe()
                        << "Missing index name in createIndexes operation on rollback, document: "
                        << redact(doc.ownedObj);
                    throw RSFatalException(
                        "Missing index name in createIndexes operation on rollback.");
                }

                // Checks if a drop was previously done on this index. If so, we remove it from the
                // indexesToCreate because a dropIndex and createIndex operation on the same
                // collection for the same index cancel each other out. We do not record the
                // createIndexes command in the fixUpInfo struct since applying both of these
                // commands will lead to the same final state as not applying either of the
                // commands. We only cancel out in the direction of [create] -> [drop] indexes
                // because it is possible that in the [drop] -> [create] direction, when we create
                // an index with the same name it may have a different index spec from that index
                // that was previously dropped.
                if (fixUpInfo.removeRedundantIndexCommands(*uuid, indexName)) {
                    return Status::OK();
                }

                // Inserts the index name to be dropped into the set of indexes that
                // need to be dropped for the collection.
                fixUpInfo.indexesToDrop[*uuid].insert(indexName);

                return Status::OK();
            }
            case OplogEntry::CommandType::kRenameCollection: {
                // Example rename collection obj
                // o:{
                //        renameCollection: "foo.x",
                //        to: "foo.y",
                //        stayTemp: false,
                //        dropTarget: BinData(...),
                //        dropSource: BinData(...),
                //   }

                // dropTarget will be false if no collection is dropped during the rename.
                // dropSource is only present during a cross-database rename, and contains
                // the UUID of the collection that is being renamed over. The ui field will
                // contain the UUID of the new collection that is created.

                // TODO: Slow.
                warning() << "Rollback of renameCollection is slow in this version of "
                          << "mongod.";
                string from = first.valuestr();
                string to = obj["to"].String();
                fixUpInfo.collectionsToResyncData.insert(from);
                fixUpInfo.collectionsToResyncData.insert(to);
                return Status::OK();
            }
            case OplogEntry::CommandType::kDropDatabase: {
                // Example drop database oplog entry
                // {
                //     ts: ...,
                //     h: ...,
                //     op: "c",
                //     ns: "foo.$cmd",
                //     o:{
                //            "dropDatabase": 1
                //        }
                //     ...
                // }

                // Since we wait for all internal collection drops to be committed before recording
                // a 'dropDatabase' oplog entry, this will always create an empty database.
                // Creating an empty database doesn't mean anything, so we do nothing.
                return Status::OK();
            }
            case OplogEntry::CommandType::kCollMod: {
                for (auto field : obj) {
                    // Example collMod obj
                    // o:{
                    //       collMod : "x",
                    //       validationLevel : "off",
                    //       index: {
                    //                  name: "indexName_1",
                    //                  expireAfterSeconds: 600
                    //              }
                    //    }

                    const auto modification = field.fieldNameStringData();
                    if (modification == "collMod") {
                        continue;  // Skips the command name. The first field in the obj will be the
                                   // command name.
                    }

                    if (modification == "validator" || modification == "validationAction" ||
                        modification == "validationLevel" || modification == "usePowerOf2Sizes" ||
                        modification == "noPadding") {
                        fixUpInfo.collectionsToResyncMetadata.insert(*uuid);
                        continue;
                    }
                    // Some collMod fields cannot be rolled back, such as the index field.
                    string message = "Cannot roll back a collMod command: ";
                    severe() << message << redact(obj);
                    throw RSFatalException(message);
                }
                return Status::OK();
            }
            case OplogEntry::CommandType::kApplyOps: {
                // Example Apply Ops oplog entry
                //{
                //    op : "c",
                //    ns : admin.$cmd,
                //    o : {
                //             applyOps : [ {
                //                            op : "u", // must be idempotent!
                //                            ns : "test.x",
                //                            ui : BinData(...),
                //                            o2 : {
                //                                _id : 1
                //                            },
                //                            o : {
                //                                _id : 2
                //                            }
                //                        }]
                //         }
                // }

                if (first.type() != Array) {
                    std::string message = str::stream()
                        << "Expected applyOps argument to be an array; found " << redact(first);
                    severe() << message;
                    return Status(ErrorCodes::UnrecoverableRollbackError, message);
                }
                for (const auto& subopElement : first.Array()) {
                    if (subopElement.type() != Object) {
                        std::string message = str::stream()
                            << "Expected applyOps operations to be of Object type, but found "
                            << redact(subopElement);
                        severe() << message;
                        return Status(ErrorCodes::UnrecoverableRollbackError, message);
                    }
                    // In applyOps, the object contains an array of different oplog entries, we call
                    // updateFixUpInfoFromLocalOplogEntry here in order to record the information
                    // needed for rollback that is contained within the applyOps, creating a nested
                    // call.
                    auto subStatus =
                        updateFixUpInfoFromLocalOplogEntry(fixUpInfo, subopElement.Obj());
                    if (!subStatus.isOK()) {
                        return subStatus;
                    }
                }
                return Status::OK();
            }
            default: {
                std::string message = str::stream() << "Can't roll back this command yet: "
                                                    << " cmdname = " << first.fieldName();
                severe() << message << " document: " << redact(obj);
                throw RSFatalException(message);
            }
        }
    }

    doc._id = oplogEntry.getIdElement();
    if (doc._id.eoo()) {
        std::string message = str::stream() << "Cannot roll back op with no _id. ns: " << doc.ns;
        severe() << message << ", document: " << redact(oplogEntry.toBSON());
        throw RSFatalException(message);
    }

    fixUpInfo.docsToRefetch.insert(doc);
    return Status::OK();
}

namespace {

/**
 * This must be called before making any changes to our local data and after fetching any
 * information from the upstream node. If any information is fetched from the upstream node after we
 * have written locally, the function must be called again.
 */
void checkRbidAndUpdateMinValid(OperationContext* opCtx,
                                const int rbid,
                                const RollbackSource& rollbackSource,
                                ReplicationProcess* replicationProcess) {
    // It is important that the steps are performed in order to avoid racing with upstream
    // rollbacks.
    // 1. Gets the last doc in their oplog.
    // 2. Gets their RBID and fail if it has changed.
    // 3. Sets our minValid to the previously fetched OpTime of the top of their oplog.
    const auto newMinValidDoc = rollbackSource.getLastOperation();
    if (newMinValidDoc.isEmpty()) {
        uasserted(40500, "rollback error newest oplog entry on source is missing or empty");
    }
    if (rbid != rollbackSource.getRollbackId()) {
        // Our source rolled back so the data we received is not necessarily consistent.
        uasserted(40508, "rollback rbid on source changed during rollback, canceling this attempt");
    }

    // We have items we are writing that aren't from a point-in-time. Thus, it is best not to come
    // online until we get to that point in freshness. In other words, we do not transition from
    // RECOVERING state to SECONDARY state until we have reached the minValid oplog entry.

    OpTime minValid = fassertStatusOK(40492, OpTime::parseFromOplogEntry(newMinValidDoc));
    log() << "Setting minvalid to " << minValid;
    replicationProcess->getConsistencyMarkers()->setAppliedThrough(opCtx, {});  // Use top of oplog.
    replicationProcess->getConsistencyMarkers()->setMinValid(opCtx, minValid);

    if (MONGO_FAIL_POINT(rollbackHangThenFailAfterWritingMinValid)) {

        // This log output is used in jstests so please leave it.
        log() << "rollback - rollbackHangThenFailAfterWritingMinValid fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(rollbackHangThenFailAfterWritingMinValid)) {
            invariant(!globalInShutdownDeprecated());  // It is an error to shutdown while enabled.
            mongo::sleepsecs(1);
        }
        uasserted(40502,
                  "failing rollback due to rollbackHangThenFailAfterWritingMinValid fail point");
    }
}

/**
 * Drops an index from the collection based on its name by removing it from the indexCatalog of the
 * collection.
 */
void dropIndex(OperationContext* opCtx,
               IndexCatalog* indexCatalog,
               const string& indexName,
               NamespaceString& nss) {
    bool includeUnfinishedIndexes = false;
    auto indexDescriptor =
        indexCatalog->findIndexByName(opCtx, indexName, includeUnfinishedIndexes);
    if (!indexDescriptor) {
        warning() << "Rollback failed to drop index " << indexName << " in " << nss.toString()
                  << ": index not found.";
        return;
    }
    WriteUnitOfWork wunit(opCtx);
    auto status = indexCatalog->dropIndex(opCtx, indexDescriptor);
    if (!status.isOK()) {
        severe() << "Rollback failed to drop index " << indexName << " in " << nss.toString()
                 << ": " << redact(status);
    }
    wunit.commit();
}


/**
 * Rolls back all createIndexes operations for the collection by dropping the
 * created indexes.
 */
void rollbackCreateIndexes(OperationContext* opCtx, UUID uuid, std::set<std::string> indexNames) {

    Collection* collection = UUIDCatalog::get(opCtx).lookupCollectionByUUID(uuid);
    NamespaceString nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(uuid);

    // If we cannot find the collection, we skip over dropping the index.
    if (!collection) {
        LOG(2) << "Cannot find the collection with uuid: " << uuid.toString()
               << " in UUID catalog during roll back of a createIndexes command.";
        return;
    }

    Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);

    // If we cannot find the index catalog, we skip over dropping the index.
    auto indexCatalog = collection->getIndexCatalog();
    if (!indexCatalog) {
        LOG(2) << "Cannot find the index catalog in collection with uuid: " << uuid.toString()
               << " during roll back of a createIndexes command.";
        return;
    }

    for (auto itIndex = indexNames.begin(); itIndex != indexNames.end(); itIndex++) {
        const string& indexName = *itIndex;

        dropIndex(opCtx, indexCatalog, indexName, nss);
        log() << "Dropped index in rollback: collection = " << nss.toString()
              << ", index = " << indexName;
    }
}

/**
 * Rolls back all the dropIndexes operations for the collection by re-creating
 * the indexes that were dropped.
 */
void rollbackDropIndexes(OperationContext* opCtx,
                         UUID uuid,
                         std::map<std::string, BSONObj> indexNames) {

    Collection* collection = UUIDCatalog::get(opCtx).lookupCollectionByUUID(uuid);
    NamespaceString nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(uuid);

    // If we cannot find the collection, we skip over dropping the index.
    if (!collection) {
        LOG(2) << "Cannot find the collection with uuid: " << uuid.toString()
               << "in UUID catalog during roll back of a dropIndexes command.";
        return;
    }

    Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);

    for (auto itIndex = indexNames.begin(); itIndex != indexNames.end(); itIndex++) {
        const string indexName = itIndex->first;
        BSONObj indexSpec = itIndex->second;

        // We replace the namespace field because it is possible that a
        // renameCollection command has occurred, changing the namespace of the
        // collection from what it initially was during the creation of this index.
        BSONObjBuilder updatedNss;
        updatedNss.append("ns", nss.ns());
        indexSpec.addField(updatedNss.obj().firstElement());

        createIndexForApplyOps(opCtx, indexSpec, nss, {});

        log() << "Created index in rollback: collection = " << nss.toString()
              << ", index = " << indexName;
    }
}

void syncFixUp(OperationContext* opCtx,
               const FixUpInfo& fixUpInfo,
               const RollbackSource& rollbackSource,
               ReplicationCoordinator* replCoord,
               ReplicationProcess* replicationProcess) {
    unsigned long long totalSize = 0;

    // namespace -> doc id -> doc
    map<string, map<DocID, BSONObj>> goodVersions;

    // Fetches all the goodVersions of each document from the current sync source.
    unsigned long long numFetched = 0;

    log() << "Starting refetching documents";

    for (auto&& doc : fixUpInfo.docsToRefetch) {
        invariant(!doc._id.eoo());  // This is checked when we insert to the set.

        try {
            LOG(2) << "Refetching document, namespace: " << doc.ns << ", _id: " << redact(doc._id);
            // TODO : Slow. Lots of round trips.
            numFetched++;
            BSONObj good = rollbackSource.findOne(NamespaceString(doc.ns), doc._id.wrap());
            totalSize += good.objsize();

            // Checks that the total amount of data that needs to be refetched is at most
            // 300 MB. We do not roll back more than 300 MB of documents in order to
            // prevent out of memory errors from too much data being stored. See SERVER-23392.
            if (totalSize >= 300 * 1024 * 1024) {
                throw RSFatalException("replSet too much data to roll back.");
            }

            // Note good might be empty, indicating we should delete it.
            goodVersions[doc.ns][doc] = good;
        } catch (const DBException& ex) {
            // If the collection turned into a view, we might get an error trying to
            // refetch documents, but these errors should be ignored, as we'll be creating
            // the view during oplog replay.
            if (ex.getCode() == ErrorCodes::CommandNotSupportedOnView)
                continue;

            log() << "Rollback couldn't re-get from ns: " << doc.ns << " _id: " << redact(doc._id)
                  << ' ' << numFetched << '/' << fixUpInfo.docsToRefetch.size() << ": "
                  << redact(ex);
            throw;
        }
    }

    log() << "Finished refetching documents. Total size of documents refetched: "
          << goodVersions.size();

    log() << "Checking the RollbackID and updating the MinValid if necessary";

    checkRbidAndUpdateMinValid(opCtx, fixUpInfo.rbid, rollbackSource, replicationProcess);

    invariant(!fixUpInfo.commonPointOurDiskloc.isNull());

    log() << "Rolling back any collections pending being dropped";

    // Roll back any drop-pending collections. This must be done first so that the collection
    // exists when we attempt to resync its metadata or insert documents into it.
    for (const auto& collPair : fixUpInfo.collectionsToRollBackPendingDrop) {
        const auto& optime = collPair.second.first;
        const auto& collectionNamespace = collPair.second.second;
        DropPendingCollectionReaper::get(opCtx)->rollBackDropPendingCollection(
            opCtx, optime, collectionNamespace);
    }

    // Full collection data and metadata resync.
    if (!fixUpInfo.collectionsToResyncData.empty() ||
        !fixUpInfo.collectionsToResyncMetadata.empty()) {

        // Reloads the collection data from the sync source in order
        // to roll back a drop/dropIndexes/renameCollection operation.
        for (const string& ns : fixUpInfo.collectionsToResyncData) {
            log() << "Resyncing collection, namespace: " << ns;

            // TODO: This invariant will be uncommented once the
            // rollback via refetch for non-WT project is complete. See SERVER-30171.
            // invariant(!fixUpInfo.indexesToDrop.count(ns));
            // invariant(!fixUpInfo.collectionsToResyncMetadata.count(ns));

            const NamespaceString nss(ns);

            {
                Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
                Database* db = dbHolder().openDb(opCtx, nss.db().toString());
                invariant(db);
                WriteUnitOfWork wunit(opCtx);
                fassertStatusOK(40505, db->dropCollectionEvenIfSystem(opCtx, nss));
                wunit.commit();
            }

            rollbackSource.copyCollectionFromRemote(opCtx, nss);
        }

        // Retrieves collections from the sync source in order to obtain the collection
        // flags needed to roll back collMod operations. We roll back collMod operations
        // after create/renameCollection/drop commands in order to ensure that the
        // collections that we want to change actually exist. For example, if a collMod
        // occurs and then the collection is dropped. If we do not first re-create the
        // collection, we will not be able to retrieve the collection's catalog entries.
        for (auto uuid : fixUpInfo.collectionsToResyncMetadata) {
            log() << "Resyncing collection metadata, uuid: " << uuid;

            Collection* collection = UUIDCatalog::get(opCtx).lookupCollectionByUUID(uuid);
            invariant(collection);
            NamespaceString nss = collection->ns();

            Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
            auto db = dbHolder().openDb(opCtx, nss.db().toString());
            invariant(db);

            auto cce = collection->getCatalogEntry();

            auto infoResult = rollbackSource.getCollectionInfoByUUID(nss.db().toString(), uuid);

            if (!infoResult.isOK()) {
                // The collection was dropped by the sync source so we can't correctly change it
                // here. If we get to the roll-forward phase, we will drop it then. If the drop
                // is rolled back upstream and we restart, we expect to still have the
                // collection.

                log() << nss.ns() << " not found on remote host, so we do not roll back collmod "
                                     "operation. Instead, we will drop the collection soon.";
                continue;
            }

            auto info = infoResult.getValue();
            CollectionOptions options;

            // Updates the collection flags.
            if (auto optionsField = info["options"]) {
                if (optionsField.type() != Object) {
                    throw RSFatalException(str::stream() << "Failed to parse options " << info
                                                         << ": expected 'options' to be an "
                                                         << "Object, got "
                                                         << typeName(optionsField.type()));
                }

                auto status = options.parse(optionsField.Obj(), CollectionOptions::parseForCommand);
                if (!status.isOK()) {
                    throw RSFatalException(str::stream() << "Failed to parse options " << info
                                                         << ": "
                                                         << status.toString());
                }
                // TODO(SERVER-27992): Set options.uuid.
            } else {
                // Use default options.
            }

            WriteUnitOfWork wuow(opCtx);

            // Resets collection user flags such as noPadding and usePowerOf2Sizes.
            if (options.flagsSet || cce->getCollectionOptions(opCtx).flagsSet) {
                cce->updateFlags(opCtx, options.flags);
            }

            auto status = collection->setValidator(opCtx, options.validator);
            if (!status.isOK()) {
                throw RSFatalException(str::stream() << "Failed to set validator: "
                                                     << status.toString());
            }
            status = collection->setValidationAction(opCtx, options.validationAction);
            if (!status.isOK()) {
                throw RSFatalException(str::stream() << "Failed to set validationAction: "
                                                     << status.toString());
            }

            status = collection->setValidationLevel(opCtx, options.validationLevel);
            if (!status.isOK()) {
                throw RSFatalException(str::stream() << "Failed to set validationLevel: "
                                                     << status.toString());
            }

            wuow.commit();
        }

        // Since we read from the sync source to retrieve the metadata of the
        // collection, we must check if the sync source rolled back as well as update
        // minValid if necessary.
        log() << "Rechecking the Rollback ID and minValid";
        checkRbidAndUpdateMinValid(opCtx, fixUpInfo.rbid, rollbackSource, replicationProcess);
    }

    log() << "Dropping collections to roll back create operations";

    // Drops collections before updating individual documents.
    for (auto it = fixUpInfo.collectionsToDrop.begin(); it != fixUpInfo.collectionsToDrop.end();
         it++) {

        // TODO: This invariant will be uncommented once the rollback via refetch for non-WT
        // project is complete. See SERVER-30171.
        // invariant(!fixUpInfo.indexesToDrop.count(*it));

        Collection* collection = UUIDCatalog::get(opCtx).lookupCollectionByUUID(*it);
        NamespaceString nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(*it);

        Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
        Database* db = dbHolder().get(opCtx, nss.db());
        if (db) {
            Helpers::RemoveSaver removeSaver("rollback", "", nss.ns());

            // Performs a collection scan and writes all documents in the collection to disk
            // in order to keep an archive of items that were rolled back.
            auto exec = InternalPlanner::collectionScan(
                opCtx, nss.toString(), collection, PlanExecutor::YIELD_AUTO);
            BSONObj curObj;
            PlanExecutor::ExecState execState;
            while (PlanExecutor::ADVANCED == (execState = exec->getNext(&curObj, NULL))) {
                auto status = removeSaver.goingToDelete(curObj);
                if (!status.isOK()) {
                    severe() << "Rolling back createCollection on " << nss
                             << " failed to write document to remove saver file: "
                             << redact(status);
                    throw RSFatalException(
                        "Rolling back createCollection. Failed to write document to remove saver "
                        "file.");
                }
            }

            // If we exited the above for loop with any other execState than IS_EOF, this means that
            // a FAILURE or DEAD state was returned. If a DEAD state occurred, the collection or
            // database that we are attempting to save may no longer be valid. If a FAILURE state
            // was returned, either an unrecoverable error was thrown by exec, or we attempted to
            // retrieve data that could not be provided by the PlanExecutor. In both of these cases
            // it is necessary for a full resync of the server.

            if (execState != PlanExecutor::IS_EOF) {
                if (execState == PlanExecutor::FAILURE &&
                    WorkingSetCommon::isValidStatusMemberObject(curObj)) {
                    Status errorStatus = WorkingSetCommon::getMemberObjectStatus(curObj);
                    severe() << "Rolling back createCollection on " << nss << " failed with "
                             << redact(errorStatus) << ". A full resync is necessary.";
                    throw RSFatalException(
                        "Rolling back createCollection failed. A full resync is necessary.");
                } else {
                    severe() << "Rolling back createCollection on " << nss
                             << " failed. A full resync is necessary.";
                    throw RSFatalException(
                        "Rolling back createCollection failed. A full resync is necessary.");
                }
            }

            WriteUnitOfWork wunit(opCtx);

            // We permanently drop the collection rather than 2-phase drop the collection
            // here. By not passing in an opTime to dropCollectionEvenIfSystem() the collection
            // is immediately dropped.
            fassertStatusOK(40504, db->dropCollectionEvenIfSystem(opCtx, nss));
            wunit.commit();

            log() << "Dropped collection with UUID: " << *it << " and nss: " << nss;
        }
    }

    // Rolls back createIndexes commands by dropping the indexes that were created. It is
    // necessary to roll back createIndexes commands before dropIndexes commands because
    // it is possible that we previously dropped an index with the same name but a different
    // index spec. If we attempt to re-create an index that has the same name as an existing
    // index, the operation will fail. Thus, we roll back createIndexes commands first in
    // order to ensure that no collisions will occur when we re-create previously dropped
    // indexes.
    log() << "Rolling back createIndexes commands.";

    for (auto it = fixUpInfo.indexesToDrop.begin(); it != fixUpInfo.indexesToDrop.end(); it++) {

        UUID uuid = it->first;
        auto indexNames = it->second;

        rollbackCreateIndexes(opCtx, uuid, indexNames);
    }

    // Rolls back dropIndexes commands by re-creating the indexes that were dropped.
    log() << "Rolling back dropIndexes commands.";
    for (auto it = fixUpInfo.indexesToCreate.begin(); it != fixUpInfo.indexesToCreate.end(); it++) {

        UUID uuid = it->first;
        auto indexNames = it->second;

        rollbackDropIndexes(opCtx, uuid, indexNames);
    }

    log() << "Deleting and updating documents to roll back insert, update and remove "
             "operations";
    unsigned deletes = 0, updates = 0;
    time_t lastProgressUpdate = time(0);
    time_t progressUpdateGap = 10;

    for (const auto& nsAndGoodVersionsByDocID : goodVersions) {

        // Keeps an archive of items rolled back if the collection has not been dropped
        // while rolling back createCollection operations.
        const auto& ns = nsAndGoodVersionsByDocID.first;
        unique_ptr<Helpers::RemoveSaver> removeSaver;

        // TODO: This invariant will be uncommented once the
        // rollback via refetch for non-WT project is complete. See SERVER-30171
        // invariant(!fixUpInfo.collectionsToDrop.count(ns));
        removeSaver.reset(new Helpers::RemoveSaver("rollback", "", ns));

        const auto& goodVersionsByDocID = nsAndGoodVersionsByDocID.second;
        for (const auto& idAndDoc : goodVersionsByDocID) {
            time_t now = time(0);
            if (now - lastProgressUpdate > progressUpdateGap) {
                log() << deletes << " delete and " << updates
                      << " update operations processed out of " << goodVersions.size()
                      << " total operations.";
                lastProgressUpdate = now;
            }
            const DocID& doc = idAndDoc.first;
            BSONObj pattern = doc._id.wrap();  // { _id : ... }
            try {
                verify(doc.ns && *doc.ns);
                invariant(!fixUpInfo.collectionsToResyncData.count(doc.ns));

                // TODO: Lots of overhead in context. This can be faster.
                const NamespaceString docNss(doc.ns);
                Lock::DBLock docDbLock(opCtx, docNss.db(), MODE_X);
                OldClientContext ctx(opCtx, doc.ns);

                Collection* collection = ctx.db()->getCollection(opCtx, docNss);

                // Adds the doc to our rollback file if the collection was not dropped while
                // rolling back createCollection operations. Does not log an error when
                // undoing an insert on a no longer existing collection. It is likely that
                // the collection was dropped as part of rolling back a createCollection
                // command and the document no longer exists.

                if (collection && removeSaver) {
                    BSONObj obj;
                    bool found = Helpers::findOne(opCtx, collection, pattern, obj, false);
                    if (found) {
                        auto status = removeSaver->goingToDelete(obj);
                        if (!status.isOK()) {
                            severe() << "Rollback cannot write document in namespace " << doc.ns
                                     << " to archive file: " << redact(status);
                            throw RSFatalException(str::stream()
                                                   << "Rollback cannot write document in namespace "
                                                   << doc.ns
                                                   << " to archive file.");
                        }
                    } else {
                        error() << "Rollback cannot find object: " << pattern << " in namespace "
                                << doc.ns;
                    }
                }

                if (idAndDoc.second.isEmpty()) {
                    // If the document could not be found on the primary, deletes the document.
                    // TODO 1.6 : can't delete from a capped collection. Need to handle that
                    // here.
                    deletes++;

                    if (collection) {
                        if (collection->isCapped()) {
                            // Can't delete from a capped collection - so we truncate instead.
                            // if this item must go, so must all successors.

                            try {
                                // TODO: IIRC cappedTruncateAfter does not handle completely
                                // empty. This will be slow if there is no _id index in
                                // the collection.

                                const auto clock = opCtx->getServiceContext()->getFastClockSource();
                                const auto findOneStart = clock->now();
                                RecordId loc = Helpers::findOne(opCtx, collection, pattern, false);
                                if (clock->now() - findOneStart > Milliseconds(200))
                                    warning() << "Roll back slow no _id index for " << doc.ns
                                              << " perhaps?";
                                // Would be faster but requires index:
                                // RecordId loc = Helpers::findById(nsd, pattern);
                                if (!loc.isNull()) {
                                    try {
                                        collection->cappedTruncateAfter(opCtx, loc, true);
                                    } catch (const DBException& e) {
                                        if (e.getCode() == 13415) {
                                            // hack: need to just make cappedTruncate do this...
                                            writeConflictRetry(
                                                opCtx, "truncate", collection->ns().ns(), [&] {
                                                    WriteUnitOfWork wunit(opCtx);
                                                    uassertStatusOK(collection->truncate(opCtx));
                                                    wunit.commit();
                                                });
                                        } else {
                                            throw e;
                                        }
                                    }
                                }
                            } catch (const DBException& e) {
                                // Replicated capped collections have many ways to become
                                // inconsistent. We rely on age-out to make these problems go away
                                // eventually.

                                warning() << "Ignoring failure to roll back change to capped "
                                          << "collection " << doc.ns << " with _id "
                                          << redact(idAndDoc.first._id.toString(
                                                 /*includeFieldName*/ false))
                                          << ": " << redact(e);
                            }
                        } else {
                            deleteObjects(opCtx,
                                          collection,
                                          docNss,
                                          pattern,
                                          true,   // justOne
                                          true);  // god
                        }
                    }
                } else {
                    // TODO faster...
                    updates++;

                    UpdateRequest request(docNss);

                    request.setQuery(pattern);
                    request.setUpdates(idAndDoc.second);
                    request.setGod();
                    request.setUpsert();
                    UpdateLifecycleImpl updateLifecycle(docNss);
                    request.setLifecycle(&updateLifecycle);

                    update(opCtx, ctx.db(), request);
                }
            } catch (const DBException& e) {
                log() << "Exception in rollback ns:" << doc.ns << ' ' << pattern.toString() << ' '
                      << redact(e) << " ndeletes:" << deletes;
                throw;
            }
        }
    }

    log() << "Rollback deleted " << deletes << " documents and updated " << updates
          << " documents.";

    log() << "Truncating the oplog at " << fixUpInfo.commonPoint.toString();

    // Cleans up the oplog.
    {
        const NamespaceString oplogNss(NamespaceString::kRsOplogNamespace);
        Lock::DBLock oplogDbLock(opCtx, oplogNss.db(), MODE_IX);
        Lock::CollectionLock oplogCollectionLoc(opCtx->lockState(), oplogNss.ns(), MODE_X);
        OldClientContext ctx(opCtx, oplogNss.ns());
        Collection* oplogCollection = ctx.db()->getCollection(opCtx, oplogNss);
        if (!oplogCollection) {
            fassertFailedWithStatusNoTrace(
                40495,
                Status(ErrorCodes::UnrecoverableRollbackError,
                       str::stream() << "Can't find " << NamespaceString::kRsOplogNamespace.ns()));
        }
        // TODO: fatal error if this throws?
        oplogCollection->cappedTruncateAfter(opCtx, fixUpInfo.commonPointOurDiskloc, false);
    }

    Status status = getGlobalAuthorizationManager()->initialize(opCtx);
    if (!status.isOK()) {
        severe() << "Failed to reinitialize auth data after rollback: " << redact(status);
        fassertFailedNoTrace(40496);
    }

    // If necessary, clear the in-memory session transaction table.
    if (fixUpInfo.refetchTransactionDocs) {
        SessionCatalog::get(opCtx)->clearTransactionTable();
    }

    // Reload the lastAppliedOpTime and lastDurableOpTime value in the replcoord and the
    // lastAppliedHash value in bgsync to reflect our new last op.
    replCoord->resetLastOpTimesFromOplog(opCtx);
}

Status _syncRollback(OperationContext* opCtx,
                     const OplogInterface& localOplog,
                     const RollbackSource& rollbackSource,
                     int requiredRBID,
                     ReplicationCoordinator* replCoord,
                     ReplicationProcess* replicationProcess) {
    invariant(!opCtx->lockState()->isLocked());

    FixUpInfo how;
    log() << "Starting rollback. Sync source: " << rollbackSource.getSource() << rsLog;
    how.rbid = rollbackSource.getRollbackId();
    uassert(
        40506, "Upstream node rolled back. Need to retry our rollback.", how.rbid == requiredRBID);

    log() << "Finding the Common Point";
    try {

        auto processOperationForFixUp = [&how](const BSONObj& operation) {
            return updateFixUpInfoFromLocalOplogEntry(how, operation);
        };

        // Calls syncRollBackLocalOperations to run updateFixUpInfoFromLocalOplogEntry
        // on each oplog entry up until the common point.
        auto res = syncRollBackLocalOperations(
            localOplog, rollbackSource.getOplog(), processOperationForFixUp);
        if (!res.isOK()) {
            const auto status = res.getStatus();
            switch (status.code()) {
                case ErrorCodes::OplogStartMissing:
                case ErrorCodes::UnrecoverableRollbackError:
                    return status;
                default:
                    throw RSFatalException(status.toString());
            }
        }

        how.commonPoint = res.getValue().first;             // OpTime
        how.commonPointOurDiskloc = res.getValue().second;  // RecordID
        how.removeRedundantOperations();
    } catch (const RSFatalException& e) {
        return Status(ErrorCodes::UnrecoverableRollbackError,
                      str::stream()
                          << "need to rollback, but unable to determine common point between"
                             " local and remote oplog: "
                          << e.what(),
                      18752);
    }

    log() << "Rollback common point is " << how.commonPoint;
    try {
        ON_BLOCK_EXIT([&] {
            auto status = replicationProcess->incrementRollbackID(opCtx);
            fassertStatusOK(40497, status);
        });
        syncFixUp(opCtx, how, rollbackSource, replCoord, replicationProcess);
    } catch (const RSFatalException& e) {
        return Status(ErrorCodes::UnrecoverableRollbackError, e.what(), 18753);
    }

    if (MONGO_FAIL_POINT(rollbackHangBeforeFinish)) {
        // This log output is used in js tests so please leave it.
        log() << "rollback - rollbackHangBeforeFinish fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(rollbackHangBeforeFinish)) {
            invariant(!globalInShutdownDeprecated());  // It is an error to shutdown while enabled.
            mongo::sleepsecs(1);
        }
    }

    return Status::OK();
}

}  // namespace

Status syncRollback(OperationContext* opCtx,
                    const OplogInterface& localOplog,
                    const RollbackSource& rollbackSource,
                    int requiredRBID,
                    ReplicationCoordinator* replCoord,
                    ReplicationProcess* replicationProcess) {
    invariant(opCtx);
    invariant(replCoord);

    DisableDocumentValidation validationDisabler(opCtx);
    UnreplicatedWritesBlock replicationDisabler(opCtx);
    Status status = _syncRollback(
        opCtx, localOplog, rollbackSource, requiredRBID, replCoord, replicationProcess);

    log() << "Rollback finished. The final minValid is: "
          << replicationProcess->getConsistencyMarkers()->getMinValid(opCtx) << rsLog;

    return status;
}

void rollback(OperationContext* opCtx,
              const OplogInterface& localOplog,
              const RollbackSource& rollbackSource,
              int requiredRBID,
              ReplicationCoordinator* replCoord,
              ReplicationProcess* replicationProcess,
              stdx::function<void(int)> sleepSecsFn) {
    // Set state to ROLLBACK while we are in this function. This prevents serving reads, even from
    // the oplog. This can fail if we are elected PRIMARY, in which case we better not do any
    // rolling back. If we successfully enter ROLLBACK we will only exit this function fatally or
    // after transitioning to RECOVERING. We always transition to RECOVERING regardless of success
    // or (recoverable) failure since we may be in an inconsistent state. If rollback failed before
    // writing anything, SyncTail will quickly take us to SECONDARY since are are still at our
    // original MinValid, which is fine because we may choose a sync source that doesn't require
    // rollback. If it failed after we wrote to MinValid, then we will pick a sync source that will
    // cause us to roll back to the same common point, which is fine. If we succeeded, we will be
    // consistent as soon as we apply up to/through MinValid and SyncTail will make us SECONDARY
    // then.

    {
        Lock::GlobalWrite globalWrite(opCtx);
        auto status = replCoord->setFollowerMode(MemberState::RS_ROLLBACK);
        if (!status.isOK()) {
            log() << "Cannot transition from " << replCoord->getMemberState().toString() << " to "
                  << MemberState(MemberState::RS_ROLLBACK).toString() << causedBy(status);
            return;
        }
    }

    try {
        auto status = syncRollback(
            opCtx, localOplog, rollbackSource, requiredRBID, replCoord, replicationProcess);

        // Aborts only when syncRollback detects we are in a unrecoverable state.
        // WARNING: these statuses sometimes have location codes which are lost with uassertStatusOK
        // so we need to check here first.
        if (ErrorCodes::UnrecoverableRollbackError == status.code()) {
            severe() << "Unable to complete rollback. A full resync may be needed: "
                     << redact(status);
            fassertFailedNoTrace(40507);
        }

        // In other cases, we log the message contained in the error status and retry later.
        uassertStatusOK(status);
    } catch (const DBException& ex) {
        // UnrecoverableRollbackError should only come from a returned status which is handled
        // above.
        invariant(ex.getCode() != ErrorCodes::UnrecoverableRollbackError);

        warning() << "Rollback cannot complete at this time (retrying later): " << redact(ex)
                  << " appliedThrough= " << replCoord->getMyLastAppliedOpTime() << " minvalid= "
                  << replicationProcess->getConsistencyMarkers()->getMinValid(opCtx);

        // Sleep a bit to allow upstream node to coalesce, if that was the cause of the failure. If
        // we failed in a way that will keep failing, but wasn't flagged as a fatal failure, this
        // will also prevent us from hot-looping and putting too much load on upstream nodes.
        sleepSecsFn(5);  // 5 seconds was chosen as a completely arbitrary amount of time.
    } catch (...) {
        std::terminate();
    }

    // At this point we are about to leave rollback.  Before we do, wait for any writes done
    // as part of rollback to be durable, and then do any necessary checks that we didn't
    // wind up rolling back something illegal.  We must wait for the rollback to be durable
    // so that if we wind up shutting down uncleanly in response to something we rolled back
    // we know that we won't wind up right back in the same situation when we start back up
    // because the rollback wasn't durable.
    opCtx->recoveryUnit()->waitUntilDurable();

    // If we detected that we rolled back the shardIdentity document as part of this rollback
    // then we must shut down to clear the in-memory ShardingState associated with the
    // shardIdentity document.
    if (ShardIdentityRollbackNotifier::get(opCtx)->didRollbackHappen()) {
        severe() << "shardIdentity document rollback detected.  Shutting down to clear "
                    "in-memory sharding state.  Restarting this process should safely return it "
                    "to a healthy state";
        fassertFailedNoTrace(40498);
    }

    auto status = replCoord->setFollowerMode(MemberState::RS_RECOVERING);
    if (!status.isOK()) {
        severe() << "Failed to transition into " << MemberState(MemberState::RS_RECOVERING)
                 << "; expected to be in state " << MemberState(MemberState::RS_ROLLBACK)
                 << "; found self in " << replCoord->getMemberState() << causedBy(status);
        fassertFailedNoTrace(40499);
    }
}

}  // namespace repl
}  // namespace mongo
