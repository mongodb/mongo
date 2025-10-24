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

#include "mongo/db/repl/storage_interface_impl.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <limits>
#include <mutex>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/collection_bulk_loader_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/rollback_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/checkpointer.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/oplog_cap_maintainer_thread.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(holdStableTimestampAtSpecificTimestamp);

const char StorageInterfaceImpl::kRollbackIdFieldName[] = "rollbackId";
const char StorageInterfaceImpl::kRollbackIdDocumentId[] = "rollbackId";

namespace {
using UniqueLock = stdx::unique_lock<Latch>;

const auto kIdIndexName = "_id_"_sd;

}  // namespace

StorageInterfaceImpl::StorageInterfaceImpl()
    : _rollbackIdNss(NamespaceString::kDefaultRollbackIdNamespace) {}

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

    LOGV2_DEBUG(
        21753, 2, "StorageInterfaceImpl::createCollectionForBulkLoading called", logAttrs(nss));

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
    Client::setCurrent(getGlobalServiceContext()
                           ->getService(ClusterRole::ShardServer)
                           ->makeClient(str::stream()
                                        << NamespaceStringUtil::serialize(
                                               nss, SerializationContext::stateDefault())
                                        << " loader"));
    auto opCtx = cc().makeOperationContext();
    opCtx->setEnforceConstraints(false);

    // This thread is killable since it is only used by initial sync which does not
    // interact with repl state changes.

    // DocumentValidationSettings::kDisableInternalValidation is currently inert.
    // But, it's logically ok to disable internal validation as this function gets called
    // only during initial sync.
    DocumentValidationSettings::get(opCtx.get())
        .setFlags(DocumentValidationSettings::kDisableSchemaValidation |
                  DocumentValidationSettings::kDisableInternalValidation);

    std::unique_ptr<CollectionBulkLoader> loader;
    // Retry if WCE.
    Status status = writeConflictRetry(opCtx.get(), "beginCollectionClone", nss, [&] {
        UnreplicatedWritesBlock uwb(opCtx.get());

        // Get locks and create the collection.
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_IX);
        AutoGetCollection coll(opCtx.get(), nss, MODE_X);
        if (coll) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream()
                              << "Collection " << nss.toStringForErrorMsg() << " already exists.");
        }
        {
            // Create the collection.
            WriteUnitOfWork wunit(opCtx.get());
            auto db = autoDb.ensureDbExists(opCtx.get());
            fassert(40332, db->createCollection(opCtx.get(), nss, options, false));
            wunit.commit();
        }

        // Build empty capped indexes.  Capped indexes cannot be built by the MultiIndexBlock
        // because the cap might delete documents off the back while we are inserting them into
        // the front.
        if (options.capped) {
            WriteUnitOfWork wunit(opCtx.get());
            if (!idIndexSpec.isEmpty()) {
                auto status =
                    coll.getWritableCollection(opCtx.get())
                        ->getIndexCatalog()
                        ->createIndexOnEmptyCollection(
                            opCtx.get(), coll.getWritableCollection(opCtx.get()), idIndexSpec);
                if (!status.getStatus().isOK()) {
                    return status.getStatus();
                }
            }
            for (auto&& spec : secondaryIndexSpecs) {
                auto status = coll.getWritableCollection(opCtx.get())
                                  ->getIndexCatalog()
                                  ->createIndexOnEmptyCollection(
                                      opCtx.get(), coll.getWritableCollection(opCtx.get()), spec);
                if (!status.getStatus().isOK()) {
                    return status.getStatus();
                }
            }
            wunit.commit();
        }

        // Instantiate the CollectionBulkLoader here so that it acquires the same MODE_X lock we've
        // used in this scope. The BulkLoader will manage an AutoGet of its own to control the
        // lifetime of the lock. This is safe to do as we're in the initial sync phase and the node
        // isn't yet available to users.
        loader =
            std::make_unique<CollectionBulkLoaderImpl>(Client::releaseCurrent(),
                                                       std::move(opCtx),
                                                       nss,
                                                       options.capped ? BSONObj() : idIndexSpec);
        return Status::OK();
    });

    if (!status.isOK()) {
        return status;
    }

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
                str::stream() << "Collection [" << nsOrUUID.toStringForErrorMsg() << "] not found. "
                              << message};
    }

    return &collection;
}

Status insertDocumentsSingleBatch(OperationContext* opCtx,
                                  const NamespaceStringOrUUID& nsOrUUID,
                                  std::vector<InsertStatement>::const_iterator begin,
                                  std::vector<InsertStatement>::const_iterator end) {
    boost::optional<AutoGetCollection> autoColl;
    boost::optional<AutoGetOplogFastPath> autoOplog;
    const CollectionPtr* collection;

    if (nsOrUUID.isNamespaceString() && nsOrUUID.nss().isOplog()) {
        // Simplify locking rules for oplog collection.
        autoOplog.emplace(opCtx, OplogAccessMode::kWrite);
        collection = &autoOplog->getCollection();
        if (!*collection) {
            return {ErrorCodes::NamespaceNotFound, "Oplog collection does not exist"};
        }
    } else {
        autoColl.emplace(opCtx, nsOrUUID, MODE_IX);
        auto collectionResult = getCollection(
            autoColl.value(), nsOrUUID, "The collection must exist before inserting documents.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        collection = collectionResult.getValue();
    }

    WriteUnitOfWork wunit(opCtx);
    OpDebug* const nullOpDebug = nullptr;
    auto status =
        collection_internal::insertDocuments(opCtx, *collection, begin, end, nullOpDebug, false);
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
          "dropReplicatedDatabases - dropping databases",
          "numDatabases"_attr = dbNames.size());

    ReplicationCoordinator::get(opCtx)->clearCommittedSnapshot();

    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto hasLocalDatabase = false;
    for (const auto& dbName : dbNames) {
        if (dbName.isLocalDB()) {
            hasLocalDatabase = true;
            continue;
        }
        writeConflictRetry(opCtx, "dropReplicatedDatabases", NamespaceString(dbName), [&] {
            if (auto db = databaseHolder->getDb(opCtx, dbName)) {
                WriteUnitOfWork wuow(opCtx);
                databaseHolder->dropDb(opCtx, db);
                wuow.commit();
            } else {
                // This is needed since dropDatabase can't be rolled back.
                // This is safe be replaced by "invariant(db);dropDatabase(opCtx, db);" once
                // fixed.
                LOGV2(21755,
                      "dropReplicatedDatabases - database disappeared after retrieving list of "
                      "database names but before drop",
                      "dbName"_attr = dbName);
            }
        });
    }
    invariant(hasLocalDatabase, "local database missing");
    LOGV2(
        21756, "dropReplicatedDatabases - dropped databases", "numDatabases"_attr = dbNames.size());

    return Status::OK();
}

Status StorageInterfaceImpl::createOplog(OperationContext* opCtx, const NamespaceString& nss) {
    mongo::repl::createOplog(opCtx, nss, true);
    return Status::OK();
}

StatusWith<size_t> StorageInterfaceImpl::getOplogMaxSize(OperationContext* opCtx) {
    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    const auto& oplog = oplogRead.getCollection();
    if (!oplog) {
        return {ErrorCodes::NamespaceNotFound, "Your oplog doesn't exist."};
    }
    const auto options = oplog->getCollectionOptions();
    if (!options.capped)
        return {ErrorCodes::BadValue,
                str::stream() << NamespaceString::kRsOplogNamespace.toStringForErrorMsg()
                              << " isn't capped"};
    return options.cappedSize;
}

Status StorageInterfaceImpl::createCollection(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const CollectionOptions& options,
                                              const bool createIdIndex,
                                              const BSONObj& idIndexSpec) {
    try {
        return writeConflictRetry(opCtx, "StorageInterfaceImpl::createCollection", nss, [&] {
            AutoGetDb databaseWriteGuard(opCtx, nss.dbName(), MODE_IX);
            auto db = databaseWriteGuard.ensureDbExists(opCtx);
            invariant(db);

            // Check if there already exist a Collection/view on the given namespace 'nss'. The
            // answer may change at any point after this call as we make this call without holding
            // the collection lock. But, it is fine as we properly handle while registering the
            // uncommitted collection with CollectionCatalog. This check is just here to prevent it
            // from being created in the common case.
            Status status = mongo::catalog::checkIfNamespaceExists(opCtx, nss);
            if (!status.isOK()) {
                return status;
            }

            Lock::CollectionLock lk(opCtx, nss, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            auto coll = db->createCollection(opCtx, nss, options, createIdIndex, idIndexSpec);
            invariant(coll);

            // This commit call can throw if a view already exists while registering the
            // collection.
            wuow.commit();
            return Status::OK();
        });
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status StorageInterfaceImpl::createIndexesOnEmptyCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<BSONObj>& secondaryIndexSpecs) {
    if (!secondaryIndexSpecs.size())
        return Status::OK();

    try {
        writeConflictRetry(opCtx, "StorageInterfaceImpl::createIndexesOnEmptyCollection", nss, [&] {
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
    try {
        return writeConflictRetry(opCtx, "StorageInterfaceImpl::dropCollection", nss, [&] {
            AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
            Lock::CollectionLock collLock(opCtx, nss, MODE_X);
            if (!autoDb.getDb()) {
                // Database does not exist - nothing to do.
                return Status::OK();
            }
            WriteUnitOfWork wunit(opCtx);
            auto status = autoDb.getDb()->dropCollectionEvenIfSystem(opCtx, nss);
            if (!status.isOK()) {
                return status;
            }
            wunit.commit();
            return Status::OK();
        });
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status StorageInterfaceImpl::dropCollectionsWithPrefix(OperationContext* opCtx,
                                                       const DatabaseName& dbName,
                                                       const std::string& collectionNamePrefix) {
    return writeConflictRetry(
        opCtx,
        "StorageInterfaceImpl::dropCollectionsWithPrefix",
        NamespaceString::createNamespaceString_forTest(dbName, collectionNamePrefix),
        [&] {
            AutoGetDb autoDB(opCtx, dbName, MODE_X);
            StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
            return storageEngine->dropCollectionsWithPrefix(opCtx, dbName, collectionNamePrefix);
        });
}

Status StorageInterfaceImpl::truncateCollection(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    return writeConflictRetry(opCtx, "StorageInterfaceImpl::truncateCollection", nss, [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        auto collectionResult =
            getCollection(autoColl, nss, "The collection must exist before truncating.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }

        WriteUnitOfWork wunit(opCtx);
        auto status = autoColl.getWritableCollection(opCtx)->truncate(opCtx);
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
    if (!fromNS.isEqualDb(toNS)) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Cannot rename collection between databases. From NS: "
                                    << fromNS.toStringForErrorMsg()
                                    << "; to NS: " << toNS.toStringForErrorMsg());
    }

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::renameCollection", fromNS, [&] {
        AutoGetDb autoDB(opCtx, fromNS.dbName(), MODE_X);
        if (!autoDB.getDb()) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream()
                              << "Cannot rename collection from " << fromNS.toStringForErrorMsg()
                              << " to " << toNS.toStringForErrorMsg() << ". Database "
                              << fromNS.dbName().toStringForErrorMsg() << " not found.");
        }
        WriteUnitOfWork wunit(opCtx);
        auto status = autoDB.getDb()->renameCollection(opCtx, fromNS, toNS, stayTemp);
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
                      str::stream()
                          << "Cannot set index " << indexName << " on " << nss.toStringForErrorMsg()
                          << " (" << collectionUUID << ") as multikey at null timestamp");
    }

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::setIndexIsMultikey", nss, [&] {
        const NamespaceStringOrUUID nsOrUUID(nss.dbName(), collectionUUID);
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
        auto tsResult = shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(ts);
        if (!tsResult.isOK()) {
            return tsResult;
        }

        auto idx = collection->getIndexCatalog()->findIndexByName(
            opCtx,
            indexName,
            IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);
        if (!idx) {
            return Status(ErrorCodes::IndexNotFound,
                          str::stream() << "Could not find index " << indexName << " in "
                                        << nss.toStringForErrorMsg() << " (" << collectionUUID
                                        << ") to set to multikey.");
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

    return writeConflictRetry(opCtx, opStr, nsOrUUID, [&]() -> StatusWith<std::vector<BSONObj>> {
        // We need to explicitly use this in a few places to help the type inference.  Use a
        // shorthand.
        using Result = StatusWith<std::vector<BSONObj>>;

        auto collectionAccessMode = isFind ? MODE_IS : MODE_IX;
        const auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx,
                                                    nsOrUUID,
                                                    isFind ? AcquisitionPrerequisites::kRead
                                                           : AcquisitionPrerequisites::kWrite),
            collectionAccessMode);
        if (!collection.exists()) {
            return Status{ErrorCodes::NamespaceNotFound,
                          str::stream()
                              << "Collection [" << nsOrUUID.toStringForErrorMsg() << "] not found. "
                              << "Unable to proceed with " << opStr << "."};
        }

        auto isForward = scanDirection == StorageInterface::ScanDirection::kForward;
        auto direction = isForward ? InternalPlanner::FORWARD : InternalPlanner::BACKWARD;

        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExecutor;
        if (!indexName) {
            if (!startKey.isEmpty()) {
                return Result(ErrorCodes::NoSuchKey,
                              "non-empty startKey not allowed for collection scan");
            }
            if (boundInclusion != BoundInclusion::kIncludeStartKeyOnly) {
                return Result(ErrorCodes::InvalidOptions,
                              "bound inclusion must be BoundInclusion::kIncludeStartKeyOnly for "
                              "collection scan");
            }
            // Use collection scan.
            planExecutor = isFind
                ? InternalPlanner::collectionScan(opCtx,
                                                  &collection.getCollectionPtr(),
                                                  PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                  direction)
                : InternalPlanner::deleteWithCollectionScan(
                      opCtx,
                      collection,
                      makeDeleteStageParamsForDeleteDocuments(),
                      PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                      direction);
        } else if (*indexName == kIdIndexName && collection.getCollectionPtr()->isClustered() &&
                   collection.getCollectionPtr()
                           ->getClusteredInfo()
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
                        return CollectionScanParams::ScanBoundInclusion::kIncludeStartRecordOnly;
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
                                                  &collection.getCollectionPtr(),
                                                  PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                  direction,
                                                  boost::none /* resumeAfterId */,
                                                  minRecord,
                                                  maxRecord,
                                                  collScanBoundInclusion)
                : InternalPlanner::deleteWithCollectionScan(
                      opCtx,
                      collection,
                      makeDeleteStageParamsForDeleteDocuments(),
                      PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                      direction,
                      minRecord,
                      maxRecord,
                      collScanBoundInclusion);
        } else {
            // Use index scan.
            auto indexCatalog = collection.getCollectionPtr()->getIndexCatalog();
            invariant(indexCatalog);
            const IndexDescriptor* indexDescriptor = indexCatalog->findIndexByName(
                opCtx, *indexName, IndexCatalog::InclusionPolicy::kReady);
            if (!indexDescriptor) {
                return Result(ErrorCodes::IndexNotFound,
                              str::stream()
                                  << "Index not found, ns:" << nsOrUUID.toStringForErrorMsg()
                                  << ", index: " << *indexName);
            }
            if (indexDescriptor->isPartial()) {
                return Result(ErrorCodes::IndexOptionsConflict,
                              str::stream()
                                  << "Partial index is not allowed for this operation, ns:"
                                  << nsOrUUID.toStringForErrorMsg() << ", index: " << *indexName);
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
                                             &collection.getCollectionPtr(),
                                             indexDescriptor,
                                             bounds.first,
                                             bounds.second,
                                             boundInclusion,
                                             PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                             direction,
                                             InternalPlanner::IXSCAN_FETCH)
                : InternalPlanner::deleteWithIndexScan(opCtx,
                                                       collection,
                                                       makeDeleteStageParamsForDeleteDocuments(),
                                                       indexDescriptor,
                                                       bounds.first,
                                                       bounds.second,
                                                       boundInclusion,
                                                       PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
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
        } catch (const StorageUnavailableException&) {
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
                              << nsOrUUID.toStringForErrorMsg()};
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
                str::stream() << "No document found in namespace: " << nss.toStringForErrorMsg()};
    } else if (docs.size() != 1U) {
        return {ErrorCodes::TooManyMatchingDocuments,
                str::stream() << "More than singleton document found in namespace: "
                              << nss.toStringForErrorMsg()};
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
    invariant(!request.shouldReturnAnyDocs());
    invariant(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY == request.getYieldPolicy());

    auto& nss = request.getNamespaceString();
    return writeConflictRetry(opCtx, "_updateWithQuery", nss, [&] {
        const auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        if (!collection.exists()) {
            return Status{ErrorCodes::NamespaceNotFound,
                          str::stream()
                              << "Collection [" << nss.toStringForErrorMsg() << "] not found. "
                              << "Unable to update documents in " << nss.toStringForErrorMsg()
                              << " using query " << request.getQuery()};
        }

        // ParsedUpdate needs to be inside the write conflict retry loop because it may create a
        // CanonicalQuery whose ownership will be transferred to the plan executor in
        // getExecutorUpdate().
        ParsedUpdate parsedUpdate(opCtx, &request, collection.getCollectionPtr());
        auto parsedUpdateStatus = parsedUpdate.parseRequest();
        if (!parsedUpdateStatus.isOK()) {
            return parsedUpdateStatus;
        }

        WriteUnitOfWork wuow(opCtx);
        if (!ts.isNull()) {
            uassertStatusOK(shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(ts));
            shard_role_details::getRecoveryUnit(opCtx)->setOrderedCommit(false);
        }

        auto planExecutorResult = mongo::getExecutorUpdate(
            nullptr, collection, &parsedUpdate, boost::none /* verbosity */);
        if (!planExecutorResult.isOK()) {
            return planExecutorResult.getStatus();
        }
        auto planExecutor = std::move(planExecutorResult.getValue());

        try {
            // The update result is ignored.
            [[maybe_unused]] auto updateResult = planExecutor->executeUpdate();
        } catch (const StorageUnavailableException&) {
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

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::upsertById", nsOrUUID, [&] {
        const auto collection =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx, nsOrUUID, AcquisitionPrerequisites::kWrite),
                              MODE_IX);
        if (!collection.exists()) {
            return Status{ErrorCodes::NamespaceNotFound,
                          str::stream()
                              << "Collection [" << nsOrUUID.toStringForErrorMsg() << "] not found. "
                              << "Unable to update document."};
        }

        // We can create an UpdateRequest now that the collection's namespace has been resolved, in
        // the event it was specified as a UUID.
        auto request = UpdateRequest();
        request.setNamespaceString(collection.nss());
        request.setQuery(query);
        request.setUpdateModification(
            write_ops::UpdateModification::parseFromClassicUpdate(update));
        request.setUpsert(true);
        invariant(!request.isMulti());  // This follows from using an exact _id query.
        invariant(!request.shouldReturnAnyDocs());
        invariant(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY == request.getYieldPolicy());

        // ParsedUpdate needs to be inside the write conflict retry loop because it contains
        // the UpdateDriver whose state may be modified while we are applying the update.
        ParsedUpdate parsedUpdate(opCtx, &request, collection.getCollectionPtr());
        auto parsedUpdateStatus = parsedUpdate.parseRequest();
        if (!parsedUpdateStatus.isOK()) {
            return parsedUpdateStatus;
        }

        // We're using the ID hack to perform the update so we have to disallow collections
        // without an _id index.
        auto descriptor = collection.getCollectionPtr()->getIndexCatalog()->findIdIndex(opCtx);
        if (!descriptor) {
            return Status(ErrorCodes::IndexNotFound,
                          "Unable to update document in a collection without an _id index.");
        }

        UpdateStageParams updateStageParams(
            parsedUpdate.getRequest(), parsedUpdate.getDriver(), nullptr);
        auto planExecutor = InternalPlanner::updateWithIdHack(opCtx,
                                                              collection,
                                                              updateStageParams,
                                                              descriptor,
                                                              idKey.wrap(""),
                                                              parsedUpdate.yieldPolicy());

        try {
            // The update result is ignored.
            [[maybe_unused]] auto updateResult = planExecutor->executeUpdate();
        } catch (const StorageUnavailableException&) {
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
    return putSingleton(opCtx, nss, {} /* query */, update);
}

Status StorageInterfaceImpl::putSingleton(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& query,
                                          const TimestampedBSONObj& update) {
    auto request = UpdateRequest();
    request.setNamespaceString(nss);
    request.setQuery(query);
    request.setUpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(update.obj));
    request.setUpsert(true);
    invariant(!request.isMulti());  // We only want to update one document for performance.
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
    invariant(!request.isMulti());  // We only want to update one document for performance.
    return _updateWithQuery(opCtx, request, update.timestamp);
}

Status StorageInterfaceImpl::updateDocuments(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const TimestampedBSONObj& update,
    const boost::optional<std::vector<BSONObj>>& arrayFilters) {
    auto request = UpdateRequest();
    request.setNamespaceString(nss);
    request.setQuery(query);
    request.setUpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(update.obj));
    request.setMulti(true);
    if (arrayFilters) {
        request.setArrayFilters(arrayFilters.get());
    }
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
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);

    // This disables the isLegalClientSystemNS() check in getExecutorDelete() which is used to
    // disallow client deletes from unrecognized system collections.
    request.setGod(true);

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::deleteByFilter", nss, [&] {
        const auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        if (!collection.exists()) {
            return Status{ErrorCodes::NamespaceNotFound,
                          str::stream()
                              << "Collection [" << nss.toStringForErrorMsg() << "] not found. "
                              << "Unable to delete documents in " << nss.toStringForErrorMsg()
                              << " using filter " << filter};
        }

        // ParsedDelete needs to be inside the write conflict retry loop because it may create a
        // CanonicalQuery whose ownership will be transferred to the plan executor in
        // getExecutorDelete().
        ParsedDelete parsedDelete(opCtx, &request, collection.getCollectionPtr());
        auto parsedDeleteStatus = parsedDelete.parseRequest();
        if (!parsedDeleteStatus.isOK()) {
            return parsedDeleteStatus;
        }

        auto planExecutorResult = mongo::getExecutorDelete(
            nullptr, collection, &parsedDelete, boost::none /* verbosity */);
        if (!planExecutorResult.isOK()) {
            return planExecutorResult.getStatus();
        }
        auto planExecutor = std::move(planExecutorResult.getValue());

        try {
            // The count of deleted documents is ignored.
            [[maybe_unused]] auto nDeleted = planExecutor->executeDelete();
        } catch (const StorageUnavailableException&) {
            throw;
        } catch (const DBException&) {
            return exceptionToStatus();
        }
        return Status::OK();
    });
}

boost::optional<OpTimeAndWallTime> StorageInterfaceImpl::findOplogOpTimeLessThanOrEqualToTimestamp(
    OperationContext* opCtx, const CollectionPtr& oplog, const Timestamp& timestamp) {
    invariant(oplog);
    invariant(shard_role_details::getLocker(opCtx)->isLocked());

    // A record id in the oplog collection is equivalent to the document's timestamp field.
    RecordId desiredRecordId = RecordId(timestamp.asULL());
    // Define a backward cursor so that the seek operation returns the first recordId less than or
    // equal to the 'desiredRecordId', if it exists.
    auto cursor = oplog->getRecordStore()->getCursor(opCtx, false /* forward */);
    if (auto record =
            cursor->seek(desiredRecordId, SeekableRecordCursor::BoundInclusion::kInclude)) {
        invariant(record->id <= desiredRecordId,
                  "RecordId returned from seek (" + record->id.toString() +
                      ") is greater than the desired recordId (" + desiredRecordId.toString() +
                      ").");
        return fassert(
            8694200,
            OpTimeAndWallTime::parseOpTimeAndWallTimeFromOplogEntry(record->data.toBson()));
    }

    return boost::none;
}

boost::optional<OpTimeAndWallTime>
StorageInterfaceImpl::findOplogOpTimeLessThanOrEqualToTimestampRetryOnWCE(
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
            return findOplogOpTimeLessThanOrEqualToTimestamp(opCtx, oplogCollection, timestamp);
        } catch (const StorageUnavailableException&) {
            // This will log a message about the conflict initially and then every 5 seconds, with
            // the current rather arbitrary settings.
            if (retries % 10 == 0) {
                LOGV2(7754201,
                      "Got a StorageUnavailableException while reading the oplog. This "
                      "could be due to conflict with a validate cmd. Continuing to retry.",
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
        AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
        return oplogRead.getCollection()->getRecordStore()->getEarliestOplogTimestamp(opCtx);
    }();

    // If the storage engine does not support getEarliestOplogTimestamp(), then fall back to higher
    // level (above the storage engine) logic to fetch the earliest oplog entry timestamp.
    if (statusWithTimestamp.getStatus() == ErrorCodes::OplogOperationUnsupported) {
        // Reset the snapshot so that it is ensured to see the latest oplog entries.
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

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

    // The oplog can be empty when an initial syncing node crashes before the oplog application
    // phase.
    if (statusWithTimestamp.getStatus() == ErrorCodes::CollectionIsEmpty) {
        return Timestamp::min();
    }

    tassert(5869102,
            str::stream() << "Unexpected status: " << statusWithTimestamp.getStatus(),
            statusWithTimestamp.isOK());

    return statusWithTimestamp.getValue();
}

Timestamp StorageInterfaceImpl::getLatestOplogTimestamp(OperationContext* opCtx) {
    auto statusWithTimestamp = [&]() {
        AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
        return oplogRead.getCollection()->getRecordStore()->getLatestOplogTimestamp(opCtx);
    }();

    // If the storage engine does not support getLatestOplogTimestamp, then fall back to higher
    // level (above the storage engine) logic to fetch the latest oplog entry timestamp.
    if (statusWithTimestamp.getStatus() == ErrorCodes::OplogOperationUnsupported) {
        // Reset the snapshot so that it is ensured to see the latest oplog entries.
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

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
        autoColl,
        nss,
        str::stream() << "Unable to get UUID of " << nss.toStringForErrorMsg() << " collection.");
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

Timestamp StorageInterfaceImpl::getInitialDataTimestamp(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->getInitialDataTimestamp();
}


Timestamp StorageInterfaceImpl::recoverToStableTimestamp(OperationContext* opCtx) {
    auto serviceContext = opCtx->getServiceContext();

    // Pass an InterruptedDueToReplStateChange error to async callers waiting on the JournalFlusher
    // thread for durability.
    Status reason = Status(ErrorCodes::InterruptedDueToReplStateChange, "Rollback in progress.");
    StorageControl::stopStorageControls(serviceContext, reason, /*forRestart=*/true);
    stopOplogCapMaintainerThread(serviceContext, reason);

    auto swStableTimestamp = serviceContext->getStorageEngine()->recoverToStableTimestamp(opCtx);
    if (!swStableTimestamp.isOK()) {
        // Dump storage engine contents (including transaction information) before fatally
        // asserting.
        serviceContext->getStorageEngine()->dump();
    }
    fassert(31049, swStableTimestamp);

    StorageControl::startStorageControls(serviceContext);
    startOplogCapMaintainerThread(
        serviceContext,
        repl::ReplicationCoordinator::get(serviceContext)->getSettings().isReplSet(),
        repl::ReplSettings::shouldSkipOplogSampling());

    return swStableTimestamp.getValue();
}

bool StorageInterfaceImpl::supportsRecoverToStableTimestamp(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->supportsRecoverToStableTimestamp();
}

bool StorageInterfaceImpl::supportsRecoveryTimestamp(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->supportsRecoveryTimestamp();
}

boost::optional<Timestamp> StorageInterfaceImpl::getRecoveryTimestamp(
    ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->getRecoveryTimestamp();
}

void StorageInterfaceImpl::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                                   bool primaryOnly) {
    // Waiting for oplog writes to be visible in the oplog does not use any storage engine resources
    // and must not wait for ticket acquisition to avoid deadlocks with updating oplog visibility.
    ScopedAdmissionPriority<ExecutionAdmissionContext> setTicketAquisition(
        opCtx, AdmissionContext::Priority::kExempt);

    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    if (primaryOnly &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                              DatabaseName::kAdmin))
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
    ScopedAdmissionPriority<ExecutionAdmissionContext> setTicketAquisition(
        opCtx, AdmissionContext::Priority::kExempt);

    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    fassert(28557,
            oplogRead.getCollection()->getRecordStore()->oplogDiskLocRegister(
                opCtx, ts, orderedCommit));
}

boost::optional<Timestamp> StorageInterfaceImpl::getLastStableRecoveryTimestamp(
    ServiceContext* serviceCtx) const {
    if (!supportsRecoverToStableTimestamp(serviceCtx)) {
        return boost::none;
    }

    auto ret = serviceCtx->getStorageEngine()->getLastStableRecoveryTimestamp();
    if (ret == boost::none) {
        return Timestamp::min();
    }

    return ret;
}

Timestamp StorageInterfaceImpl::getAllDurableTimestamp(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->getAllDurableTimestamp();
}

Timestamp StorageInterfaceImpl::getPointInTimeReadTimestamp(OperationContext* opCtx) const {
    auto readTimestamp =
        shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);
    invariant(readTimestamp);
    return *readTimestamp;
}

void StorageInterfaceImpl::setPinnedOplogTimestamp(OperationContext* opCtx,
                                                   const Timestamp& pinnedTimestamp) const {
    opCtx->getServiceContext()->getStorageEngine()->setPinnedOplogTimestamp(pinnedTimestamp);
}

}  // namespace repl
}  // namespace mongo
