/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/write_ops/write_op_helper.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/topology/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"

namespace mongo {
namespace write_op_helpers {

bool isRetryErrCode(int errCode) {
    return errCode == ErrorCodes::StaleConfig || errCode == ErrorCodes::StaleDbVersion ||
        errCode == ErrorCodes::ShardCannotRefreshDueToLocksHeld ||
        errCode == ErrorCodes::CannotImplicitlyCreateCollection;
}

bool shouldTargetAllShardsSVIgnored(bool inTransaction, bool isMulti) {
    // Fetch the 'onlyTargetDataOwningShardsForMultiWrites' cluster param.
    if (isMulti && !inTransaction) {
        auto* clusterParam =
            ServerParameterSet::getClusterParameterSet()
                ->get<ClusterParameterWithStorage<OnlyTargetDataOwningShardsForMultiWritesParam>>(
                    "onlyTargetDataOwningShardsForMultiWrites");
        return !clusterParam->getValue(boost::none).getEnabled();
    }
    return false;
}

bool isSafeToIgnoreErrorInPartiallyAppliedOp(const Status& status) {
    return status.code() == ErrorCodes::CollectionUUIDMismatch &&
        !status.extraInfo<CollectionUUIDMismatchInfo>()->actualCollection();
}

int computeBaseSizeEstimate(OperationContext* opCtx, WriteCommandRef cmdRef) {
    // For simplicity, we build a dummy bulk write command request that contains all the common
    // fields and serialize it to get the base command size. We only bother to copy over variable
    // size and/or optional fields, since the value of fields that are fixed-size and always present
    // (e.g. 'ordered') won't affect the size calculation.
    BulkWriteCommandRequest request;

    // We'll account for the size to store each individual nsInfo as we add them, so just put an
    // empty vector as a placeholder for the array. This will ensure we properly count the size of
    // the field name and the empty array.
    request.setNsInfo({});

    // Bulk writes are executed against the admin database.
    request.setDbName(DatabaseName::kAdmin);
    request.setLet(cmdRef.getLet());

    // We'll account for the size to store each individual op as we add them, so just put an empty
    // vector as a placeholder for the array. This will ensure we properly count the size of the
    // field name and the empty array.
    request.setOps({});

    if (opCtx->isRetryableWrite()) {
        // We'll account for the size to store each individual stmtId as we add ops, so similar to
        // above with ops, we just put an empty vector as a placeholder for now.
        request.setStmtIds({});
    }

    request.setBypassEmptyTsReplacement(cmdRef.getBypassEmptyTsReplacement());

    if (cmdRef.isBulkWriteCommand()) {
        request.setComment(cmdRef.getComment());
    }

    BSONObjBuilder builder;
    request.serialize(&builder);
    // Add writeConcern and lsid/txnNumber to ensure we save space for them.
    logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
    builder.append(WriteConcernOptions::kWriteConcernField, opCtx->getWriteConcern().toBSON());

    return builder.obj().objsize();
}
BulkCommandSizeEstimator::BulkCommandSizeEstimator(OperationContext* opCtx, WriteCommandRef cmdRef)
    : _cmdRef(std::move(cmdRef)),
      _isRetryableWriteOrInTransaction(opCtx->getTxnNumber().has_value()),
      _baseSizeEstimate(computeBaseSizeEstimate(opCtx, _cmdRef)) {}

int BulkCommandSizeEstimator::getBaseSizeEstimate() const {
    return _baseSizeEstimate;
}

int BulkCommandSizeEstimator::getOpSizeEstimate(int opIdx, const ShardId& shardId) const {
    const auto op = WriteOpRef{_cmdRef, opIdx};

    // If retryable writes are used, MongoS needs to send an additional array of stmtId(s)
    // corresponding to the statements that got routed to each individual shard, so they need to
    // be accounted in the potential request size so it does not exceed the max BSON size.
    // TODO(SERVER-115826): Update size estimation logic now that WriteBatchExecutor uses
    // insert/update/delete commands.
    int writeSizeBytes = op.estimateOpSizeInBytesAsBulkOp() +
        write_ops::kWriteCommandBSONArrayPerElementOverheadBytes +
        (_isRetryableWriteOrInTransaction
             ? write_ops::kStmtIdSize + write_ops::kWriteCommandBSONArrayPerElementOverheadBytes
             : 0);

    // Get the set of nsInfos we've accounted for on this shardId.
    auto iter = _accountedForNsInfos.find(shardId);

    // If we have not accounted for this one already then increase the write size estimate by the
    // nsInfo size and store this index so it does not get counted again.
    const auto nss = op.getNss();
    if (iter == _accountedForNsInfos.end() || !iter->second.contains(nss)) {
        // Account for optional fields that can be set per namespace to have a conservative
        // estimate.
        static const ShardVersion mockShardVersion =
            ShardVersionFactory::make(ChunkVersion::IGNORED());
        static const DatabaseVersion mockDBVersion = DatabaseVersion(UUID::gen(), Timestamp());

        NamespaceInfoEntry nsEntry(nss);
        nsEntry.setCollectionUUID(op.getCollectionUUID());
        nsEntry.setEncryptionInformation(op.getEncryptionInformation());

        nsEntry.setShardVersion(mockShardVersion);
        nsEntry.setDatabaseVersion(mockDBVersion);
        if (!nsEntry.getNs().isTimeseriesBucketsCollection()) {
            // This could be a timeseries view. To be conservative about the estimate, we
            // speculatively account for the additional size needed for the timeseries bucket
            // translation and the 'isTimeseriesCollection' field.
            nsEntry.setNs(nsEntry.getNs().makeTimeseriesBucketsNamespace());
            nsEntry.setIsTimeseriesNamespace(true);
        }

        writeSizeBytes +=
            nsEntry.toBSON().objsize() + write_ops::kWriteCommandBSONArrayPerElementOverheadBytes;
    }

    return writeSizeBytes;
}

void BulkCommandSizeEstimator::addOpToBatch(int opIdx, const ShardId& shardId) {
    // Get the set of nsInfos we've accounted for on this shardId.
    _accountedForNsInfos.try_emplace(shardId, absl::flat_hash_set<NamespaceString>());
    auto iter = _accountedForNsInfos.find(shardId);
    tassert(10414702,
            "Expected to find namespace in accounted for namespaces",
            iter != _accountedForNsInfos.end());

    // If we have not accounted for this one already then store the namespace so it doesn't get
    // counted again.
    const auto op = WriteOpRef{_cmdRef, opIdx};
    iter->second.insert(op.getNss());
}

BulkWriteUpdateOp toBulkWriteUpdate(const write_ops::UpdateOpEntry& op) {
    // TODO SERVER-107545: Move this check to parse time and potentially convert this to a tassert.
    uassert(ErrorCodes::FailedToParse,
            "Cannot specify sort with multi=true",
            !op.getSort() || !op.getMulti());

    BulkWriteUpdateOp update;

    // Set 'nsInfoIdx' to 0, as there is only one namespace in a regular update.
    update.setNsInfoIdx(0);
    update.setFilter(op.getQ());
    update.setMulti(op.getMulti());
    update.setConstants(op.getC());
    update.setUpdateMods(op.getU());
    update.setSort(op.getSort());
    update.setHint(op.getHint());
    update.setCollation(op.getCollation());
    update.setArrayFilters(op.getArrayFilters());
    update.setUpsert(op.getUpsert());
    update.setUpsertSupplied(op.getUpsertSupplied());
    update.setSampleId(op.getSampleId());
    update.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
        op.getAllowShardKeyUpdatesWithoutFullShardKeyInQuery());
    return update;
}

BulkWriteDeleteOp toBulkWriteDelete(const write_ops::DeleteOpEntry& op) {
    BulkWriteDeleteOp deleteOp;

    // Set 'nsInfoIdx' to 0, as there is only one namespace in a regular delete.
    deleteOp.setNsInfoIdx(0);
    if (op.getCollation()) {
        deleteOp.setCollation(op.getCollation());
    }
    deleteOp.setHint(op.getHint());
    deleteOp.setMulti(op.getMulti());
    deleteOp.setFilter(op.getQ());
    return deleteOp;
}

BulkWriteOpVariant getOrMakeBulkWriteOp(WriteOpRef op) {
    tassert(10394907,
            "Unexpected findAndModify command to convert to bulkWrite op",
            !op.isFindAndModify());
    return op.visitOpData(OverloadedVisitor{
        [&](const BSONObj& insertDoc) -> BulkWriteOpVariant {
            return BulkWriteInsertOp(0, insertDoc);
        },
        [&](const write_ops::UpdateOpEntry& updateOp) -> BulkWriteOpVariant {
            return write_op_helpers::toBulkWriteUpdate(updateOp);
        },
        [&](const write_ops::DeleteOpEntry& deleteOp) -> BulkWriteOpVariant {
            return write_op_helpers::toBulkWriteDelete(deleteOp);
        },
        [&](const mongo::BulkWriteInsertOp& insertOp) -> BulkWriteOpVariant { return insertOp; },
        [&](const mongo::BulkWriteUpdateOp& updateOp) -> BulkWriteOpVariant { return updateOp; },
        [&](const mongo::BulkWriteDeleteOp& deleteOp) -> BulkWriteOpVariant { return deleteOp; },
        [&](const write_ops::FindAndModifyCommandRequest&) -> BulkWriteOpVariant {
            MONGO_UNREACHABLE;
        }});
}

write_ops::UpdateOpEntry getOrMakeUpdateOpEntry(UpdateOpRef updateOp) {
    auto cmdRef = updateOp.getCommand();

    if (cmdRef.isBatchWriteCommand()) {
        auto batchType = cmdRef.getBatchedCommandRequest().getBatchType();
        tassert(11468100, "Expected update", batchType == BatchedCommandRequest::BatchType_Update);

        const auto& request = cmdRef.getBatchedCommandRequest().getUpdateRequest();
        auto idx = updateOp.getIndex();
        const bool inBounds = (idx >= 0 && static_cast<size_t>(idx) < request.getUpdates().size());
        tassert(11468113, "Expected index to be in bounds", inBounds);

        return request.getUpdates()[idx];
    }

    // TODO SERVER-107545: Move this check to parse time and potentially convert this to a
    // tassert.
    uassert(ErrorCodes::FailedToParse,
            "Cannot specify sort with multi=true",
            !updateOp.getSort() || !updateOp.getMulti());

    write_ops::UpdateOpEntry updateOpEntry;
    updateOpEntry.setQ(updateOp.getFilter());
    updateOpEntry.setMulti(updateOp.getMulti());
    updateOpEntry.setC(updateOp.getConstants());
    updateOpEntry.setU(updateOp.getUpdateMods());
    updateOpEntry.setSort(updateOp.getSort());
    updateOpEntry.setHint(updateOp.getHint());
    updateOpEntry.setCollation(updateOp.getCollation());
    updateOpEntry.setArrayFilters(updateOp.getArrayFilters());
    updateOpEntry.setUpsert(updateOp.getUpsert());
    updateOpEntry.setUpsertSupplied(updateOp.getUpsertSupplied());
    updateOpEntry.setIncludeQueryStatsMetrics(updateOp.getIncludeQueryStatsMetrics());
    updateOpEntry.setSampleId(updateOp.getSampleId());
    updateOpEntry.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
        updateOp.getAllowShardKeyUpdatesWithoutFullShardKeyInQuery());

    return updateOpEntry;
}

write_ops::DeleteOpEntry getOrMakeDeleteOpEntry(DeleteOpRef deleteOp) {
    auto cmdRef = deleteOp.getCommand();

    if (cmdRef.isBatchWriteCommand()) {
        auto batchType = cmdRef.getBatchedCommandRequest().getBatchType();
        tassert(11468101, "Expected delete", batchType == BatchedCommandRequest::BatchType_Delete);

        const auto& request = cmdRef.getBatchedCommandRequest().getDeleteRequest();
        auto idx = deleteOp.getIndex();
        const bool inBounds = (idx >= 0 && static_cast<size_t>(idx) < request.getDeletes().size());
        tassert(11468114, "Expected index to be in bounds", inBounds);

        return request.getDeletes()[idx];
    }

    write_ops::DeleteOpEntry deleteOpEntry;
    deleteOpEntry.setQ(deleteOp.getFilter());
    deleteOpEntry.setMulti(deleteOp.getMulti());
    deleteOpEntry.setHint(deleteOp.getHint());
    deleteOpEntry.setCollation(deleteOp.getCollation());
    deleteOpEntry.setSampleId(deleteOp.getSampleId());

    return deleteOpEntry;
}

}  // namespace write_op_helpers
}  // namespace mongo
