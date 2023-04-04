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


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/db/change_stream_change_collection_manager.h"

#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"

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

    // The oplog entry might be a single delete command on a change collection, avoid
    // inserting such oplog entries back to the change collection.
    if (opTypeFieldElem == repl::OpType_serializer(repl::OpTypeEnum::kDelete) &&
        NamespaceString(nss).isChangeCollection()) {
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

    const auto isFromTenantMigration =
        oplogEntry.hasField(repl::OplogEntry::kFromTenantMigrationFieldName);
    const auto isNoop = oplogEntry.getStringField(repl::OplogEntry::kOpTypeFieldName) ==
        repl::OpType_serializer(repl::OpTypeEnum::kNoop);

    // Skip CRUD writes on user DBs from Tenant Migrations. Instead, extract that nested 'o2' from
    // the corresponding noop write to ensure that change events for user DB writes that took place
    // during a Tenant Migration are on the Donor timeline.
    const auto oplogDoc = [&]() -> boost::optional<Document> {
        if (!isFromTenantMigration) {
            return Document(oplogEntry);
        }

        if (!isNoop) {
            return boost::none;
        }

        const auto o2 = oplogEntry.getObjectField(repl::OplogEntry::kObject2FieldName);
        if (o2.isEmpty()) {
            return boost::none;
        }

        if (shouldSkipOplogEntry(o2)) {
            return boost::none;
        }

        return Document(o2);
    }();

    if (!oplogDoc) {
        return boost::none;
    }

    MutableDocument changeCollDoc(oplogDoc.get());
    changeCollDoc[repl::OplogEntry::k_idFieldName] =
        Value(oplogDoc->getField(repl::OplogEntry::kTimestampFieldName));

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
    void add(InsertStatement insertStatement) {
        if (auto tenantId = _extractTenantId(insertStatement); tenantId) {
            _tenantToStatementsAndChangeCollectionMap[*tenantId].insertStatements.push_back(
                std::move(insertStatement));
        }
    }

    /**
     * Acquires locks to change collections of all tenants referred to by added insert statements.
     */
    void acquireLocks() {
        tassert(6671503, "Locks cannot be acquired twice", !_locksAcquired);
        for (auto&& [tenantId, insertStatementsAndChangeCollection] :
             _tenantToStatementsAndChangeCollectionMap) {
            insertStatementsAndChangeCollection.tenantChangeCollection.emplace(
                _opCtx, _accessMode, tenantId);
        }
        _locksAcquired = true;
    }

    /**
     * Writes the batch of insert statements for each change collection. If a DuplicateKey error is
     * encountered, the write is skipped and the remaining inserts are attempted individually. Bails
     * out further writes if any other type of failure is encountered in writing to any change
     * collection.
     *
     * Locks should be acquired before calling this method by calling 'acquireLocks()'.
     */
    Status write() {
        tassert(6671504, "Locks should be acquired first", _locksAcquired);
        for (auto&& [tenantId, insertStatementsAndChangeCollection] :
             _tenantToStatementsAndChangeCollectionMap) {
            AutoGetChangeCollection& tenantChangeCollection =
                *insertStatementsAndChangeCollection.tenantChangeCollection;

            // The change collection does not exist for a particular tenant because either the
            // change collection is not enabled or is in the process of enablement. Ignore this
            // insert for now.
            if (!tenantChangeCollection) {
                continue;
            }

            // Writes to the change collection should not be replicated.
            repl::UnreplicatedWritesBlock unReplBlock(_opCtx);

            /**
             * For a serverless shard merge, we clone all change collection entries from the donor
             * and then fetch/apply retryable writes that took place before the migration. As a
             * result, we can end up in a situation where a change collection entry already exists.
             * If we encounter a DuplicateKey error and the entry is identical to the existing one,
             * we can safely skip and continue.
             */
            for (auto&& insertStatement : insertStatementsAndChangeCollection.insertStatements) {
                Status status = collection_internal::insertDocument(
                    _opCtx, *tenantChangeCollection, insertStatement, _opDebug, false);

                if (status.code() == ErrorCodes::DuplicateKey) {
                    const auto dupKeyInfo = status.extraInfo<DuplicateKeyErrorInfo>();
                    invariant(dupKeyInfo->toBSON()
                                  .getObjectField("foundValue")
                                  .binaryEqual(insertStatement.doc));
                    LOGV2(7282901,
                          "Ignoring DuplicateKey error for change collection insert",
                          "doc"_attr = insertStatement.doc.toString());
                } else if (!status.isOK()) {
                    return Status(status.code(),
                                  str::stream()
                                      << "Write to change collection: "
                                      << tenantChangeCollection->ns().toStringWithTenantId()
                                      << "failed")
                        .withReason(status.reason());
                }
            }
        }
        return Status::OK();
    }

private:
    /**
     * Field 'insertStatements' contains insert statements to be written to the tenant's change
     * collection associated with 'tenantChangeCollection' field.
     */
    struct TenantStatementsAndChangeCollection {

        std::vector<InsertStatement> insertStatements;

        boost::optional<AutoGetChangeCollection> tenantChangeCollection;
    };

    boost::optional<TenantId> _extractTenantId(const InsertStatement& insertStatement) {
        // Parse the oplog entry to fetch the tenant id from 'tid' field. The oplog entry will not
        // written to the change collection if 'tid' field is missing.
        auto& oplogDoc = insertStatement.doc;
        if (auto tidFieldElem = oplogDoc.getField(repl::OplogEntry::kTidFieldName)) {
            return TenantId{Value(tidFieldElem).getOid()};
        }

        if (MONGO_unlikely(internalChangeStreamUseTenantIdForTesting.load())) {
            return change_stream_serverless_helpers::getTenantIdForTesting();
        }

        return boost::none;
    }

    // Mode required to access change collections.
    const AutoGetChangeCollection::AccessMode _accessMode;

    // A mapping from a tenant id to insert statements and the change collection of the tenant.
    stdx::unordered_map<TenantId, TenantStatementsAndChangeCollection, TenantId::Hasher>
        _tenantToStatementsAndChangeCollectionMap;

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
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    _writer = std::make_unique<ChangeCollectionsWriterInternal>(
        opCtx, opDebug, AutoGetChangeCollection::AccessMode::kWrite);

    // Transform oplog entries to change collections entries and group them by tenant id.
    for (auto oplogEntryIter = beginOplogEntries; oplogEntryIter != endOplogEntries;
         oplogEntryIter++) {
        auto& oplogDoc = oplogEntryIter->doc;

        // The initial seed oplog insertion is not timestamped as such the 'oplogSlot' is not
        // initialized. The corresponding change collection insertion will not be timestamped.
        auto oplogSlot = oplogEntryIter->oplogSlot;

        auto changeCollDoc = createChangeCollectionEntryFromOplog(oplogDoc);

        if (changeCollDoc) {
            _writer->add(InsertStatement{
                std::move(*changeCollDoc), oplogSlot.getTimestamp(), oplogSlot.getTerm()});
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
                          << changeCollNss.toStringWithTenantId() << causedBy(status.reason()),
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
                          << changeCollNss.toStringWithTenantId() << causedBy(status.reason()),
            status.isOK() || status.code() == ErrorCodes::NamespaceNotFound);
}

void ChangeStreamChangeCollectionManager::insertDocumentsToChangeCollection(
    OperationContext* opCtx,
    const std::vector<Record>& oplogRecords,
    const std::vector<Timestamp>& oplogTimestamps) {
    invariant(oplogRecords.size() == oplogTimestamps.size());

    // This method must be called within a 'WriteUnitOfWork'. The caller must be responsible for
    // commiting the unit of work.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    ChangeCollectionsWriterInternal changeCollectionsWriter{
        opCtx, nullptr /*opDebug*/, AutoGetChangeCollection::AccessMode::kWriteInOplogContext};

    for (size_t idx = 0; idx < oplogRecords.size(); idx++) {
        auto& record = oplogRecords[idx];
        auto& ts = oplogTimestamps[idx];

        // Create an insert statement that should be written at the timestamp 'ts' for a particular
        // tenant.
        if (auto changeCollDoc = createChangeCollectionEntryFromOplog(record.data.toBson())) {
            changeCollectionsWriter.add(InsertStatement{
                std::move(changeCollDoc.get()), ts, repl::OpTime::kUninitializedTerm});
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
    OperationContext* opCtx, const CollectionPtr* changeCollection) {
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

size_t ChangeStreamChangeCollectionManager::removeExpiredChangeCollectionsDocuments(
    OperationContext* opCtx,
    const CollectionPtr* changeCollection,
    RecordIdBound maxRecordIdBound,
    Date_t expirationTime) {
    auto params = std::make_unique<DeleteStageParams>();
    params->isMulti = true;

    auto batchedDeleteParams = std::make_unique<BatchedDeleteStageParams>();
    LTEMatchExpression filter{"wall"_sd, Value(expirationTime)};
    auto deleteExecutor = InternalPlanner::deleteWithCollectionScan(
        opCtx,
        &(*changeCollection),
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
