/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/platform/basic.h"

#include "mongo/db/repl/storage_interface_impl.h"

#include <algorithm>
#include <boost/optional.hpp>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/collection_bulk_loader_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/rollback_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/checkpointer.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/oplog_cap_maintainer_thread.h"
#include "mongo/db/storage/storage_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(holdStableTimestampAtSpecificTimestamp);

const char StorageInterfaceImpl::kDefaultRollbackIdNamespace[] = "local.system.rollback.id";
const char StorageInterfaceImpl::kRollbackIdFieldName[] = "rollbackId";
const char StorageInterfaceImpl::kRollbackIdDocumentId[] = "rollbackId";

namespace {
using UniqueLock = stdx::unique_lock<Latch>;

const auto kIdIndexName = "_id_"_sd;

}  // namespace

StorageInterfaceImpl::StorageInterfaceImpl()
    : _rollbackIdNss(StorageInterfaceImpl::kDefaultRollbackIdNamespace) {}

StatusWith<int> StorageInterfaceImpl::getRollbackID(OperationContext* opCtx) {
    BSONObjBuilder bob;
    bob.append("_id", kRollbackIdDocumentId);
    auto id = bob.obj();

    try {
        auto rbidDoc = findById(opCtx, _rollbackIdNss, id["_id"]);
        if (!rbidDoc.isOK()) {
            return rbidDoc.getStatus();
        }

        auto rbid = RollbackID::parse(IDLParserContext("RollbackID"), rbidDoc.getValue());
        invariant(rbid.get_id() == kRollbackIdDocumentId);
        return rbid.getRollbackId();
    } catch (const DBException&) {
        return exceptionToStatus();
    }

    MONGO_UNREACHABLE;
}

StatusWith<int> StorageInterfaceImpl::initializeRollbackID(OperationContext* opCtx) {
    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    auto status = createCollection(opCtx, _rollbackIdNss, CollectionOptions());
    if (!status.isOK()) {
        return status;
    }

    RollbackID rbid;
    int initRBID = 1;
    rbid.set_id(kRollbackIdDocumentId);
    rbid.setRollbackId(initRBID);

    BSONObjBuilder bob;
    rbid.serialize(&bob);
    Timestamp noTimestamp;  // This write is not replicated.
    status = insertDocument(opCtx,
                            _rollbackIdNss,
                            TimestampedBSONObj{bob.done(), noTimestamp},
                            OpTime::kUninitializedTerm);
    if (status.isOK()) {
        return initRBID;
    } else {
        return status;
    }
}

StatusWith<int> StorageInterfaceImpl::incrementRollbackID(OperationContext* opCtx) {
    // This is safe because this is only called during rollback, and you can not have two
    // rollbacks at once.
    auto rbidSW = getRollbackID(opCtx);
    if (!rbidSW.isOK()) {
        return rbidSW;
    }

    // If we would go over the integer limit, reset the Rollback ID to 1.
    BSONObjBuilder updateBob;
    int newRBID = -1;
    if (rbidSW.getValue() == std::numeric_limits<int>::max()) {
        newRBID = 1;
        BSONObjBuilder setBob(updateBob.subobjStart("$set"));
        setBob.append(kRollbackIdFieldName, newRBID);
    } else {
        BSONObjBuilder incBob(updateBob.subobjStart("$inc"));
        incBob.append(kRollbackIdFieldName, 1);
        newRBID = rbidSW.getValue() + 1;
    }

    // Since the Rollback ID is in a singleton collection, we can fix the _id field.
    BSONObjBuilder bob;
    bob.append("_id", kRollbackIdDocumentId);
    auto id = bob.obj();
    Status status = upsertById(opCtx, _rollbackIdNss, id["_id"], updateBob.obj());

    // We wait until durable so that we are sure the Rollback ID is updated before rollback ends.
    if (status.isOK()) {
        JournalFlusher::get(opCtx)->waitForJournalFlush();
        return newRBID;
    }
    return status;
}

StatusWith<std::unique_ptr<CollectionBulkLoader>>
StorageInterfaceImpl::createCollectionForBulkLoading(
    const NamespaceString& nss,
    const CollectionOptions& options,
    const BSONObj idIndexSpec,
    const std::vector<BSONObj>& secondaryIndexSpecs) {

    LOGV2_DEBUG(21753,
                2,
                "StorageInterfaceImpl::createCollectionForBulkLoading called for ns: {namespace}",
                "StorageInterfaceImpl::createCollectionForBulkLoading called",
                "namespace"_attr = nss.ns());

    class StashClient {
    public:
        StashClient() {
            if (Client::getCurrent()) {
                _stashedClient = Client::releaseCurrent();
            }
        }
        ~StashClient() {
            if (Client::getCurrent()) {
                Client::releaseCurrent();
            }
            if (_stashedClient) {
                Client::setCurrent(std::move(_stashedClient));
            }
        }

    private:
        ServiceContext::UniqueClient _stashedClient;
    } stash;
    Client::setCurrent(
        getGlobalServiceContext()->makeClient(str::stream() << nss.ns() << " loader"));
    auto opCtx = cc().makeOperationContext();
    opCtx->setEnforceConstraints(false);

    // DocumentValidationSettings::kDisableInternalValidation is currently inert.
    // But, it's logically ok to disable internal validation as this function gets called
    // only during initial sync.
    DocumentValidationSettings::get(opCtx.get())
        .setFlags(DocumentValidationSettings::kDisableSchemaValidation |
                  DocumentValidationSettings::kDisableInternalValidation);

    std::unique_ptr<AutoGetCollection> autoColl;
    // Retry if WCE.
    Status status = writeConflictRetry(opCtx.get(), "beginCollectionClone", nss.ns(), [&] {
        UnreplicatedWritesBlock uwb(opCtx.get());

        // Get locks and create the collection.
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_IX);
        AutoGetCollection coll(opCtx.get(), nss, fixLockModeForSystemDotViewsChanges(nss, MODE_X));
        if (coll) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "Collection " << nss.ns() << " already exists.");
        }
        {
            // Create the collection.
            WriteUnitOfWork wunit(opCtx.get());
            auto db = autoDb.ensureDbExists(opCtx.get());
            fassert(40332, db->createCollection(opCtx.get(), nss, options, false));
            wunit.commit();
        }

        autoColl = std::make_unique<AutoGetCollection>(
            opCtx.get(), nss, fixLockModeForSystemDotViewsChanges(nss, MODE_IX));

        // Build empty capped indexes.  Capped indexes cannot be built by the MultiIndexBlock
        // because the cap might delete documents off the back while we are inserting them into
        // the front.
        if (options.capped) {
            WriteUnitOfWork wunit(opCtx.get());
            if (!idIndexSpec.isEmpty()) {
                auto status =
                    autoColl->getWritableCollection(opCtx.get())
                        ->getIndexCatalog()
                        ->createIndexOnEmptyCollection(
                            opCtx.get(), autoColl->getWritableCollection(opCtx.get()), idIndexSpec);
                if (!status.getStatus().isOK()) {
                    return status.getStatus();
                }
            }
            for (auto&& spec : secondaryIndexSpecs) {
                auto status =
                    autoColl->getWritableCollection(opCtx.get())
                        ->getIndexCatalog()
                        ->createIndexOnEmptyCollection(
                            opCtx.get(), autoColl->getWritableCollection(opCtx.get()), spec);
                if (!status.getStatus().isOK()) {
                    return status.getStatus();
                }
            }
            wunit.commit();
        }

        return Status::OK();
    });

    if (!status.isOK()) {
        return status;
    }

    // Move locks into loader, so it now controls their lifetime.
    auto loader =
        std::make_unique<CollectionBulkLoaderImpl>(Client::releaseCurrent(),
                                                   std::move(opCtx),
                                                   std::move(autoColl),
                                                   options.capped ? BSONObj() : idIndexSpec);

    status = loader->init(options.capped ? std::vector<BSONObj>() : secondaryIndexSpecs);
    if (!status.isOK()) {
        return status;
    }
    return {std::move(loader)};
}

Status StorageInterfaceImpl::insertDocument(OperationContext* opCtx,
                                            const NamespaceStringOrUUID& nsOrUUID,
                                            const TimestampedBSONObj& doc,
                                            long long term) {
    return insertDocuments(opCtx, nsOrUUID, {InsertStatement(doc.obj, doc.timestamp, term)});
}

namespace {

/**
 * Returns const CollectionPtr& from database RAII object.
 * Returns NamespaceNotFound if the database or collection does not exist.
 */
template <typename AutoGetCollectionType>
StatusWith<const CollectionPtr*> getCollection(const AutoGetCollectionType& autoGetCollection,
                                               const NamespaceStringOrUUID& nsOrUUID,
                                               const std::string& message) {
    const auto& collection = autoGetCollection.getCollection();
    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection [" << nsOrUUID.toString() << "] not found. "
                              << message};
    }

    return &collection;
}

Status insertDocumentsSingleBatch(OperationContext* opCtx,
                                  const NamespaceStringOrUUID& nsOrUUID,
                                  std::vector<InsertStatement>::const_iterator begin,
                                  std::vector<InsertStatement>::const_iterator end) {
    boost::optional<AutoGetCollection> autoColl;
    boost::optional<AutoGetOplog> autoOplog;
    const CollectionPtr* collection;

    auto nss = nsOrUUID.nss();
    if (nss && nss->isOplog()) {
        // Simplify locking rules for oplog collection.
        autoOplog.emplace(opCtx, OplogAccessMode::kWrite);
        collection = &autoOplog->getCollection();
        if (!*collection) {
            return {ErrorCodes::NamespaceNotFound, "Oplog collection does not exist"};
        }
    } else {
        autoColl.emplace(opCtx, nsOrUUID, MODE_IX);
        auto collectionResult = getCollection(
            autoColl.get(), nsOrUUID, "The collection must exist before inserting documents.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        collection = collectionResult.getValue();
    }

    WriteUnitOfWork wunit(opCtx);
    OpDebug* const nullOpDebug = nullptr;
    auto status = (*collection)->insertDocuments(opCtx, begin, end, nullOpDebug, false);
    if (!status.isOK()) {
        return status;
    }

    wunit.commit();

    return Status::OK();
}

}  // namespace

Status StorageInterfaceImpl::insertDocuments(OperationContext* opCtx,
                                             const NamespaceStringOrUUID& nsOrUUID,
                                             const std::vector<InsertStatement>& docs) {
    return storage_helpers::insertBatchAndHandleRetry(
        opCtx, nsOrUUID, docs, [&](auto* opCtx, auto begin, auto end) {
            return insertDocumentsSingleBatch(opCtx, nsOrUUID, begin, end);
        });
}

Status StorageInterfaceImpl::dropReplicatedDatabases(OperationContext* opCtx) {
    Lock::GlobalWrite globalWriteLock(opCtx);

    std::vector<DatabaseName> dbNames =
        opCtx->getServiceContext()->getStorageEngine()->listDatabases();
    invariant(!dbNames.empty());
    LOGV2(21754,
          "dropReplicatedDatabases - dropping {numDatabases} databases",
          "dropReplicatedDatabases - dropping databases",
          "numDatabases"_attr = dbNames.size());

    ReplicationCoordinator::get(opCtx)->clearCommittedSnapshot();

    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto hasLocalDatabase = false;
    for (const auto& dbName : dbNames) {
        if (dbName.db() == "local") {
            hasLocalDatabase = true;
            continue;
        }
        writeConflictRetry(opCtx, "dropReplicatedDatabases", dbName.toString(), [&] {
            if (auto db = databaseHolder->getDb(opCtx, dbName)) {
                databaseHolder->dropDb(opCtx, db);
            } else {
                // This is needed since dropDatabase can't be rolled back.
                // This is safe be replaced by "invariant(db);dropDatabase(opCtx, db);" once fixed.
                LOGV2(21755,
                      "dropReplicatedDatabases - database disappeared after retrieving list of "
                      "database names but before drop: {dbName}",
                      "dropReplicatedDatabases - database disappeared after retrieving list of "
                      "database names but before drop",
                      "dbName"_attr = dbName);
            }
        });
    }
    invariant(hasLocalDatabase, "local database missing");
    LOGV2(21756,
          "dropReplicatedDatabases - dropped {numDatabases} databases",
          "dropReplicatedDatabases - dropped databases",
          "numDatabases"_attr = dbNames.size());

    return Status::OK();
}

Status StorageInterfaceImpl::createOplog(OperationContext* opCtx, const NamespaceString& nss) {
    mongo::repl::createOplog(opCtx, nss, true);
    return Status::OK();
}

StatusWith<size_t> StorageInterfaceImpl::getOplogMaxSize(OperationContext* opCtx) {
    AutoGetOplog oplogRead(opCtx, OplogAccessMode::kRead);
    const auto& oplog = oplogRead.getCollection();
    if (!oplog) {
        return {ErrorCodes::NamespaceNotFound, "Your oplog doesn't exist."};
    }
    const auto options = oplog->getCollectionOptions();
    if (!options.capped)
        return {ErrorCodes::BadValue,
                str::stream() << NamespaceString::kRsOplogNamespace.ns() << " isn't capped"};
    return options.cappedSize;
}

Status StorageInterfaceImpl::createCollection(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const CollectionOptions& options,
                                              const bool createIdIndex,
                                              const BSONObj& idIndexSpec) {
    return writeConflictRetry(opCtx, "StorageInterfaceImpl::createCollection", nss.ns(), [&] {
        AutoGetDb databaseWriteGuard(opCtx, nss.dbName(), MODE_IX);
        auto db = databaseWriteGuard.ensureDbExists(opCtx);
        invariant(db);

        // Check if there already exist a Collection/view on the given namespace 'nss'. The answer
        // may change at any point after this call as we make this call without holding the
        // collection lock. But, it is fine as we properly handle while registering the uncommitted
        // collection with CollectionCatalog. This check is just here to prevent it from being
        // created in the common case.
        Status status = mongo::catalog::checkIfNamespaceExists(opCtx, nss);
        if (!status.isOK()) {
            return status;
        }

        Lock::CollectionLock lk(opCtx, nss, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        try {
            auto coll = db->createCollection(opCtx, nss, options, createIdIndex, idIndexSpec);
            invariant(coll);

            // This commit call can throw if a view already exists while registering the collection.
            wuow.commit();
        } catch (const AssertionException& ex) {
            return ex.toStatus();
        }

        return Status::OK();
    });
}

Status StorageInterfaceImpl::createIndexesOnEmptyCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<BSONObj>& secondaryIndexSpecs) {
    if (!secondaryIndexSpecs.size())
        return Status::OK();

    try {
        writeConflictRetry(
            opCtx, "StorageInterfaceImpl::createIndexesOnEmptyCollection", nss.ns(), [&] {
                AutoGetCollection autoColl(
                    opCtx, nss, fixLockModeForSystemDotViewsChanges(nss, MODE_X));
                CollectionWriter collection(opCtx, nss);

                WriteUnitOfWork wunit(opCtx);
                // Use IndexBuildsCoordinator::createIndexesOnEmptyCollection() rather than
                // IndexCatalog::createIndexOnEmptyCollection() as the former generates
                // 'createIndexes' oplog entry for replicated writes.
                IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                    opCtx, collection, secondaryIndexSpecs, false /* fromMigrate */);
                wunit.commit();
            });
    } catch (DBException& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

Status StorageInterfaceImpl::dropCollection(OperationContext* opCtx, const NamespaceString& nss) {
    return writeConflictRetry(opCtx, "StorageInterfaceImpl::dropCollection", nss.ns(), [&] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        if (!autoDb.getDb()) {
            // Database does not exist - nothing to do.
            return Status::OK();
        }
        WriteUnitOfWork wunit(opCtx);
        const auto status = autoDb.getDb()->dropCollectionEvenIfSystem(opCtx, nss);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();
        return Status::OK();
    });
}

Status StorageInterfaceImpl::truncateCollection(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    return writeConflictRetry(opCtx, "StorageInterfaceImpl::truncateCollection", nss.ns(), [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        auto collectionResult =
            getCollection(autoColl, nss, "The collection must exist before truncating.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }

        WriteUnitOfWork wunit(opCtx);
        const auto status = autoColl.getWritableCollection(opCtx)->truncate(opCtx);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();
        return Status::OK();
    });
}

Status StorageInterfaceImpl::renameCollection(OperationContext* opCtx,
                                              const NamespaceString& fromNS,
                                              const NamespaceString& toNS,
                                              bool stayTemp) {
    if (fromNS.db() != toNS.db()) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Cannot rename collection between databases. From NS: "
                                    << fromNS.ns() << "; to NS: " << toNS.ns());
    }

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::renameCollection", fromNS.ns(), [&] {
        AutoGetDb autoDB(opCtx, fromNS.dbName(), MODE_X);
        if (!autoDB.getDb()) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream()
                              << "Cannot rename collection from " << fromNS.ns() << " to "
                              << toNS.ns() << ". Database " << fromNS.db() << " not found.");
        }
        WriteUnitOfWork wunit(opCtx);
        const auto status = autoDB.getDb()->renameCollection(opCtx, fromNS, toNS, stayTemp);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();
        return status;
    });
}

Status StorageInterfaceImpl::setIndexIsMultikey(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const UUID& collectionUUID,
                                                const std::string& indexName,
                                                const KeyStringSet& multikeyMetadataKeys,
                                                const MultikeyPaths& paths,
                                                Timestamp ts) {
    if (ts.isNull()) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Cannot set index " << indexName << " on " << nss.ns()
                                    << " (" << collectionUUID << ") as multikey at null timestamp");
    }

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::setIndexIsMultikey", nss.ns(), [&] {
        const NamespaceStringOrUUID nsOrUUID(nss.db().toString(), collectionUUID);
        boost::optional<AutoGetCollection> autoColl;
        try {
            autoColl.emplace(opCtx, nsOrUUID, MODE_IX);
        } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
            return ex.toStatus();
        }
        auto collectionResult = getCollection(
            *autoColl, nsOrUUID, "The collection must exist before setting an index to multikey.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        const auto& collection = *collectionResult.getValue();

        WriteUnitOfWork wunit(opCtx);
        auto tsResult = opCtx->recoveryUnit()->setTimestamp(ts);
        if (!tsResult.isOK()) {
            return tsResult;
        }

        auto idx = collection->getIndexCatalog()->findIndexByName(
            opCtx,
            indexName,
            IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);
        if (!idx) {
            return Status(ErrorCodes::IndexNotFound,
                          str::stream()
                              << "Could not find index " << indexName << " in " << nss.ns() << " ("
                              << collectionUUID << ") to set to multikey.");
        }
        collection->getIndexCatalog()->setMultikeyPaths(
            opCtx, collection, idx, multikeyMetadataKeys, paths);
        wunit.commit();
        return Status::OK();
    });
}

namespace {

/**
 * Returns DeleteStageParams for deleteOne with fetch.
 */
std::unique_ptr<DeleteStageParams> makeDeleteStageParamsForDeleteDocuments() {
    auto deleteStageParams = std::make_unique<DeleteStageParams>();
    deleteStageParams->isMulti = true;
    deleteStageParams->returnDeleted = true;
    return deleteStageParams;
}

/**
 * Shared implementation between findDocuments, deleteDocuments, and _findOrDeleteById.
 * _findOrDeleteById is used by findById, and deleteById.
 */
enum class FindDeleteMode { kFind, kDelete };
StatusWith<std::vector<BSONObj>> _findOrDeleteDocuments(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    boost::optional<StringData> indexName,
    StorageInterface::ScanDirection scanDirection,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    std::size_t limit,
    FindDeleteMode mode) {
    auto isFind = mode == FindDeleteMode::kFind;
    auto opStr = isFind ? "StorageInterfaceImpl::find" : "StorageInterfaceImpl::delete";

    return writeConflictRetry(
        opCtx, opStr, nsOrUUID.toString(), [&]() -> StatusWith<std::vector<BSONObj>> {
            // We need to explicitly use this in a few places to help the type inference.  Use a
            // shorthand.
            using Result = StatusWith<std::vector<BSONObj>>;

            auto collectionAccessMode = isFind ? MODE_IS : MODE_IX;
            AutoGetCollection autoColl(opCtx, nsOrUUID, collectionAccessMode);
            auto collectionResult = getCollection(
                autoColl, nsOrUUID, str::stream() << "Unable to proceed with " << opStr << ".");
            if (!collectionResult.isOK()) {
                return Result(collectionResult.getStatus());
            }
            const auto& collection = *collectionResult.getValue();

            auto isForward = scanDirection == StorageInterface::ScanDirection::kForward;
            auto direction = isForward ? InternalPlanner::FORWARD : InternalPlanner::BACKWARD;

            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExecutor;
            if (!indexName) {
                if (!startKey.isEmpty()) {
                    return Result(ErrorCodes::NoSuchKey,
                                  "non-empty startKey not allowed for collection scan");
                }
                if (boundInclusion != BoundInclusion::kIncludeStartKeyOnly) {
                    return Result(
                        ErrorCodes::InvalidOptions,
                        "bound inclusion must be BoundInclusion::kIncludeStartKeyOnly for "
                        "collection scan");
                }
                // Use collection scan.
                planExecutor = isFind
                    ? InternalPlanner::collectionScan(
                          opCtx, &collection, PlanYieldPolicy::YieldPolicy::NO_YIELD, direction)
                    : InternalPlanner::deleteWithCollectionScan(
                          opCtx,
                          &collection,
                          makeDeleteStageParamsForDeleteDocuments(),
                          PlanYieldPolicy::YieldPolicy::NO_YIELD,
                          direction);
            } else if (*indexName == kIdIndexName && collection->isClustered() &&
                       collection->getClusteredInfo()
                               ->getIndexSpec()
                               .getKey()
                               .firstElement()
                               .fieldNameStringData() == "_id") {

                auto collScanBoundInclusion = [boundInclusion]() {
                    switch (boundInclusion) {
                        case BoundInclusion::kExcludeBothStartAndEndKeys:
                            return CollectionScanParams::ScanBoundInclusion::
                                kExcludeBothStartAndEndRecords;
                        case BoundInclusion::kIncludeStartKeyOnly:
                            return CollectionScanParams::ScanBoundInclusion::
                                kIncludeStartRecordOnly;
                        case BoundInclusion::kIncludeEndKeyOnly:
                            return CollectionScanParams::ScanBoundInclusion::kIncludeEndRecordOnly;
                        case BoundInclusion::kIncludeBothStartAndEndKeys:
                            return CollectionScanParams::ScanBoundInclusion::
                                kIncludeBothStartAndEndRecords;
                        default:
                            MONGO_UNREACHABLE;
                    }
                }();

                boost::optional<RecordIdBound> minRecord, maxRecord;
                if (direction == InternalPlanner::FORWARD) {
                    if (!startKey.isEmpty()) {
                        minRecord = RecordIdBound(record_id_helpers::keyForObj(startKey));
                    }
                    if (!endKey.isEmpty()) {
                        maxRecord = RecordIdBound(record_id_helpers::keyForObj(endKey));
                    }
                } else {
                    if (!startKey.isEmpty()) {
                        maxRecord = RecordIdBound(record_id_helpers::keyForObj(startKey));
                    }
                    if (!endKey.isEmpty()) {
                        minRecord = RecordIdBound(record_id_helpers::keyForObj(endKey));
                    }
                }

                planExecutor = isFind
                    ? InternalPlanner::collectionScan(opCtx,
                                                      &collection,
                                                      PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                                      direction,
                                                      boost::none /* resumeAfterId */,
                                                      minRecord,
                                                      maxRecord,
                                                      collScanBoundInclusion)
                    : InternalPlanner::deleteWithCollectionScan(
                          opCtx,
                          &collection,
                          makeDeleteStageParamsForDeleteDocuments(),
                          PlanYieldPolicy::YieldPolicy::NO_YIELD,
                          direction,
                          minRecord,
                          maxRecord,
                          collScanBoundInclusion);
            } else {
                // Use index scan.
                auto indexCatalog = collection->getIndexCatalog();
                invariant(indexCatalog);
                const IndexDescriptor* indexDescriptor = indexCatalog->findIndexByName(
                    opCtx, *indexName, IndexCatalog::InclusionPolicy::kReady);
                if (!indexDescriptor) {
                    return Result(ErrorCodes::IndexNotFound,
                                  str::stream() << "Index not found, ns:" << nsOrUUID.toString()
                                                << ", index: " << *indexName);
                }
                if (indexDescriptor->isPartial()) {
                    return Result(ErrorCodes::IndexOptionsConflict,
                                  str::stream()
                                      << "Partial index is not allowed for this operation, ns:"
                                      << nsOrUUID.toString() << ", index: " << *indexName);
                }

                KeyPattern keyPattern(indexDescriptor->keyPattern());
                auto minKey = Helpers::toKeyFormat(keyPattern.extendRangeBound({}, false));
                auto maxKey = Helpers::toKeyFormat(keyPattern.extendRangeBound({}, true));
                auto bounds =
                    isForward ? std::make_pair(minKey, maxKey) : std::make_pair(maxKey, minKey);
                if (!startKey.isEmpty()) {
                    bounds.first = startKey;
                }
                if (!endKey.isEmpty()) {
                    bounds.second = endKey;
                }
                planExecutor = isFind
                    ? InternalPlanner::indexScan(opCtx,
                                                 &collection,
                                                 indexDescriptor,
                                                 bounds.first,
                                                 bounds.second,
                                                 boundInclusion,
                                                 PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                                 direction,
                                                 InternalPlanner::IXSCAN_FETCH)
                    : InternalPlanner::deleteWithIndexScan(
                          opCtx,
                          &collection,
                          makeDeleteStageParamsForDeleteDocuments(),
                          indexDescriptor,
                          bounds.first,
                          bounds.second,
                          boundInclusion,
                          PlanYieldPolicy::YieldPolicy::NO_YIELD,
                          direction);
            }

            std::vector<BSONObj> docs;

            try {
                BSONObj out;
                PlanExecutor::ExecState state = PlanExecutor::ExecState::ADVANCED;
                while (state == PlanExecutor::ExecState::ADVANCED && docs.size() < limit) {
                    state = planExecutor->getNext(&out, nullptr);
                    if (state == PlanExecutor::ExecState::ADVANCED) {
                        docs.push_back(out.getOwned());
                    }
                }
            } catch (const WriteConflictException&) {
                // Re-throw the WCE, since it will get caught be a retry loop at a higher level.
                throw;
            } catch (const DBException&) {
                return exceptionToStatus();
            }

            return Result{docs};
        });
}

StatusWith<BSONObj> _findOrDeleteById(OperationContext* opCtx,
                                      const NamespaceStringOrUUID& nsOrUUID,
                                      const BSONElement& idKey,
                                      FindDeleteMode mode) {
    auto wrappedIdKey = idKey.wrap("");
    auto result = _findOrDeleteDocuments(opCtx,
                                         nsOrUUID,
                                         kIdIndexName,
                                         StorageInterface::ScanDirection::kForward,
                                         wrappedIdKey,
                                         wrappedIdKey,
                                         BoundInclusion::kIncludeBothStartAndEndKeys,
                                         1U,
                                         mode);
    if (!result.isOK()) {
        return result.getStatus();
    }
    const auto& docs = result.getValue();
    if (docs.empty()) {
        return {ErrorCodes::NoSuchKey,
                str::stream() << "No document found with _id: " << redact(idKey) << " in namespace "
                              << nsOrUUID.toString()};
    }

    return docs.front();
}

}  // namespace

StatusWith<std::vector<BSONObj>> StorageInterfaceImpl::findDocuments(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<StringData> indexName,
    ScanDirection scanDirection,
    const BSONObj& startKey,
    BoundInclusion boundInclusion,
    std::size_t limit) {
    return _findOrDeleteDocuments(opCtx,
                                  nss,
                                  indexName,
                                  scanDirection,
                                  startKey,
                                  {},
                                  boundInclusion,
                                  limit,
                                  FindDeleteMode::kFind);
}

StatusWith<std::vector<BSONObj>> StorageInterfaceImpl::deleteDocuments(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<StringData> indexName,
    ScanDirection scanDirection,
    const BSONObj& startKey,
    BoundInclusion boundInclusion,
    std::size_t limit) {
    return _findOrDeleteDocuments(opCtx,
                                  nss,
                                  indexName,
                                  scanDirection,
                                  startKey,
                                  {},
                                  boundInclusion,
                                  limit,
                                  FindDeleteMode::kDelete);
}

StatusWith<BSONObj> StorageInterfaceImpl::findSingleton(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    auto result = findDocuments(opCtx,
                                nss,
                                boost::none,  // Collection scan.
                                StorageInterface::ScanDirection::kForward,
                                {},  // Start at the beginning of the collection.
                                BoundInclusion::kIncludeStartKeyOnly,
                                2U);  // Ask for 2 documents to ensure it's a singleton.
    if (!result.isOK()) {
        return result.getStatus();
    }

    const auto& docs = result.getValue();
    if (docs.empty()) {
        return {ErrorCodes::CollectionIsEmpty,
                str::stream() << "No document found in namespace: " << nss.ns()};
    } else if (docs.size() != 1U) {
        return {ErrorCodes::TooManyMatchingDocuments,
                str::stream() << "More than singleton document found in namespace: " << nss.ns()};
    }

    return docs.front();
}

StatusWith<BSONObj> StorageInterfaceImpl::findById(OperationContext* opCtx,
                                                   const NamespaceStringOrUUID& nsOrUUID,
                                                   const BSONElement& idKey) {
    return _findOrDeleteById(opCtx, nsOrUUID, idKey, FindDeleteMode::kFind);
}

StatusWith<BSONObj> StorageInterfaceImpl::deleteById(OperationContext* opCtx,
                                                     const NamespaceStringOrUUID& nsOrUUID,
                                                     const BSONElement& idKey) {
    return _findOrDeleteById(opCtx, nsOrUUID, idKey, FindDeleteMode::kDelete);
}

namespace {

/**
 * Checks _id key passed to upsertById and returns a query document for UpdateRequest.
 */
StatusWith<BSONObj> makeUpsertQuery(const BSONElement& idKey) {
    auto query = BSON("_id" << idKey);

    // With the ID hack, only simple _id queries are allowed. Otherwise, UpdateStage will fail with
    // a fatal assertion.
    if (!CanonicalQuery::isSimpleIdQuery(query)) {
        return {ErrorCodes::InvalidIdField,
                str::stream() << "Unable to update document with a non-simple _id query: "
                              << query};
    }

    return query;
}

Status _updateWithQuery(OperationContext* opCtx,
                        const UpdateRequest& request,
                        const Timestamp& ts) {
    invariant(!request.isMulti());  // We only want to update one document for performance.
    invariant(!request.shouldReturnAnyDocs());
    invariant(PlanYieldPolicy::YieldPolicy::NO_YIELD == request.getYieldPolicy());

    auto& nss = request.getNamespaceString();
    return writeConflictRetry(opCtx, "_updateWithQuery", nss.ns(), [&] {
        // ParsedUpdate needs to be inside the write conflict retry loop because it may create a
        // CanonicalQuery whose ownership will be transferred to the plan executor in
        // getExecutorUpdate().
        const ExtensionsCallbackReal extensionsCallback(opCtx, &request.getNamespaceString());
        ParsedUpdate parsedUpdate(opCtx, &request, extensionsCallback);
        auto parsedUpdateStatus = parsedUpdate.parseRequest();
        if (!parsedUpdateStatus.isOK()) {
            return parsedUpdateStatus;
        }

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto collectionResult =
            getCollection(autoColl,
                          nss,
                          str::stream() << "Unable to update documents in " << nss.ns()
                                        << " using query " << request.getQuery());
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        const auto& collection = *collectionResult.getValue();
        WriteUnitOfWork wuow(opCtx);
        if (!ts.isNull()) {
            uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(ts));
            opCtx->recoveryUnit()->setOrderedCommit(false);
        }

        auto planExecutorResult = mongo::getExecutorUpdate(
            nullptr, &collection, &parsedUpdate, boost::none /* verbosity */);
        if (!planExecutorResult.isOK()) {
            return planExecutorResult.getStatus();
        }
        auto planExecutor = std::move(planExecutorResult.getValue());

        try {
            // The update result is ignored.
            [[maybe_unused]] auto updateResult = planExecutor->executeUpdate();
        } catch (const WriteConflictException&) {
            // Re-throw the WCE, since it will get caught and retried at a higher level.
            throw;
        } catch (const DBException&) {
            return exceptionToStatus();
        }
        wuow.commit();
        return Status::OK();
    });
}

}  // namespace

Status StorageInterfaceImpl::upsertById(OperationContext* opCtx,
                                        const NamespaceStringOrUUID& nsOrUUID,
                                        const BSONElement& idKey,
                                        const BSONObj& update) {
    // Validate and construct an _id query for UpdateResult.
    // The _id key will be passed directly to IDHackStage.
    auto queryResult = makeUpsertQuery(idKey);
    if (!queryResult.isOK()) {
        return queryResult.getStatus();
    }
    auto query = queryResult.getValue();

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::upsertById", nsOrUUID.toString(), [&] {
        AutoGetCollection autoColl(opCtx, nsOrUUID, MODE_IX);
        auto collectionResult = getCollection(autoColl, nsOrUUID, "Unable to update document.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        const auto& collection = *collectionResult.getValue();

        // We can create an UpdateRequest now that the collection's namespace has been resolved, in
        // the event it was specified as a UUID.
        auto request = UpdateRequest();
        request.setNamespaceString(collection->ns());
        request.setQuery(query);
        request.setUpdateModification(
            write_ops::UpdateModification::parseFromClassicUpdate(update));
        request.setUpsert(true);
        invariant(!request.isMulti());  // This follows from using an exact _id query.
        invariant(!request.shouldReturnAnyDocs());
        invariant(PlanYieldPolicy::YieldPolicy::NO_YIELD == request.getYieldPolicy());

        // ParsedUpdate needs to be inside the write conflict retry loop because it contains
        // the UpdateDriver whose state may be modified while we are applying the update.
        const ExtensionsCallbackReal extensionsCallback(opCtx, &request.getNamespaceString());
        ParsedUpdate parsedUpdate(opCtx, &request, extensionsCallback);
        auto parsedUpdateStatus = parsedUpdate.parseRequest();
        if (!parsedUpdateStatus.isOK()) {
            return parsedUpdateStatus;
        }

        // We're using the ID hack to perform the update so we have to disallow collections
        // without an _id index.
        auto descriptor = collection->getIndexCatalog()->findIdIndex(opCtx);
        if (!descriptor) {
            return Status(ErrorCodes::IndexNotFound,
                          "Unable to update document in a collection without an _id index.");
        }

        UpdateStageParams updateStageParams(
            parsedUpdate.getRequest(), parsedUpdate.getDriver(), nullptr);
        auto planExecutor = InternalPlanner::updateWithIdHack(opCtx,
                                                              &collection,
                                                              updateStageParams,
                                                              descriptor,
                                                              idKey.wrap(""),
                                                              parsedUpdate.yieldPolicy());

        try {
            // The update result is ignored.
            [[maybe_unused]] auto updateResult = planExecutor->executeUpdate();
        } catch (const WriteConflictException&) {
            // Re-throw the WCE, since it will get caught and retried at a higher level.
            throw;
        } catch (const DBException&) {
            return exceptionToStatus();
        }
        return Status::OK();
    });
}

Status StorageInterfaceImpl::putSingleton(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const TimestampedBSONObj& update) {
    auto request = UpdateRequest();
    request.setNamespaceString(nss);
    request.setQuery({});
    request.setUpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(update.obj));
    request.setUpsert(true);
    return _updateWithQuery(opCtx, request, update.timestamp);
}

Status StorageInterfaceImpl::updateSingleton(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const BSONObj& query,
                                             const TimestampedBSONObj& update) {
    auto request = UpdateRequest();
    request.setNamespaceString(nss);
    request.setQuery(query);
    request.setUpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(update.obj));
    invariant(!request.isUpsert());
    return _updateWithQuery(opCtx, request, update.timestamp);
}

Status StorageInterfaceImpl::deleteByFilter(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const BSONObj& filter) {
    auto request = DeleteRequest{};
    request.setNsString(nss);
    request.setQuery(filter);
    request.setMulti(true);
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::NO_YIELD);

    // This disables the isLegalClientSystemNS() check in getExecutorDelete() which is used to
    // disallow client deletes from unrecognized system collections.
    request.setGod(true);

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::deleteByFilter", nss.ns(), [&] {
        // ParsedDelete needs to be inside the write conflict retry loop because it may create a
        // CanonicalQuery whose ownership will be transferred to the plan executor in
        // getExecutorDelete().
        ParsedDelete parsedDelete(opCtx, &request);
        auto parsedDeleteStatus = parsedDelete.parseRequest();
        if (!parsedDeleteStatus.isOK()) {
            return parsedDeleteStatus;
        }

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto collectionResult =
            getCollection(autoColl,
                          nss,
                          str::stream() << "Unable to delete documents in " << nss.ns()
                                        << " using filter " << filter);
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        const auto& collection = *collectionResult.getValue();

        auto planExecutorResult = mongo::getExecutorDelete(
            nullptr, &collection, &parsedDelete, boost::none /* verbosity */);
        if (!planExecutorResult.isOK()) {
            return planExecutorResult.getStatus();
        }
        auto planExecutor = std::move(planExecutorResult.getValue());

        try {
            // The count of deleted documents is ignored.
            [[maybe_unused]] auto nDeleted = planExecutor->executeDelete();
        } catch (const WriteConflictException&) {
            // Re-throw the WCE, since it will get caught and retried at a higher level.
            throw;
        } catch (const DBException&) {
            return exceptionToStatus();
        }
        return Status::OK();
    });
}

boost::optional<BSONObj> StorageInterfaceImpl::findOplogEntryLessThanOrEqualToTimestamp(
    OperationContext* opCtx, const CollectionPtr& oplog, const Timestamp& timestamp) {
    invariant(oplog);
    invariant(opCtx->lockState()->isLocked());

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec = InternalPlanner::collectionScan(
        opCtx, &oplog, PlanYieldPolicy::YieldPolicy::NO_YIELD, InternalPlanner::BACKWARD);

    // A record id in the oplog collection is equivalent to the document's timestamp field.
    RecordId desiredRecordId = RecordId(timestamp.asULL());

    // Iterate the collection in reverse until the desiredRecordId, or one less than, is found.
    BSONObj bson;
    RecordId recordId;
    PlanExecutor::ExecState state;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&bson, &recordId))) {
        if (recordId <= desiredRecordId) {
            invariant(!bson.isEmpty(),
                      "An empty oplog entry was returned while searching for an oplog entry <= " +
                          timestamp.toString());
            return bson.getOwned();
        }
    }

    return boost::none;
}

boost::optional<BSONObj> StorageInterfaceImpl::findOplogEntryLessThanOrEqualToTimestampRetryOnWCE(
    OperationContext* opCtx, const CollectionPtr& oplogCollection, const Timestamp& timestamp) {
    // Oplog reads are specially done under only MODE_IS global locks, without database or
    // collection level intent locks. Therefore, reads can run concurrently with validate cmds that
    // take collection MODE_X locks. Validate with {full:true} set calls WT::verify on the
    // collection, which causes concurrent readers to hit WT EBUSY errors that MongoDB converts
    // into WriteConflictException errors.
    //
    // Consequently, this code must be resilient to WCE errors and retry until the validate cmd
    // finishes. The greater operation using this helper cannot simply fail because it would cause
    // correctness errors.

    int retries = 0;
    while (true) {
        try {
            return findOplogEntryLessThanOrEqualToTimestamp(opCtx, oplogCollection, timestamp);
        } catch (const WriteConflictException&) {
            // This will log a message about the conflict initially and then every 5 seconds, with
            // the current rather arbitrary settings.
            if (retries % 10 == 0) {
                LOGV2(4795900,
                      "Reading the oplog collection conflicts with a validate cmd. Continuing to "
                      "retry.",
                      "retries"_attr = retries);
            }

            ++retries;

            // Sleep a bit so we do not keep hammering the system with retries while the validate
            // cmd finishes.
            opCtx->sleepFor(Milliseconds(500));
        }
    }
}

Timestamp StorageInterfaceImpl::getEarliestOplogTimestamp(OperationContext* opCtx) {
    auto statusWithTimestamp = [&]() {
        AutoGetOplog oplogRead(opCtx, OplogAccessMode::kRead);
        return oplogRead.getCollection()->getRecordStore()->getEarliestOplogTimestamp(opCtx);
    }();

    // If the storage engine does not support getEarliestOplogTimestamp(), then fall back to higher
    // level (above the storage engine) logic to fetch the earliest oplog entry timestamp.
    if (statusWithTimestamp.getStatus() == ErrorCodes::OplogOperationUnsupported) {
        // Reset the snapshot so that it is ensured to see the latest oplog entries.
        opCtx->recoveryUnit()->abandonSnapshot();

        BSONObj oplogEntryBSON;
        tassert(5869100,
                "Failed reading the earliest oplog entry",
                Helpers::getSingleton(opCtx, NamespaceString::kRsOplogNamespace, oplogEntryBSON));

        auto optime = OpTime::parseFromOplogEntry(oplogEntryBSON);
        tassert(5869101,
                str::stream() << "Found an invalid oplog entry: " << oplogEntryBSON
                              << ", error: " << optime.getStatus(),
                optime.isOK());
        return optime.getValue().getTimestamp();
    }

    tassert(5869102,
            str::stream() << "Expected oplog entries to exist: " << statusWithTimestamp.getStatus(),
            statusWithTimestamp.isOK());

    return statusWithTimestamp.getValue();
}

Timestamp StorageInterfaceImpl::getLatestOplogTimestamp(OperationContext* opCtx) {
    auto statusWithTimestamp = [&]() {
        AutoGetOplog oplogRead(opCtx, OplogAccessMode::kRead);
        return oplogRead.getCollection()->getRecordStore()->getLatestOplogTimestamp(opCtx);
    }();

    // If the storage engine does not support getLatestOplogTimestamp, then fall back to higher
    // level (above the storage engine) logic to fetch the latest oplog entry timestamp.
    if (statusWithTimestamp.getStatus() == ErrorCodes::OplogOperationUnsupported) {
        // Reset the snapshot so that it is ensured to see the latest oplog entries.
        opCtx->recoveryUnit()->abandonSnapshot();

        // Helpers::getLast will bypass the oplog visibility rules by doing a backwards collection
        // scan.
        BSONObj oplogEntryBSON;
        invariant(Helpers::getLast(opCtx, NamespaceString::kRsOplogNamespace, oplogEntryBSON));

        auto optime = OpTime::parseFromOplogEntry(oplogEntryBSON);
        invariant(optime.isOK(),
                  str::stream() << "Found an invalid oplog entry: " << oplogEntryBSON
                                << ", error: " << optime.getStatus());
        return optime.getValue().getTimestamp();
    }

    invariant(statusWithTimestamp.isOK(),
              str::stream() << "Expected oplog entries to exist: "
                            << statusWithTimestamp.getStatus());

    return statusWithTimestamp.getValue();
}

StatusWith<StorageInterface::CollectionSize> StorageInterfaceImpl::getCollectionSize(
    OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead autoColl(opCtx, nss);

    auto collectionResult =
        getCollection(autoColl, nss, "Unable to get total size of documents in collection.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    const auto& collection = *collectionResult.getValue();

    return collection->dataSize(opCtx);
}

StatusWith<StorageInterface::CollectionCount> StorageInterfaceImpl::getCollectionCount(
    OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) {
    AutoGetCollectionForRead autoColl(opCtx, nsOrUUID);

    auto collectionResult =
        getCollection(autoColl, nsOrUUID, "Unable to get number of documents in collection.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    const auto& collection = *collectionResult.getValue();

    return collection->numRecords(opCtx);
}

Status StorageInterfaceImpl::setCollectionCount(OperationContext* opCtx,
                                                const NamespaceStringOrUUID& nsOrUUID,
                                                long long newCount) {
    AutoGetCollection autoColl(opCtx, nsOrUUID, LockMode::MODE_X);

    auto collectionResult =
        getCollection(autoColl, nsOrUUID, "Unable to set number of documents in collection.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    const auto& collection = *collectionResult.getValue();

    auto rs = collection->getRecordStore();
    // We cannot fix the data size correctly, so we just get the current cached value and keep it
    // the same.
    long long dataSize = rs->dataSize(opCtx);
    rs->updateStatsAfterRepair(opCtx, newCount, dataSize);
    return Status::OK();
}

StatusWith<UUID> StorageInterfaceImpl::getCollectionUUID(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    AutoGetCollectionForRead autoColl(opCtx, nss);

    auto collectionResult = getCollection(
        autoColl, nss, str::stream() << "Unable to get UUID of " << nss.ns() << " collection.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    const auto& collection = *collectionResult.getValue();
    return collection->uuid();
}

void StorageInterfaceImpl::setStableTimestamp(ServiceContext* serviceCtx,
                                              Timestamp snapshotName,
                                              bool force) {
    auto newStableTimestamp = snapshotName;
    // Hold the stable timestamp back if this failpoint is enabled.
    holdStableTimestampAtSpecificTimestamp.execute([&](const BSONObj& dataObj) {
        const auto holdStableTimestamp = dataObj["timestamp"].timestamp();
        if (newStableTimestamp > holdStableTimestamp) {
            newStableTimestamp = holdStableTimestamp;
            LOGV2(4784410,
                  "holdStableTimestampAtSpecificTimestamp holding the stable timestamp",
                  "holdStableTimestamp"_attr = holdStableTimestamp);
        }
    });

    StorageEngine* storageEngine = serviceCtx->getStorageEngine();
    Timestamp prevStableTimestamp = storageEngine->getStableTimestamp();

    storageEngine->setStableTimestamp(newStableTimestamp, force);

    Checkpointer* checkpointer = Checkpointer::get(serviceCtx);
    if (checkpointer && !checkpointer->hasTriggeredFirstStableCheckpoint()) {
        checkpointer->triggerFirstStableCheckpoint(prevStableTimestamp,
                                                   storageEngine->getInitialDataTimestamp(),
                                                   storageEngine->getStableTimestamp());
    }
}

void StorageInterfaceImpl::setInitialDataTimestamp(ServiceContext* serviceCtx,
                                                   Timestamp snapshotName) {
    serviceCtx->getStorageEngine()->setInitialDataTimestamp(snapshotName);
}

Timestamp StorageInterfaceImpl::recoverToStableTimestamp(OperationContext* opCtx) {
    auto serviceContext = opCtx->getServiceContext();

    // Pass an InterruptedDueToReplStateChange error to async callers waiting on the JournalFlusher
    // thread for durability.
    Status reason = Status(ErrorCodes::InterruptedDueToReplStateChange, "Rollback in progress.");
    StorageControl::stopStorageControls(serviceContext, reason, /*forRestart=*/true);

    auto swStableTimestamp = serviceContext->getStorageEngine()->recoverToStableTimestamp(opCtx);
    if (!swStableTimestamp.isOK()) {
        // Dump storage engine contents (including transaction information) before fatally
        // asserting.
        serviceContext->getStorageEngine()->dump();
    }
    fassert(31049, swStableTimestamp);

    StorageControl::startStorageControls(serviceContext);

    return swStableTimestamp.getValue();
}

bool StorageInterfaceImpl::supportsRecoverToStableTimestamp(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->supportsRecoverToStableTimestamp();
}

bool StorageInterfaceImpl::supportsRecoveryTimestamp(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->supportsRecoveryTimestamp();
}

void StorageInterfaceImpl::initializeStorageControlsForReplication(
    ServiceContext* serviceCtx) const {
    // The storage engine may support the use of OplogStones to more finely control
    // oplog history deletion, in which case we need to start the thread to
    // periodically execute deletion via oplog stones. OplogStones are a replacement
    // for capped collection deletion of the oplog collection history.
    if (serviceCtx->getStorageEngine()->supportsOplogStones()) {
        BackgroundJob* backgroundThread = new OplogCapMaintainerThread();
        backgroundThread->go();
    }
}

boost::optional<Timestamp> StorageInterfaceImpl::getRecoveryTimestamp(
    ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->getRecoveryTimestamp();
}

Status StorageInterfaceImpl::isAdminDbValid(OperationContext* opCtx) {
    AutoGetDb autoDB(opCtx, DatabaseName(boost::none, "admin"), MODE_X);
    auto adminDb = autoDB.getDb();
    if (!adminDb) {
        return Status::OK();
    }

    auto catalog = CollectionCatalog::get(opCtx);
    CollectionPtr usersCollection =
        catalog->lookupCollectionByNamespace(opCtx, AuthorizationManager::usersCollectionNamespace);
    const bool hasUsers =
        usersCollection && !Helpers::findOne(opCtx, usersCollection, BSONObj()).isNull();
    CollectionPtr adminVersionCollection = catalog->lookupCollectionByNamespace(
        opCtx, AuthorizationManager::versionCollectionNamespace);
    BSONObj authSchemaVersionDocument;
    if (!adminVersionCollection ||
        !Helpers::findOne(opCtx,
                          adminVersionCollection,
                          AuthorizationManager::versionDocumentQuery,
                          authSchemaVersionDocument)) {
        if (!hasUsers) {
            // It's OK to have no auth version document if there are no user documents.
            return Status::OK();
        }
        std::string msg = str::stream()
            << "During initial sync, found documents in "
            << AuthorizationManager::usersCollectionNamespace.ns()
            << " but could not find an auth schema version document in "
            << AuthorizationManager::versionCollectionNamespace.ns() << ".  "
            << "This indicates that the primary of this replica set was not successfully "
               "upgraded to schema version "
            << AuthorizationManager::schemaVersion26Final
            << ", which is the minimum supported schema version in this version of MongoDB";
        return {ErrorCodes::AuthSchemaIncompatible, msg};
    }
    long long foundSchemaVersion;
    Status status = bsonExtractIntegerField(authSchemaVersionDocument,
                                            AuthorizationManager::schemaVersionFieldName,
                                            &foundSchemaVersion);
    if (!status.isOK()) {
        std::string msg = str::stream()
            << "During initial sync, found malformed auth schema version document: "
            << status.toString() << "; document: " << authSchemaVersionDocument;
        return {ErrorCodes::AuthSchemaIncompatible, msg};
    }
    if ((foundSchemaVersion != AuthorizationManager::schemaVersion26Final) &&
        (foundSchemaVersion != AuthorizationManager::schemaVersion28SCRAM)) {
        std::string msg = str::stream()
            << "During initial sync, found auth schema version " << foundSchemaVersion
            << ", but this version of MongoDB only supports schema versions "
            << AuthorizationManager::schemaVersion26Final << " and "
            << AuthorizationManager::schemaVersion28SCRAM;
        return {ErrorCodes::AuthSchemaIncompatible, msg};
    }

    return Status::OK();
}

void StorageInterfaceImpl::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                                   bool primaryOnly) {
    // Waiting for oplog writes to be visible in the oplog does not use any storage engine resources
    // and must skip ticket acquisition to avoid deadlocks with updating oplog visibility.
    SkipTicketAcquisitionForLock skipTicketAcquisition(opCtx);

    AutoGetOplog oplogRead(opCtx, OplogAccessMode::kRead);
    if (primaryOnly &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx, "admin"))
        return;
    const auto& oplog = oplogRead.getCollection();
    uassert(ErrorCodes::NotYetInitialized, "The oplog does not exist", oplog);
    oplog->getRecordStore()->waitForAllEarlierOplogWritesToBeVisible(opCtx);
}

void StorageInterfaceImpl::oplogDiskLocRegister(OperationContext* opCtx,
                                                const Timestamp& ts,
                                                bool orderedCommit) {
    // Setting the oplog visibility does not use any storage engine resources and must skip ticket
    // acquisition to avoid deadlocks with updating oplog visibility.
    SkipTicketAcquisitionForLock skipTicketAcquisition(opCtx);

    AutoGetOplog oplogRead(opCtx, OplogAccessMode::kRead);
    fassert(28557,
            oplogRead.getCollection()->getRecordStore()->oplogDiskLocRegister(
                opCtx, ts, orderedCommit));
}

boost::optional<Timestamp> StorageInterfaceImpl::getLastStableRecoveryTimestamp(
    ServiceContext* serviceCtx) const {
    if (!supportsRecoverToStableTimestamp(serviceCtx)) {
        return boost::none;
    }

    const auto ret = serviceCtx->getStorageEngine()->getLastStableRecoveryTimestamp();
    if (ret == boost::none) {
        return Timestamp::min();
    }

    return ret;
}

Timestamp StorageInterfaceImpl::getAllDurableTimestamp(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->getAllDurableTimestamp();
}

Timestamp StorageInterfaceImpl::getPointInTimeReadTimestamp(OperationContext* opCtx) const {
    auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
    invariant(readTimestamp);
    return *readTimestamp;
}

void StorageInterfaceImpl::setPinnedOplogTimestamp(OperationContext* opCtx,
                                                   const Timestamp& pinnedTimestamp) const {
    opCtx->getServiceContext()->getStorageEngine()->setPinnedOplogTimestamp(pinnedTimestamp);
}

}  // namespace repl
}  // namespace mongo
