/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/change_stream_change_collection_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/batched_delete_stage.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/ddl/drop_gen.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace {
const auto getChangeCollectionManager =
    ServiceContext::declareDecoration<boost::optional<ChangeStreamChangeCollectionManager>>();

// Helper used to determine whether or not a given oplog entry should be used to create a change
// collection entry.
bool shouldSkipOplogEntry(const BSONObj& oplogEntry) {
    auto nss = oplogEntry.getStringField(repl::OplogEntry::kNssFieldName);

    // Avoid writing entry with empty 'ns' field, for eg. 'periodic noop' entry.
    if (nss.empty()) {
        return true;
    }

    if (nss == "config.$cmd"_sd) {
        if (auto objectFieldElem = oplogEntry[repl::OplogEntry::kObjectFieldName]) {
            // The oplog entry might be a drop command on the change collection. Check if
            // the drop request is for the already deleted change collection, as such do not
            // attempt to write to the change collection if that is the case. This scenario
            // is possible because 'WriteUnitOfWork' will stage the changes and while
            // committing the staged 'CollectionImpl::insertDocuments' change the collection
            // object might have already been deleted.
            if (auto dropFieldElem = objectFieldElem["drop"_sd]) {
                return dropFieldElem.String() == NamespaceString::kChangeCollectionName;
            }

            // Do not write the change collection's own 'create' oplog entry. This is
            // because the secondaries will not be able to capture this oplog entry and as
            // such, will result in inconsistent state of the change collection in the
            // primary and the secondary.
            if (auto createFieldElem = objectFieldElem["create"_sd]) {
                return createFieldElem.String() == NamespaceString::kChangeCollectionName;
            }
        }
    }

    if (nss == "admin.$cmd"_sd) {
        if (auto objectFieldElem = oplogEntry[repl::OplogEntry::kObjectFieldName]) {
            // The oplog entry might be a batch delete command on a change collection, avoid
            // inserting such oplog entries back to the change collection.
            if (auto applyOpsFieldElem = objectFieldElem["applyOps"_sd]) {
                const auto nestedOperations = repl::ApplyOps::extractOperations(oplogEntry);
                for (auto& op : nestedOperations) {
                    if (op.getNss().isChangeCollection() &&
                        op.getOpType() == repl::OpTypeEnum::kDelete) {
                        return true;
                    }
                }
            }
        }
    }

    const auto opTypeFieldElem = oplogEntry.getStringField(repl::OplogEntry::kOpTypeFieldName);
    boost::optional<TenantId> tid = boost::none;
    if (auto tidFieldElem = oplogEntry.getField(repl::OplogEntry::kTidFieldName)) {
        tid = TenantId{Value(tidFieldElem).getOid()};
    }
    // The oplog entry might be a single delete command on a change collection, avoid
    // inserting such oplog entries back to the change collection.
    if (opTypeFieldElem == repl::OpType_serializer(repl::OpTypeEnum::kDelete) &&
        NamespaceStringUtil::deserialize(tid, nss, SerializationContext::stateDefault())
            .isChangeCollection()) {
        return true;
    }

    return false;
}

/**
 * Creates a Document object from the supplied oplog entry, performs necessary modifications to it
 * and then returns it as a BSON object. Can return boost::none if the entry should be skipped.
 */
boost::optional<BSONObj> createChangeCollectionEntryFromOplog(const BSONObj& oplogEntry) {
    if (shouldSkipOplogEntry(oplogEntry)) {
        return boost::none;
    }

    const Document oplogDoc{oplogEntry};

    MutableDocument changeCollDoc(oplogDoc);
    changeCollDoc[repl::OplogEntry::k_idFieldName] =
        Value(oplogDoc.getField(repl::OplogEntry::kTimestampFieldName));

    auto readyChangeCollDoc = changeCollDoc.freeze();
    return readyChangeCollDoc.toBson();
}
}  // namespace

/**
 * Locks respective change collections, writes insert statements to respective change collections
 * based on tenant ids.
 */
class ChangeStreamChangeCollectionManager::ChangeCollectionsWriterInternal {
public:
    explicit ChangeCollectionsWriterInternal(OperationContext* opCtx,
                                             OpDebug* opDebug,
                                             const AutoGetChangeCollection::AccessMode& accessMode)
        : _accessMode{accessMode}, _opCtx{opCtx}, _opDebug{opDebug} {}
    /**
     * Adds the insert statement for the provided tenant that will be written to the change
     * collection when the 'write()' method is called.
     */
    void add(BSONObj changeCollDoc, Timestamp ts, long long term) {
        if (auto tenantId = _extractTenantId(changeCollDoc); tenantId) {
            _tenantInsertStatements.emplace_back(std::move(changeCollDoc), ts, term, *tenantId);
            _tenantToChangeCollectionMap.try_emplace(*tenantId, boost::none);
        }
    }

    /**
     * Acquires locks to change collections of all tenants referred to by added insert statements.
     */
    void acquireLocks() {
        tassert(6671503, "Locks cannot be acquired twice", !_locksAcquired);
        for (auto&& [tenantId, autoGetChangeColl] : _tenantToChangeCollectionMap) {
            autoGetChangeColl.emplace(_opCtx, _accessMode, tenantId);
            // We assume that the tenant change collection is clustered so that we
            // can reconstruct the RecordId when performing writes.
            dassert(!(*autoGetChangeColl) || (*autoGetChangeColl)->isClustered());
        }
        _locksAcquired = true;
    }

    /**
     * Writes all the insert statements to their tenant change collections. If a DuplicateKey error
     * is encountered, the write is skipped and the remaining inserts are attempted individually.
     * Bails out further writes if any other type of failure is encountered when writing to any of
     * the change collections.
     *
     * Locks should be acquired before calling this method by calling 'acquireLocks()'.
     */
    Status write() {
        tassert(6671504, "Locks should be acquired first", _locksAcquired);

        stdx::unordered_map<TenantId, TenantWriteStats, TenantId::Hasher> tenantToWriteStatsMap;

        // Writes to the change collection should not be replicated.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        for (auto&& tenantInsertStatement : _tenantInsertStatements) {
            const auto& tenantId = tenantInsertStatement.tenantId;
            const auto& insertStatement = tenantInsertStatement.insertStatement;
            auto& tenantChangeCollection = *_tenantToChangeCollectionMap[tenantId];

            // The change collection does not exist for a particular tenant because either the
            // change collection is not enabled or is in the process of enablement. Ignore this
            // insert for now.
            if (!tenantChangeCollection) {
                continue;
            }

            Status status = collection_internal::insertDocument(
                _opCtx, *tenantChangeCollection, insertStatement, _opDebug, false);

            if (status.code() == ErrorCodes::DuplicateKey) {
                const auto dupKeyInfo = status.extraInfo<DuplicateKeyErrorInfo>();
                invariant(dupKeyInfo->toBSON()
                              .getObjectField("foundValue")
                              .binaryEqual(insertStatement.doc));
                LOGV2(7282901,
                      "Ignoring DuplicateKey error for change collection insert",
                      "doc"_attr = redact(insertStatement.doc.toString()));
                // Continue to the next insert statement as we've ommitted the current one.
                continue;
            } else if (!status.isOK()) {
                return Status(status.code(),
                              str::stream()
                                  << "Write to change collection: "
                                  << tenantChangeCollection->ns().toStringForErrorMsg() << "failed")
                    .withReason(status.reason());
            }

            // Right now we assume that the tenant change collection is clustered and
            // reconstruct the RecordId used in the KV store. Ideally we want the
            // write path to return the record ids used for the insert, but as it is
            // not available we reconstruct the key here.
            auto& tenantWriteStats = tenantToWriteStatsMap[tenantId];
            auto recordId = invariantStatusOK(record_id_helpers::keyForDoc(
                insertStatement.doc,
                tenantChangeCollection->getClusteredInfo()->getIndexSpec(),
                tenantChangeCollection->getDefaultCollator()));

            if (tenantWriteStats.maxRecordIdSeen < recordId) {
                tenantWriteStats.maxRecordIdSeen = std::move(recordId);
            }
            auto docWallTime =
                insertStatement.doc[repl::OplogEntry::kWallClockTimeFieldName].Date();
            if (tenantWriteStats.maxWallTimeSeen < docWallTime) {
                tenantWriteStats.maxWallTimeSeen = docWallTime;
            }
            tenantWriteStats.bytesInserted += insertStatement.doc.objsize();
            tenantWriteStats.docsInserted++;
        }

        return Status::OK();
    }

private:
    struct TenantInsertStatement {
        InsertStatement insertStatement;
        TenantId tenantId;
        TenantInsertStatement(BSONObj changeCollDoc,
                              Timestamp ts,
                              long long term,
                              TenantId tenantId)
            : insertStatement{std::move(changeCollDoc), ts, term}, tenantId{std::move(tenantId)} {}
    };

    struct TenantWriteStats {
        RecordId maxRecordIdSeen;
        Date_t maxWallTimeSeen;
        int64_t bytesInserted = 0;
        int64_t docsInserted = 0;
    };

    boost::optional<TenantId> _extractTenantId(const BSONObj& changeCollDoc) {
        // Parse the oplog entry to fetch the tenant id from 'tid' field. The entry will
        // not be written to the change collection if 'tid' field is missing.
        if (auto tidFieldElem = changeCollDoc.getField(repl::OplogEntry::kTidFieldName)) {
            return TenantId{Value(tidFieldElem).getOid()};
        }

        if (MONGO_unlikely(internalChangeStreamUseTenantIdForTesting.load())) {
            return change_stream_serverless_helpers::getTenantIdForTesting();
        }

        return boost::none;
    }

    // Mode required to access change collections.
    const AutoGetChangeCollection::AccessMode _accessMode;

    // A vector of all insert statements in timestamp order.
    std::vector<TenantInsertStatement> _tenantInsertStatements;

    struct TenantIdComparator {
        bool operator()(const TenantId& lhs, const TenantId& rhs) const {
            return ResourceId(RESOURCE_TENANT, lhs) < ResourceId(RESOURCE_TENANT, rhs);
        }
    };
    // A mapping from a tenant id to insert statements and the change collection of the tenant. We
    // don't use an unordered_map here since we will iterate the map in order to acquire locks
    // within the acquireLocks() method. This avoids possible deadlocks based on lock inversion.
    std::map<TenantId, boost::optional<AutoGetChangeCollection>, TenantIdComparator>
        _tenantToChangeCollectionMap;

    // An operation context to use while performing all operations in this class.
    OperationContext* const _opCtx;

    // An OpDebug to use while performing all operations in this class.
    OpDebug* const _opDebug;

    // Indicates if locks have been acquired.
    bool _locksAcquired{false};
};

ChangeStreamChangeCollectionManager::ChangeCollectionsWriter::ChangeCollectionsWriter(
    OperationContext* opCtx,
    std::vector<InsertStatement>::const_iterator beginOplogEntries,
    std::vector<InsertStatement>::const_iterator endOplogEntries,
    OpDebug* opDebug) {
    // This method must be called within a 'WriteUnitOfWork'. The caller must be responsible for
    // commiting the unit of work.
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    _writer = std::make_unique<ChangeCollectionsWriterInternal>(
        opCtx, opDebug, AutoGetChangeCollection::AccessMode::kWrite);

    // Transform oplog entries to change collections entries and group them by tenant id.
    for (auto oplogEntryIter = beginOplogEntries; oplogEntryIter != endOplogEntries;
         oplogEntryIter++) {
        auto& oplogDoc = oplogEntryIter->doc;

        // The initial seed oplog insertion is not timestamped as such the 'oplogSlot' is not
        // initialized. The corresponding change collection insertion will not be timestamped.
        auto oplogSlot = oplogEntryIter->oplogSlot;

        if (auto changeCollDoc = createChangeCollectionEntryFromOplog(oplogDoc)) {
            _writer->add(std::move(*changeCollDoc), oplogSlot.getTimestamp(), oplogSlot.getTerm());
        }
    }
}

ChangeStreamChangeCollectionManager::ChangeCollectionsWriter::ChangeCollectionsWriter(
    ChangeStreamChangeCollectionManager::ChangeCollectionsWriter&& other) = default;

ChangeStreamChangeCollectionManager::ChangeCollectionsWriter&
ChangeStreamChangeCollectionManager::ChangeCollectionsWriter::operator=(
    ChangeStreamChangeCollectionManager::ChangeCollectionsWriter&& other) = default;

ChangeStreamChangeCollectionManager::ChangeCollectionsWriter::~ChangeCollectionsWriter() = default;

void ChangeStreamChangeCollectionManager::ChangeCollectionsWriter::acquireLocks() {
    _writer->acquireLocks();
}

Status ChangeStreamChangeCollectionManager::ChangeCollectionsWriter::write() {
    return _writer->write();
}

ChangeStreamChangeCollectionManager::ChangeCollectionsWriter
ChangeStreamChangeCollectionManager::createChangeCollectionsWriter(
    OperationContext* opCtx,
    std::vector<InsertStatement>::const_iterator beginOplogEntries,
    std::vector<InsertStatement>::const_iterator endOplogEntries,
    OpDebug* opDebug) {
    return ChangeCollectionsWriter{opCtx, beginOplogEntries, endOplogEntries, opDebug};
}

BSONObj ChangeStreamChangeCollectionManager::PurgingJobStats::toBSON() const {
    return BSON("totalPass" << totalPass.load() << "docsDeleted" << docsDeleted.load()
                            << "bytesDeleted" << bytesDeleted.load() << "scannedCollections"
                            << scannedCollections.load() << "maxStartWallTimeMillis"
                            << maxStartWallTimeMillis.load() << "timeElapsedMillis"
                            << timeElapsedMillis.load());
}

ChangeStreamChangeCollectionManager& ChangeStreamChangeCollectionManager::get(
    ServiceContext* service) {
    return *getChangeCollectionManager(service);
}

ChangeStreamChangeCollectionManager& ChangeStreamChangeCollectionManager::get(
    OperationContext* opCtx) {
    return *getChangeCollectionManager(opCtx->getServiceContext());
}

void ChangeStreamChangeCollectionManager::create(ServiceContext* service) {
    getChangeCollectionManager(service).emplace(service);
}

void ChangeStreamChangeCollectionManager::createChangeCollection(OperationContext* opCtx,
                                                                 const TenantId& tenantId) {
    // Make the change collection clustered by '_id'. The '_id' field will have the same value as
    // the 'ts' field of the oplog.
    CollectionOptions changeCollectionOptions;
    changeCollectionOptions.clusteredIndex.emplace(clustered_util::makeDefaultClusteredIdIndex());
    changeCollectionOptions.capped = true;
    const auto changeCollNss = NamespaceString::makeChangeCollectionNSS(tenantId);

    const auto status = createCollection(opCtx, changeCollNss, changeCollectionOptions, BSONObj());
    uassert(status.code(),
            str::stream() << "Failed to create change collection: "
                          << changeCollNss.toStringForErrorMsg() << causedBy(status.reason()),
            status.isOK() || status.code() == ErrorCodes::NamespaceExists);
}

void ChangeStreamChangeCollectionManager::dropChangeCollection(OperationContext* opCtx,
                                                               const TenantId& tenantId) {
    DropReply dropReply;
    const auto changeCollNss = NamespaceString::makeChangeCollectionNSS(tenantId);
    const auto status =
        dropCollection(opCtx,
                       changeCollNss,
                       &dropReply,
                       DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
    uassert(status.code(),
            str::stream() << "Failed to drop change collection: "
                          << changeCollNss.toStringForErrorMsg() << causedBy(status.reason()),
            status.isOK() || status.code() == ErrorCodes::NamespaceNotFound);
}

void ChangeStreamChangeCollectionManager::insertDocumentsToChangeCollection(
    OperationContext* opCtx,
    const std::vector<Record>& oplogRecords,
    const std::vector<Timestamp>& oplogTimestamps) {
    invariant(oplogRecords.size() == oplogTimestamps.size());

    // This method must be called within a 'WriteUnitOfWork'. The caller must be responsible for
    // commiting the unit of work.
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    ChangeCollectionsWriterInternal changeCollectionsWriter{
        opCtx, nullptr /*opDebug*/, AutoGetChangeCollection::AccessMode::kWriteInOplogContext};

    for (size_t idx = 0; idx < oplogRecords.size(); idx++) {
        auto& record = oplogRecords[idx];
        auto& ts = oplogTimestamps[idx];

        // Create an insert statement that should be written at the timestamp 'ts' for a particular
        // tenant.
        if (auto changeCollDoc = createChangeCollectionEntryFromOplog(record.data.toBson())) {
            changeCollectionsWriter.add(
                std::move(*changeCollDoc), ts, repl::OpTime::kUninitializedTerm);
        }
    }

    changeCollectionsWriter.acquireLocks();

    // Write documents to change collections and throw exception in case of any failure.
    Status status = changeCollectionsWriter.write();
    if (!status.isOK()) {
        LOGV2_FATAL(
            6612300, "Failed to write to change collection", "reason"_attr = status.reason());
    }
}

boost::optional<ChangeCollectionPurgingJobMetadata>
ChangeStreamChangeCollectionManager::getChangeCollectionPurgingJobMetadata(
    OperationContext* opCtx, const CollectionAcquisition& changeCollection) {
    auto findWallTimeAndRecordIdForFirstDocument = [&](InternalPlanner::Direction direction)
        -> boost::optional<std::pair<long long, RecordId>> {
        BSONObj currChangeDoc;
        RecordId currRecordId;

        auto scanExecutor = InternalPlanner::collectionScan(
            opCtx, changeCollection, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, direction);
        switch (scanExecutor->getNext(&currChangeDoc, &currRecordId)) {
            case PlanExecutor::IS_EOF:
                return boost::none;
            case PlanExecutor::ADVANCED:
                return {{currChangeDoc["wall"].Date().toMillisSinceEpoch(), currRecordId}};
            default:
                MONGO_UNREACHABLE_TASSERT(7010800);
        }
    };

    const auto firstDocAttributes =
        findWallTimeAndRecordIdForFirstDocument(InternalPlanner::Direction::FORWARD);
    if (!firstDocAttributes) {
        return boost::none;
    }
    auto [_, lastDocRecordId] =
        *findWallTimeAndRecordIdForFirstDocument(InternalPlanner::Direction::BACKWARD);
    return {{firstDocAttributes->first, RecordIdBound(std::move(lastDocRecordId))}};
}

size_t ChangeStreamChangeCollectionManager::removeExpiredChangeCollectionsDocumentsWithCollScan(
    OperationContext* opCtx,
    const CollectionAcquisition& changeCollection,
    RecordIdBound maxRecordIdBound,
    Date_t expirationTime) {
    auto params = std::make_unique<DeleteStageParams>();
    params->isMulti = true;

    auto batchedDeleteParams = std::make_unique<BatchedDeleteStageParams>();
    LTEMatchExpression filter{"wall"_sd, Value(expirationTime)};
    auto deleteExecutor = InternalPlanner::deleteWithCollectionScan(
        opCtx,
        changeCollection,
        std::move(params),
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        InternalPlanner::Direction::FORWARD,
        boost::none /* minRecord */,
        std::move(maxRecordIdBound),
        CollectionScanParams::ScanBoundInclusion::kIncludeStartRecordOnly,
        std::move(batchedDeleteParams),
        &filter,
        true /* shouldReturnEofOnFilterMismatch */
    );

    try {
        (void)deleteExecutor->executeDelete();
        auto batchedDeleteStats = deleteExecutor->getBatchedDeleteStats();
        auto& changeCollectionManager = ChangeStreamChangeCollectionManager::get(opCtx);
        changeCollectionManager.getPurgingJobStats().docsDeleted.fetchAndAddRelaxed(
            batchedDeleteStats.docsDeleted);
        changeCollectionManager.getPurgingJobStats().bytesDeleted.fetchAndAddRelaxed(
            batchedDeleteStats.bytesDeleted);
        return batchedDeleteStats.docsDeleted;
    } catch (const ExceptionFor<ErrorCodes::QueryPlanKilled>&) {
        // It is expected that a collection drop can kill a query plan while deleting an old
        // document, so ignore this error.
        return 0;
    }
}
}  // namespace mongo
