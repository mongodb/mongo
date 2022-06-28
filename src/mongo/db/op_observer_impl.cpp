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

#include "mongo/db/op_observer_impl.h"

#include <algorithm>
#include <limits>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/batched_write_context.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/import_collection_oplog_entry_gen.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_pre_image_helpers.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
using repl::DurableOplogEntry;
using repl::MutableOplogEntry;
using ChangeStreamPreImageRecordingMode = repl::ReplOperation::ChangeStreamPreImageRecordingMode;

const OperationContext::Decoration<boost::optional<ShardId>> destinedRecipientDecoration =
    OperationContext::declareDecoration<boost::optional<ShardId>>();

namespace {

MONGO_FAIL_POINT_DEFINE(failCollectionUpdates);
MONGO_FAIL_POINT_DEFINE(hangAndFailUnpreparedCommitAfterReservingOplogSlot);
MONGO_FAIL_POINT_DEFINE(hangAfterLoggingApplyOpsForTransaction);

constexpr auto kNumRecordsFieldName = "numRecords"_sd;
constexpr auto kMsgFieldName = "msg"_sd;
constexpr long long kInvalidNumRecords = -1LL;

Date_t getWallClockTimeForOpLog(OperationContext* opCtx) {
    auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
    return clockSource->now();
}

repl::OpTime logOperation(OperationContext* opCtx,
                          MutableOplogEntry* oplogEntry,
                          bool assignWallClockTime = true) {
    if (assignWallClockTime) {
        oplogEntry->setWallClockTime(getWallClockTimeForOpLog(opCtx));
    }
    auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
    auto opTime = repl::logOp(opCtx, oplogEntry);
    times.push_back(opTime);
    return opTime;
}

/**
 * Updates the session state with the last write timestamp and transaction for that session.
 *
 * In the case of writes with transaction/statement id, this method will be recursively entered a
 * second time for the actual write to the transactions table. Since this write does not generate an
 * oplog entry, the recursion will stop at this point.
 */
void onWriteOpCompleted(OperationContext* opCtx,
                        std::vector<StmtId> stmtIdsWritten,
                        SessionTxnRecord sessionTxnRecord) {
    if (sessionTxnRecord.getLastWriteOpTime().isNull())
        return;

    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (!txnParticipant ||
        (!stmtIdsWritten.empty() && stmtIdsWritten.front() == kUninitializedStmtId))
        // If the first statement written in uninitialized, then all the statements are assumed to
        // be uninitialized.
        return;

    // We add these here since they may not exist if we return early.
    const auto lsid = *opCtx->getLogicalSessionId();
    sessionTxnRecord.setSessionId(lsid);
    if (isInternalSessionForRetryableWrite(lsid)) {
        sessionTxnRecord.setParentSessionId(*getParentSessionId(lsid));
    }
    sessionTxnRecord.setTxnNum(*opCtx->getTxnNumber());
    txnParticipant.onWriteOpCompletedOnPrimary(opCtx, std::move(stmtIdsWritten), sessionTxnRecord);
}

/**
 * Given the collection count from Collection::numRecords(), create and return the object for the
 * 'o2' field of a drop or rename oplog entry. If the collection count exceeds the upper limit of a
 * BSON NumberLong (long long), we will add a count of -1 and append a message with the original
 * collection count.
 *
 * Replication rollback uses this field to correct correction counts on drop-pending collections.
 */
BSONObj makeObject2ForDropOrRename(uint64_t numRecords) {
    BSONObjBuilder obj2Builder;
    if (numRecords > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
        obj2Builder.appendNumber(kNumRecordsFieldName, kInvalidNumRecords);
        std::string msg = str::stream() << "Collection count " << numRecords
                                        << " is larger than the "
                                           "maximum int64_t value. Setting numRecords to -1.";
        obj2Builder.append(kMsgFieldName, msg);
    } else {
        obj2Builder.appendNumber(kNumRecordsFieldName, static_cast<long long>(numRecords));
    }
    auto obj = obj2Builder.obj();
    return obj;
}

struct OpTimeBundle {
    repl::OpTime writeOpTime;
    repl::OpTime prePostImageOpTime;
    Date_t wallClockTime;
};

struct ImageBundle {
    repl::RetryImageEnum imageKind;
    BSONObj imageDoc;
    Timestamp timestamp;
};

/**
 * Write oplog entry(ies) for the update operation.
 */
OpTimeBundle replLogUpdate(OperationContext* opCtx,
                           const OplogUpdateEntryArgs& args,
                           MutableOplogEntry* oplogEntry) {
    // TODO SERVER-62114 Change to check for upgraded FCV rather than feature flag
    if (gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility))
        oplogEntry->setTid(args.nss.tenantId());
    oplogEntry->setNss(args.nss);
    oplogEntry->setUuid(args.uuid);

    repl::OplogLink oplogLink;
    repl::appendOplogEntryChainInfo(opCtx, oplogEntry, &oplogLink, args.updateArgs->stmtIds);

    OpTimeBundle opTimes;
    // We never want to store pre- or post- images when we're migrating oplog entries from another
    // replica set.
    const auto& migrationRecipientInfo = repl::tenantMigrationRecipientInfo(opCtx);
    const auto storePreImageInOplogForRetryableWrite =
        (args.updateArgs->storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage &&
         opCtx->getTxnNumber() && !oplogEntry->getNeedsRetryImage());
    if ((storePreImageInOplogForRetryableWrite ||
         args.updateArgs->preImageRecordingEnabledForCollection) &&
        !migrationRecipientInfo) {
        MutableOplogEntry noopEntry = *oplogEntry;
        invariant(args.updateArgs->preImageDoc);
        noopEntry.setOpType(repl::OpTypeEnum::kNoop);
        noopEntry.setObject(*args.updateArgs->preImageDoc);
        if (args.updateArgs->preImageRecordingEnabledForCollection &&
            args.retryableFindAndModifyLocation ==
                RetryableFindAndModifyLocation::kSideCollection) {
            // We are writing a no-op pre-image oplog entry and storing a post-image into a side
            // collection. In this case, we expect to have already reserved 3 oplog slots:
            // TS - 2: Oplog slot for the current no-op preimage oplog entry
            // TS - 1: Oplog slot for the forged no-op oplog entry that may eventually get used by
            //         tenant migrations or resharding.
            // TS:     Oplog slot for the actual update oplog entry.
            const auto reservedOplogSlots = args.updateArgs->oplogSlots;
            invariant(reservedOplogSlots.size() == 3);
            noopEntry.setOpTime(repl::OpTime(reservedOplogSlots.front().getTimestamp(),
                                             reservedOplogSlots.front().getTerm()));
        }
        oplogLink.preImageOpTime = logOperation(opCtx, &noopEntry);
        if (storePreImageInOplogForRetryableWrite) {
            opTimes.prePostImageOpTime = oplogLink.preImageOpTime;
        }
    }

    // This case handles storing the post image for retryable findAndModify's.
    if (args.updateArgs->storeDocOption == CollectionUpdateArgs::StoreDocOption::PostImage &&
        opCtx->getTxnNumber() && !migrationRecipientInfo && !oplogEntry->getNeedsRetryImage()) {
        MutableOplogEntry noopEntry = *oplogEntry;
        noopEntry.setOpType(repl::OpTypeEnum::kNoop);
        noopEntry.setObject(args.updateArgs->updatedDoc);
        oplogLink.postImageOpTime = logOperation(opCtx, &noopEntry);
        invariant(opTimes.prePostImageOpTime.isNull());
        opTimes.prePostImageOpTime = oplogLink.postImageOpTime;
    }

    oplogEntry->setOpType(repl::OpTypeEnum::kUpdate);
    oplogEntry->setObject(args.updateArgs->update);
    oplogEntry->setObject2(args.updateArgs->criteria);
    oplogEntry->setFromMigrateIfTrue(args.updateArgs->source == OperationSource::kFromMigrate);
    // oplogLink could have been changed to include pre/postImageOpTime by the previous no-op write.
    repl::appendOplogEntryChainInfo(opCtx, oplogEntry, &oplogLink, args.updateArgs->stmtIds);
    if (!args.updateArgs->oplogSlots.empty()) {
        oplogEntry->setOpTime(args.updateArgs->oplogSlots.back());
    }
    opTimes.writeOpTime = logOperation(opCtx, oplogEntry);
    opTimes.wallClockTime = oplogEntry->getWallClockTime();
    return opTimes;
}

/**
 * Write oplog entry(ies) for the delete operation.
 */
OpTimeBundle replLogDelete(OperationContext* opCtx,
                           const NamespaceString& nss,
                           MutableOplogEntry* oplogEntry,
                           const boost::optional<UUID>& uuid,
                           StmtId stmtId,
                           bool fromMigrate,
                           const boost::optional<BSONObj>& deletedDoc) {
    // TODO SERVER-62114 Change to check for upgraded FCV rather than feature flag
    if (gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility))
        oplogEntry->setTid(nss.tenantId());
    oplogEntry->setNss(nss);
    oplogEntry->setUuid(uuid);
    oplogEntry->setDestinedRecipient(destinedRecipientDecoration(opCtx));

    repl::OplogLink oplogLink;
    repl::appendOplogEntryChainInfo(opCtx, oplogEntry, &oplogLink, {stmtId});

    OpTimeBundle opTimes;
    // We never want to store pre-images when we're migrating oplog entries from another
    // replica set.
    const auto& migrationRecipientInfo = repl::tenantMigrationRecipientInfo(opCtx);
    if (deletedDoc && !migrationRecipientInfo) {
        MutableOplogEntry noopEntry = *oplogEntry;
        noopEntry.setOpType(repl::OpTypeEnum::kNoop);
        noopEntry.setObject(*deletedDoc);
        auto noteOplog = logOperation(opCtx, &noopEntry);
        opTimes.prePostImageOpTime = noteOplog;
        oplogLink.preImageOpTime = noteOplog;
    }

    oplogEntry->setOpType(repl::OpTypeEnum::kDelete);
    oplogEntry->setObject(repl::documentKeyDecoration(opCtx).get().getShardKeyAndId());
    oplogEntry->setFromMigrateIfTrue(fromMigrate);
    // oplogLink could have been changed to include preImageOpTime by the previous no-op write.
    repl::appendOplogEntryChainInfo(opCtx, oplogEntry, &oplogLink, {stmtId});
    opTimes.writeOpTime = logOperation(opCtx, oplogEntry);
    opTimes.wallClockTime = oplogEntry->getWallClockTime();
    return opTimes;
}

void writeToImageCollection(OperationContext* opCtx,
                            const LogicalSessionId& sessionId,
                            const Timestamp timestamp,
                            repl::RetryImageEnum imageKind,
                            const BSONObj& dataImage) {
    repl::ImageEntry imageEntry;
    imageEntry.set_id(sessionId);
    imageEntry.setTxnNumber(opCtx->getTxnNumber().get());
    imageEntry.setTs(timestamp);
    imageEntry.setImageKind(imageKind);
    imageEntry.setImage(dataImage);

    DisableDocumentValidation documentValidationDisabler(
        opCtx, DocumentValidationSettings::kDisableInternalValidation);

    // In practice, this lock acquisition on kConfigImagesNamespace cannot block. The only time a
    // stronger lock acquisition is taken on this namespace is during step up to create the
    // collection.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
    AutoGetCollection imageCollectionRaii(
        opCtx, NamespaceString::kConfigImagesNamespace, LockMode::MODE_IX);
    auto curOp = CurOp::get(opCtx);
    const std::string existingNs = curOp->getNS();
    UpdateResult res = Helpers::upsert(
        opCtx, NamespaceString::kConfigImagesNamespace.toString(), imageEntry.toBSON());
    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        curOp->setNS_inlock(existingNs);
    }

    invariant(res.numDocsModified == 1 || !res.upsertedId.isEmpty());
}

bool shouldTimestampIndexBuildSinglePhase(OperationContext* opCtx, const NamespaceString& nss) {
    // This function returns whether a timestamp for a catalog write when beginning an index build,
    // or aborting an index build is necessary. There are four scenarios:

    // 1. A timestamp is already set -- replication application sets a timestamp ahead of time.
    // This could include the phase of initial sync where it applies oplog entries.  Also,
    // primaries performing an index build via `applyOps` may have a wrapping commit timestamp.
    if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull())
        return false;

    // 2. If the node is initial syncing, we do not set a timestamp.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->isReplEnabled() && replCoord->getMemberState().startup2())
        return false;

    // 3. If the index build is on the local database, do not timestamp.
    if (nss.isLocal())
        return false;

    // 4. All other cases, we generate a timestamp by writing a no-op oplog entry.  This is
    // better than using a ghost timestamp.  Writing an oplog entry ensures this node is
    // primary.
    return true;
}

}  // namespace

void OpObserverImpl::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const UUID& uuid,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    if (inMultiDocumentTransaction) {
        auto operation = MutableOplogEntry::makeCreateIndexesCommand(nss, uuid, indexDoc);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        BSONObjBuilder builder;
        builder.append("createIndexes", nss.coll());
        builder.appendElements(indexDoc);

        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(nss.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(builder.done());
        oplogEntry.setFromMigrateIfTrue(fromMigrate);
        logOperation(opCtx, &oplogEntry);
    }
}

void OpObserverImpl::onStartIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("startIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (auto indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    if (!shouldTimestampIndexBuildSinglePhase(opCtx, nss)) {
        return;
    }


    onInternalOpMessage(
        opCtx,
        {},
        boost::none,
        BSON("msg" << std::string(str::stream() << "Creating indexes. Coll: " << nss)),
        boost::none,
        boost::none,
        boost::none,
        boost::none,
        boost::none);
}

void OpObserverImpl::onAbortIndexBuildSinglePhase(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    if (!shouldTimestampIndexBuildSinglePhase(opCtx, nss)) {
        return;
    }

    onInternalOpMessage(
        opCtx,
        {},
        boost::none,
        BSON("msg" << std::string(str::stream() << "Aborting indexes. Coll: " << nss)),
        boost::none,
        boost::none,
        boost::none,
        boost::none,
        boost::none);
}

void OpObserverImpl::onCommitIndexBuild(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const UUID& collUUID,
                                        const UUID& indexBuildUUID,
                                        const std::vector<BSONObj>& indexes,
                                        bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("commitIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (auto indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onAbortIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       const Status& cause,
                                       bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("abortIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (auto indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    BSONObjBuilder causeBuilder(oplogEntryBuilder.subobjStart("cause"));
    // Some functions that extract a Status from a BSONObj, such as getStatusFromCommandResult(),
    // expect the 'ok' field.
    causeBuilder.appendBool("ok", 0);
    cause.serializeErrorToBSON(&causeBuilder);
    causeBuilder.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onInserts(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& uuid,
                               std::vector<InsertStatement>::const_iterator first,
                               std::vector<InsertStatement>::const_iterator last,
                               bool fromMigrate) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    Date_t lastWriteDate;

    std::vector<repl::OpTime> opTimeList;
    repl::OpTime lastOpTime;

    ShardingWriteRouter shardingWriteRouter(opCtx, nss, Grid::get(opCtx)->catalogCache());

    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    const bool inBatchedWrite = batchedWriteContext.writesAreBatched();

    if (inBatchedWrite) {
        invariant(!fromMigrate);

        write_stage_common::PreWriteFilter preWriteFilter(opCtx, nss);

        for (auto iter = first; iter != last; iter++) {
            const auto docKey = repl::getDocumentKey(opCtx, nss, iter->doc).getShardKeyAndId();
            auto operation = MutableOplogEntry::makeInsertOperation(nss, uuid, iter->doc, docKey);
            operation.setDestinedRecipient(
                shardingWriteRouter.getReshardingDestinedRecipient(iter->doc));

            if (!OperationShardingState::isComingFromRouter(opCtx) &&
                preWriteFilter.computeAction(Document(iter->doc)) ==
                    write_stage_common::PreWriteFilter::Action::kWriteAsFromMigrate) {
                LOGV2_DEBUG(6585800,
                            3,
                            "Marking insert operation of orphan document with the 'fromMigrate' "
                            "flag to prevent a wrong change stream event",
                            "namespace"_attr = nss,
                            "document"_attr = iter->doc);

                operation.setFromMigrate(true);
            }

            batchedWriteContext.addBatchedOperation(opCtx, operation);
        }
    } else if (inMultiDocumentTransaction) {
        invariant(!fromMigrate);

        // Do not add writes to the profile collection to the list of transaction operations, since
        // these are done outside the transaction. There is no top-level WriteUnitOfWork when we are
        // in a SideTransactionBlock.
        if (!opCtx->getWriteUnitOfWork()) {
            invariant(nss.isSystemDotProfile());
            return;
        }

        const bool inRetryableInternalTransaction =
            isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId());
        write_stage_common::PreWriteFilter preWriteFilter(opCtx, nss);

        for (auto iter = first; iter != last; iter++) {
            const auto docKey = repl::getDocumentKey(opCtx, nss, iter->doc).getShardKeyAndId();
            auto operation = MutableOplogEntry::makeInsertOperation(nss, uuid, iter->doc, docKey);
            if (inRetryableInternalTransaction) {
                operation.setInitializedStatementIds(iter->stmtIds);
            }
            operation.setDestinedRecipient(
                shardingWriteRouter.getReshardingDestinedRecipient(iter->doc));

            if (!OperationShardingState::isComingFromRouter(opCtx) &&
                preWriteFilter.computeAction(Document(iter->doc)) ==
                    write_stage_common::PreWriteFilter::Action::kWriteAsFromMigrate) {
                LOGV2_DEBUG(6585801,
                            3,
                            "Marking insert operation of orphan document with the 'fromMigrate' "
                            "flag to prevent a wrong change stream event",
                            "namespace"_attr = nss,
                            "document"_attr = iter->doc);

                operation.setFromMigrate(true);
            }

            txnParticipant.addTransactionOperation(opCtx, operation);
        }
    } else {
        std::function<boost::optional<ShardId>(const BSONObj& doc)> getDestinedRecipientFn =
            [&shardingWriteRouter](const BSONObj& doc) {
                return shardingWriteRouter.getReshardingDestinedRecipient(doc);
            };

        MutableOplogEntry oplogEntryTemplate;
        // TODO SERVER-62114 Change to check for upgraded FCV rather than feature flag
        if (gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility))
            oplogEntryTemplate.setTid(nss.tenantId());
        oplogEntryTemplate.setNss(nss);
        oplogEntryTemplate.setUuid(uuid);
        oplogEntryTemplate.setFromMigrateIfTrue(fromMigrate);
        lastWriteDate = getWallClockTimeForOpLog(opCtx);
        oplogEntryTemplate.setWallClockTime(lastWriteDate);

        opTimeList =
            repl::logInsertOps(opCtx, &oplogEntryTemplate, first, last, getDestinedRecipientFn);
        if (!opTimeList.empty())
            lastOpTime = opTimeList.back();

        auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
        using std::begin;
        using std::end;
        times.insert(end(times), begin(opTimeList), end(opTimeList));

        std::vector<StmtId> stmtIdsWritten;
        std::for_each(first, last, [&](const InsertStatement& stmt) {
            stmtIdsWritten.insert(stmtIdsWritten.end(), stmt.stmtIds.begin(), stmt.stmtIds.end());
        });

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(lastOpTime);
        sessionTxnRecord.setLastWriteDate(lastWriteDate);
        onWriteOpCompleted(opCtx, stmtIdsWritten, sessionTxnRecord);
    }

    size_t index = 0;
    for (auto it = first; it != last; it++, index++) {
        auto opTime = opTimeList.empty() ? repl::OpTime() : opTimeList[index];
        shardObserveInsertOp(opCtx,
                             nss,
                             it->doc,
                             opTime,
                             shardingWriteRouter,
                             fromMigrate,
                             inMultiDocumentTransaction);
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        try {
            for (auto it = first; it != last; it++) {
                uassertStatusOK(DurableViewCatalog::onExternalInsert(opCtx, it->doc, nss));
            }
        } catch (const DBException&) {
            // If a previous operation left the view catalog in an invalid state, our inserts can
            // fail even if all the definitions are valid. Reloading may help us reset the state.
            DurableViewCatalog::onExternalChange(opCtx, nss);
        }
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace && !lastOpTime.isNull()) {
        for (auto it = first; it != last; it++) {
            MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx, it->doc);
        }
    } else if (nss == NamespaceString::kConfigSettingsNamespace) {
        for (auto it = first; it != last; it++) {
            ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
                opCtx, it->doc["_id"], it->doc);
        }
    } else if (nss == NamespaceString::kExternalKeysCollectionNamespace) {
        for (auto it = first; it != last; it++) {
            auto externalKey = ExternalKeysCollectionDocument::parse(
                IDLParserErrorContext("externalKey"), it->doc);
            opCtx->recoveryUnit()->onCommit(
                [this, opCtx, externalKey = std::move(externalKey)](
                    boost::optional<Timestamp> unusedCommitTime) mutable {
                    auto validator = LogicalTimeValidator::get(opCtx);
                    if (validator) {
                        validator->cacheExternalKey(externalKey);
                    }
                });
        }
    }
}

void OpObserverImpl::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    failCollectionUpdates.executeIf(
        [&](const BSONObj&) {
            uasserted(40654,
                      str::stream() << "failCollectionUpdates failpoint enabled, namespace: "
                                    << args.nss.ns() << ", update: " << args.updateArgs->update
                                    << " on document with " << args.updateArgs->criteria);
        },
        [&](const BSONObj& data) {
            // If the failpoint specifies no collection or matches the existing one, fail.
            auto collElem = data["collectionNS"];
            return !collElem || args.nss.ns() == collElem.String();
        });

    // Do not log a no-op operation; see SERVER-21738
    if (args.updateArgs->update.isEmpty()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    ShardingWriteRouter shardingWriteRouter(opCtx, args.nss, Grid::get(opCtx)->catalogCache());

    OpTimeBundle opTime;
    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    const bool inBatchedWrite = batchedWriteContext.writesAreBatched();

    if (inBatchedWrite) {
        auto operation = MutableOplogEntry::makeUpdateOperation(
            args.nss, args.uuid, args.updateArgs->update, args.updateArgs->criteria);
        operation.setDestinedRecipient(
            shardingWriteRouter.getReshardingDestinedRecipient(args.updateArgs->updatedDoc));
        operation.setFromMigrateIfTrue(args.updateArgs->source == OperationSource::kFromMigrate);
        batchedWriteContext.addBatchedOperation(opCtx, operation);
    } else if (inMultiDocumentTransaction) {
        const bool inRetryableInternalTransaction =
            isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId());

        auto operation = MutableOplogEntry::makeUpdateOperation(
            args.nss, args.uuid, args.updateArgs->update, args.updateArgs->criteria);

        if (inRetryableInternalTransaction) {
            uassert(6462400,
                    str::stream() << "Found a retryable internal transaction on a sharded cluster "
                                  << "executing an update against the collection '" << args.nss
                                  << "' with the 'recordPreImages' option enabled",
                    !args.updateArgs->preImageRecordingEnabledForCollection ||
                        serverGlobalParams.clusterRole == ClusterRole::None);

            operation.setInitializedStatementIds(args.updateArgs->stmtIds);
            if (args.updateArgs->storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage) {
                invariant(args.updateArgs->preImageDoc);
                operation.setPreImage(args.updateArgs->preImageDoc->getOwned());
                operation.setPreImageRecordedForRetryableInternalTransaction();
                if (args.retryableFindAndModifyLocation ==
                        RetryableFindAndModifyLocation::kSideCollection &&
                    !args.updateArgs->preImageRecordingEnabledForCollection) {
                    operation.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
                }
            }
            if (args.updateArgs->storeDocOption ==
                CollectionUpdateArgs::StoreDocOption::PostImage) {
                invariant(!args.updateArgs->updatedDoc.isEmpty());
                operation.setPostImage(args.updateArgs->updatedDoc.getOwned());
                if (args.retryableFindAndModifyLocation ==
                    RetryableFindAndModifyLocation::kSideCollection) {
                    operation.setNeedsRetryImage(repl::RetryImageEnum::kPostImage);
                }
            }
        }

        if (args.updateArgs->preImageRecordingEnabledForCollection) {
            invariant(args.updateArgs->preImageDoc);
            tassert(
                5869402,
                "Change stream pre-image recording to the oplog and to the pre-image collection "
                "requested at the same time",
                !args.updateArgs->changeStreamPreAndPostImagesEnabledForCollection);
            operation.setPreImage(args.updateArgs->preImageDoc->getOwned());
            operation.setChangeStreamPreImageRecordingMode(
                ChangeStreamPreImageRecordingMode::kOplog);
        }

        if (args.updateArgs->changeStreamPreAndPostImagesEnabledForCollection) {
            invariant(args.updateArgs->preImageDoc);
            tassert(
                5869403,
                "Change stream pre-image recording to the oplog and to the pre-image collection "
                "requested at the same time",
                !args.updateArgs->preImageRecordingEnabledForCollection);
            operation.setPreImage(args.updateArgs->preImageDoc->getOwned());
            operation.setChangeStreamPreImageRecordingMode(
                ChangeStreamPreImageRecordingMode::kPreImagesCollection);
        }
        operation.setDestinedRecipient(
            shardingWriteRouter.getReshardingDestinedRecipient(args.updateArgs->updatedDoc));
        operation.setFromMigrateIfTrue(args.updateArgs->source == OperationSource::kFromMigrate);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        MutableOplogEntry oplogEntry;
        oplogEntry.setDestinedRecipient(
            shardingWriteRouter.getReshardingDestinedRecipient(args.updateArgs->updatedDoc));

        if (args.retryableFindAndModifyLocation ==
            RetryableFindAndModifyLocation::kSideCollection) {
            // If we've stored a preImage:
            if (args.updateArgs->storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage &&
                // And we're not writing to a noop entry anyways for
                // `preImageRecordingEnabledForCollection`:
                !args.updateArgs->preImageRecordingEnabledForCollection) {
                oplogEntry.setNeedsRetryImage({repl::RetryImageEnum::kPreImage});
            } else if (args.updateArgs->storeDocOption ==
                       CollectionUpdateArgs::StoreDocOption::PostImage) {
                // Or if we're storing a postImage.
                oplogEntry.setNeedsRetryImage({repl::RetryImageEnum::kPostImage});
            }
        }

        opTime = replLogUpdate(opCtx, args, &oplogEntry);

        if (oplogEntry.getNeedsRetryImage()) {
            // If the oplog entry has `needsRetryImage`, copy the image into image collection.
            const BSONObj& dataImage = [&]() {
                if (oplogEntry.getNeedsRetryImage().get() == repl::RetryImageEnum::kPreImage) {
                    return args.updateArgs->preImageDoc.get();
                } else {
                    return args.updateArgs->updatedDoc;
                }
            }();
            writeToImageCollection(opCtx,
                                   *opCtx->getLogicalSessionId(),
                                   opTime.writeOpTime.getTimestamp(),
                                   oplogEntry.getNeedsRetryImage().get(),
                                   dataImage);
        }

        // Write a pre-image to the change streams pre-images collection when following conditions
        // are met:
        // 1. The collection has 'changeStreamPreAndPostImages' enabled.
        // 2. The node wrote the oplog entry for the corresponding operation.
        // 3. The request to write the pre-image does not come from chunk-migrate event, i.e. source
        //    of the request is not 'fromMigrate'. The 'fromMigrate' events are filtered out by
        //    change streams and storing them in pre-image collection is redundant.
        // 4. a request to update is not on a temporary resharding collection. This update request
        //    does not result in change streams events. Recording pre-images from temporary
        //    resharing collection could result in incorrect pre-image getting recorded due to the
        //    temporary resharding collection not being consistent until writes are blocked (initial
        //    sync mode application).
        if (args.updateArgs->changeStreamPreAndPostImagesEnabledForCollection &&
            !opTime.writeOpTime.isNull() &&
            args.updateArgs->source != OperationSource::kFromMigrate &&
            !args.nss.isTemporaryReshardingCollection()) {
            const auto& preImageDoc = args.updateArgs->preImageDoc;
            tassert(5868600, "PreImage must be set", preImageDoc && !preImageDoc.get().isEmpty());

            ChangeStreamPreImageId id(args.uuid, opTime.writeOpTime.getTimestamp(), 0);
            ChangeStreamPreImage preImage(id, opTime.wallClockTime, preImageDoc.get());
            writeToChangeStreamPreImagesCollection(opCtx, preImage);
        }

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(opTime.writeOpTime);
        sessionTxnRecord.setLastWriteDate(opTime.wallClockTime);
        onWriteOpCompleted(opCtx, args.updateArgs->stmtIds, sessionTxnRecord);
    }

    if (args.nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (args.updateArgs->source != OperationSource::kFromMigrate) {
            shardObserveUpdateOp(opCtx,
                                 args.nss,
                                 args.updateArgs->preImageDoc,
                                 args.updateArgs->updatedDoc,
                                 opTime.writeOpTime,
                                 shardingWriteRouter,
                                 opTime.prePostImageOpTime,
                                 inMultiDocumentTransaction);
        }
    }

    if (args.nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (args.nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, args.nss);
    } else if (args.nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx,
                                                                     args.updateArgs->updatedDoc);
    } else if (args.nss == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
            opCtx, args.updateArgs->updatedDoc["_id"], args.updateArgs->updatedDoc);
    } else if (args.nss.isTimeseriesBucketsCollection()) {
        if (args.updateArgs->source != OperationSource::kTimeseriesInsert) {
            auto& bucketCatalog = BucketCatalog::get(opCtx);
            bucketCatalog.clear(args.updateArgs->updatedDoc["_id"].OID());
        }
    }
}

void OpObserverImpl::aboutToDelete(OperationContext* opCtx,
                                   NamespaceString const& nss,
                                   const UUID& uuid,
                                   BSONObj const& doc) {
    repl::documentKeyDecoration(opCtx).emplace(repl::getDocumentKey(opCtx, nss, doc));

    ShardingWriteRouter shardingWriteRouter(opCtx, nss, Grid::get(opCtx)->catalogCache());

    repl::DurableReplOperation op;
    op.setDestinedRecipient(shardingWriteRouter.getReshardingDestinedRecipient(doc));
    destinedRecipientDecoration(opCtx) = op.getDestinedRecipient();

    shardObserveAboutToDelete(opCtx, nss, doc);

    if (nss.isTimeseriesBucketsCollection()) {
        auto& bucketCatalog = BucketCatalog::get(opCtx);
        bucketCatalog.clear(doc["_id"].OID());
    }
}

void OpObserverImpl::onDelete(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const UUID& uuid,
                              StmtId stmtId,
                              const OplogDeleteEntryArgs& args) {
    auto optDocKey = repl::documentKeyDecoration(opCtx);
    invariant(optDocKey, nss.ns());
    auto& documentKey = optDocKey.get();

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    const bool inBatchedWrite = batchedWriteContext.writesAreBatched();

    OpTimeBundle opTime;
    if (inBatchedWrite) {
        if (nss == NamespaceString::kSessionTransactionsTableNamespace) {
            MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx,
                                                                         documentKey.getId());
        }
        auto operation =
            MutableOplogEntry::makeDeleteOperation(nss, uuid, documentKey.getShardKeyAndId());
        operation.setDestinedRecipient(destinedRecipientDecoration(opCtx));
        operation.setFromMigrateIfTrue(args.fromMigrate);
        batchedWriteContext.addBatchedOperation(opCtx, operation);
    } else if (inMultiDocumentTransaction) {
        const bool inRetryableInternalTransaction =
            isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId());

        tassert(5868700,
                "Attempted a retryable write within a non-retryable multi-document transaction",
                inRetryableInternalTransaction ||
                    args.retryableFindAndModifyLocation == RetryableFindAndModifyLocation::kNone);

        auto operation =
            MutableOplogEntry::makeDeleteOperation(nss, uuid, documentKey.getShardKeyAndId());

        if (inRetryableInternalTransaction) {
            uassert(6462401,
                    str::stream() << "Found a retryable internal transaction on a sharded cluster "
                                  << "executing an delete against the collection '" << nss
                                  << "' with the 'recordPreImages' option enabled",
                    !args.preImageRecordingEnabledForCollection ||
                        serverGlobalParams.clusterRole == ClusterRole::None);

            operation.setInitializedStatementIds({stmtId});
            if (args.retryableFindAndModifyLocation != RetryableFindAndModifyLocation::kNone) {
                tassert(6054000,
                        "Deleted document must be present for pre-image recording",
                        args.deletedDoc);
                operation.setPreImage(args.deletedDoc->getOwned());
                operation.setPreImageRecordedForRetryableInternalTransaction();
                if (args.retryableFindAndModifyLocation ==
                        RetryableFindAndModifyLocation::kSideCollection &&
                    !args.preImageRecordingEnabledForCollection) {
                    operation.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
                }
            }
        }

        if (args.changeStreamPreAndPostImagesEnabledForCollection) {
            tassert(5869400,
                    "Deleted document must be present for pre-image recording",
                    args.deletedDoc);
            tassert(
                5869401,
                "Change stream pre-image recording to the oplog and to the pre-image collection "
                "requested at the same time",
                !args.preImageRecordingEnabledForCollection);
            operation.setPreImage(args.deletedDoc->getOwned());
            operation.setChangeStreamPreImageRecordingMode(
                ChangeStreamPreImageRecordingMode::kPreImagesCollection);
        } else if (args.preImageRecordingEnabledForCollection) {
            tassert(5868701,
                    "Deleted document must be present for pre-image recording",
                    args.deletedDoc);
            operation.setPreImage(args.deletedDoc->getOwned());
            operation.setChangeStreamPreImageRecordingMode(
                ChangeStreamPreImageRecordingMode::kOplog);
        }

        operation.setDestinedRecipient(destinedRecipientDecoration(opCtx));
        operation.setFromMigrateIfTrue(args.fromMigrate);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        MutableOplogEntry oplogEntry;
        boost::optional<BSONObj> deletedDocForOplog = boost::none;

        if (args.retryableFindAndModifyLocation == RetryableFindAndModifyLocation::kOplog ||
            args.preImageRecordingEnabledForCollection) {
            tassert(5868702,
                    "Deleted document must be present for pre-image recording",
                    args.deletedDoc);
            deletedDocForOplog = {*(args.deletedDoc)};
        } else if (args.retryableFindAndModifyLocation ==
                   RetryableFindAndModifyLocation::kSideCollection) {
            tassert(5868703,
                    "Deleted document must be present for pre-image recording",
                    args.deletedDoc);
            invariant(opCtx->getTxnNumber());

            oplogEntry.setNeedsRetryImage({repl::RetryImageEnum::kPreImage});
            if (!args.oplogSlots.empty()) {
                oplogEntry.setOpTime(args.oplogSlots.back());
            }
        }
        opTime = replLogDelete(
            opCtx, nss, &oplogEntry, uuid, stmtId, args.fromMigrate, deletedDocForOplog);

        if (oplogEntry.getNeedsRetryImage()) {
            writeToImageCollection(opCtx,
                                   *opCtx->getLogicalSessionId(),
                                   opTime.writeOpTime.getTimestamp(),
                                   repl::RetryImageEnum::kPreImage,
                                   *(args.deletedDoc));
        }

        // Write a pre-image to the change streams pre-images collection when following conditions
        // are met:
        // 1. The collection has 'changeStreamPreAndPostImages' enabled.
        // 2. The node wrote the oplog entry for the corresponding operation.
        // 3. The request to write the pre-image does not come from chunk-migrate event, i.e. source
        //    of the request is not 'fromMigrate'. The 'fromMigrate' events are filtered out by
        //    change streams and storing them in pre-image collection is redundant.
        // 4. a request to delete is not on a temporary resharding collection. This delete request
        //    does not result in change streams events. Recording pre-images from temporary
        //    resharing collection could result in incorrect pre-image getting recorded due to the
        //    temporary resharding collection not being consistent until writes are blocked (initial
        //    sync mode application).
        if (args.changeStreamPreAndPostImagesEnabledForCollection && !opTime.writeOpTime.isNull() &&
            !args.fromMigrate && !nss.isTemporaryReshardingCollection()) {
            tassert(5868704, "Deleted document must be set", args.deletedDoc);

            ChangeStreamPreImageId id(uuid, opTime.writeOpTime.getTimestamp(), 0);
            ChangeStreamPreImage preImage(id, opTime.wallClockTime, *args.deletedDoc);
            writeToChangeStreamPreImagesCollection(opCtx, preImage);
        }

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(opTime.writeOpTime);
        sessionTxnRecord.setLastWriteDate(opTime.wallClockTime);
        onWriteOpCompleted(opCtx, std::vector<StmtId>{stmtId}, sessionTxnRecord);
    }

    if (nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!args.fromMigrate) {
            ShardingWriteRouter shardingWriteRouter(opCtx, nss, Grid::get(opCtx)->catalogCache());
            shardObserveDeleteOp(opCtx,
                                 nss,
                                 documentKey.getShardKeyAndId(),
                                 opTime.writeOpTime,
                                 shardingWriteRouter,
                                 opTime.prePostImageOpTime,
                                 inMultiDocumentTransaction);
        }
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx, documentKey.getId());
    } else if (nss == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
            opCtx, documentKey.getId().firstElement(), boost::none);
    }
}

void OpObserverImpl::onInternalOpMessage(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& uuid,
    const BSONObj& msgObj,
    const boost::optional<BSONObj> o2MsgObj,
    const boost::optional<repl::OpTime> preImageOpTime,
    const boost::optional<repl::OpTime> postImageOpTime,
    const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
    const boost::optional<OplogSlot> slot) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(msgObj);
    oplogEntry.setObject2(o2MsgObj);
    oplogEntry.setPreImageOpTime(preImageOpTime);
    oplogEntry.setPostImageOpTime(postImageOpTime);
    oplogEntry.setPrevWriteOpTimeInTransaction(prevWriteOpTimeInTransaction);
    if (slot) {
        oplogEntry.setOpTime(*slot);
    }
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onCreateCollection(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex,
                                        const OplogSlot& createOpTime,
                                        bool fromMigrate) {
    // do not replicate system.profile modifications
    if (collectionName.isSystemDotProfile()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    if (inMultiDocumentTransaction) {
        auto operation = MutableOplogEntry::makeCreateCommand(collectionName, options, idIndex);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(collectionName.getCommandNS());
        oplogEntry.setUuid(options.uuid);
        oplogEntry.setObject(
            MutableOplogEntry::makeCreateCollCmdObj(collectionName, options, idIndex));
        oplogEntry.setOpTime(createOpTime);
        oplogEntry.setFromMigrateIfTrue(fromMigrate);
        logOperation(opCtx, &oplogEntry);
    }
}

void OpObserverImpl::onCollMod(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& uuid,
                               const BSONObj& collModCmd,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<IndexCollModInfo> indexInfo) {

    if (!nss.isSystemDotProfile()) {
        // do not replicate system.profile modifications

        // Create the 'o2' field object. We save the old collection metadata and TTL expiration.
        BSONObjBuilder o2Builder;
        o2Builder.append("collectionOptions_old", oldCollOptions.toBSON());
        if (indexInfo) {
            BSONObjBuilder oldIndexOptions;
            if (indexInfo->oldExpireAfterSeconds) {
                auto oldExpireAfterSeconds =
                    durationCount<Seconds>(indexInfo->oldExpireAfterSeconds.get());
                oldIndexOptions.append("expireAfterSeconds", oldExpireAfterSeconds);
            }
            if (indexInfo->oldHidden) {
                auto oldHidden = indexInfo->oldHidden.get();
                oldIndexOptions.append("hidden", oldHidden);
            }
            if (indexInfo->oldPrepareUnique) {
                auto oldPrepareUnique = indexInfo->oldPrepareUnique.get();
                oldIndexOptions.append("prepareUnique", oldPrepareUnique);
            }
            o2Builder.append("indexOptions_old", oldIndexOptions.obj());
        }

        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(nss.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(repl::makeCollModCmdObj(collModCmd, oldCollOptions, indexInfo));
        oplogEntry.setObject2(o2Builder.done());
        logOperation(opCtx, &oplogEntry);
    }

    // Make sure the UUID values in the Collection metadata, the Collection object, and the UUID
    // catalog are all present and equal.
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, nss.dbName());
    // Some unit tests call the op observer on an unregistered Database.
    if (!db) {
        return;
    }
    const CollectionPtr& coll =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);

    invariant(coll->uuid() == uuid);
}

void OpObserverImpl::onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss({dbName, "$cmd"});
    oplogEntry.setObject(BSON("dropDatabase" << 1));
    logOperation(opCtx, &oplogEntry);

    uassert(
        50714, "dropping the admin database is not allowed.", dbName != NamespaceString::kAdminDb);

    if (dbName == NamespaceString::kSessionTransactionsTableNamespace.db()) {
        MongoDSessionCatalog::invalidateAllSessions(opCtx);
    }

    BucketCatalog::get(opCtx).clear(dbName);
}

repl::OpTime OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const UUID& uuid,
                                              std::uint64_t numRecords,
                                              CollectionDropType dropType) {
    return onDropCollection(
        opCtx, collectionName, uuid, numRecords, dropType, false /* markFromMigrate */);
}

repl::OpTime OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const UUID& uuid,
                                              std::uint64_t numRecords,
                                              const CollectionDropType dropType,
                                              bool markFromMigrate) {
    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications.
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(collectionName.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setFromMigrateIfTrue(markFromMigrate);
        oplogEntry.setObject(BSON("drop" << collectionName.coll()));
        oplogEntry.setObject2(makeObject2ForDropOrRename(numRecords));
        logOperation(opCtx, &oplogEntry);
    }

    uassert(50715,
            "dropping the server configuration collection (admin.system.version) is not allowed.",
            collectionName != NamespaceString::kServerConfigurationNamespace);

    if (collectionName.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onSystemViewsCollectionDrop(opCtx, collectionName);
    } else if (collectionName == NamespaceString::kSessionTransactionsTableNamespace) {
        // Disallow this drop if there are currently prepared transactions.
        const auto sessionCatalog = SessionCatalog::get(opCtx);
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        bool noPreparedTxns = true;
        sessionCatalog->scanSessions(matcherAllSessions, [&](const ObservableSession& session) {
            auto txnParticipant = TransactionParticipant::get(session);
            if (txnParticipant.transactionIsPrepared()) {
                noPreparedTxns = false;
            }
        });
        uassert(4852500,
                "Unable to drop transactions table (config.transactions) while prepared "
                "transactions are present.",
                noPreparedTxns);

        MongoDSessionCatalog::invalidateAllSessions(opCtx);
    } else if (collectionName == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).invalidate();
    } else if (collectionName.isTimeseriesBucketsCollection()) {
        BucketCatalog::get(opCtx).clear(collectionName.getTimeseriesViewNamespace());
    }

    return {};
}

void OpObserverImpl::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(BSON("dropIndexes" << nss.coll() << "index" << indexName));
    oplogEntry.setObject2(indexInfo);
    logOperation(opCtx, &oplogEntry);
}

repl::OpTime OpObserverImpl::preRenameCollection(OperationContext* const opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 const UUID& uuid,
                                                 const boost::optional<UUID>& dropTargetUUID,
                                                 std::uint64_t numRecords,
                                                 bool stayTemp) {
    return preRenameCollection(opCtx,
                               fromCollection,
                               toCollection,
                               uuid,
                               dropTargetUUID,
                               numRecords,
                               stayTemp,
                               false /* markFromMigrate */);
}

repl::OpTime OpObserverImpl::preRenameCollection(OperationContext* const opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 const UUID& uuid,
                                                 const boost::optional<UUID>& dropTargetUUID,
                                                 std::uint64_t numRecords,
                                                 bool stayTemp,
                                                 bool markFromMigrate) {
    BSONObjBuilder builder;
    builder.append("renameCollection", fromCollection.ns());
    builder.append("to", toCollection.ns());
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(fromCollection.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setFromMigrateIfTrue(markFromMigrate);
    oplogEntry.setObject(builder.done());
    if (dropTargetUUID)
        oplogEntry.setObject2(makeObject2ForDropOrRename(numRecords));
    logOperation(opCtx, &oplogEntry);

    return {};
}

void OpObserverImpl::postRenameCollection(OperationContext* const opCtx,
                                          const NamespaceString& fromCollection,
                                          const NamespaceString& toCollection,
                                          const UUID& uuid,
                                          const boost::optional<UUID>& dropTargetUUID,
                                          bool stayTemp) {
    if (fromCollection.isSystemDotViews())
        DurableViewCatalog::onExternalChange(opCtx, fromCollection);
    if (toCollection.isSystemDotViews())
        DurableViewCatalog::onExternalChange(opCtx, toCollection);
}

void OpObserverImpl::onRenameCollection(OperationContext* const opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        const UUID& uuid,
                                        const boost::optional<UUID>& dropTargetUUID,
                                        std::uint64_t numRecords,
                                        bool stayTemp) {
    onRenameCollection(opCtx,
                       fromCollection,
                       toCollection,
                       uuid,
                       dropTargetUUID,
                       numRecords,
                       stayTemp,
                       false /* markFromMigrate */);
}

void OpObserverImpl::onRenameCollection(OperationContext* const opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        const UUID& uuid,
                                        const boost::optional<UUID>& dropTargetUUID,
                                        std::uint64_t numRecords,
                                        bool stayTemp,
                                        bool markFromMigrate) {
    preRenameCollection(opCtx,
                        fromCollection,
                        toCollection,
                        uuid,
                        dropTargetUUID,
                        numRecords,
                        stayTemp,
                        markFromMigrate);
    postRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
}

void OpObserverImpl::onImportCollection(OperationContext* opCtx,
                                        const UUID& importUUID,
                                        const NamespaceString& nss,
                                        long long numRecords,
                                        long long dataSize,
                                        const BSONObj& catalogEntry,
                                        const BSONObj& storageMetadata,
                                        bool isDryRun) {
    ImportCollectionOplogEntry importCollection(
        nss, importUUID, numRecords, dataSize, catalogEntry, storageMetadata, isDryRun);

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setObject(importCollection.toBSON());
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss({dbName, "$cmd"});
    oplogEntry.setObject(applyOpCmd);
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onEmptyCapped(OperationContext* opCtx,
                                   const NamespaceString& collectionName,
                                   const UUID& uuid) {
    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(collectionName.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(BSON("emptycapped" << collectionName.coll()));
        logOperation(opCtx, &oplogEntry);
    }
}

namespace {

/**
 * Writes pre-images for update/replace/delete operations packed into a single "applyOps" entry to
 * the change stream pre-images collection if required. The operations are defined by sequence
 * ['stmtBegin', 'stmtEnd'). 'applyOpsTimestamp' and 'operationTime' are the timestamp and the wall
 * clock time, respectively, of the "applyOps" entry. A pre-image is recorded for an operation only
 * if pre-images are enabled for the collection the operation is issued on.
 */
void writeChangeStreamPreImagesForApplyOpsEntries(
    OperationContext* opCtx,
    std::vector<repl::ReplOperation>::const_iterator stmtBegin,
    std::vector<repl::ReplOperation>::const_iterator stmtEnd,
    Timestamp applyOpsTimestamp,
    Date_t operationTime) {
    int64_t applyOpsIndex{0};
    for (auto stmtIterator = stmtBegin; stmtIterator != stmtEnd; ++stmtIterator) {
        auto& operation = *stmtIterator;
        if (operation.isChangeStreamPreImageRecordedInPreImagesCollection() &&
            !operation.getNss().isTemporaryReshardingCollection()) {
            invariant(operation.getUuid());
            invariant(!operation.getPreImage().isEmpty());
            writeToChangeStreamPreImagesCollection(
                opCtx,
                ChangeStreamPreImage{
                    ChangeStreamPreImageId{*operation.getUuid(), applyOpsTimestamp, applyOpsIndex},
                    operationTime,
                    operation.getPreImage()});
        }
        ++applyOpsIndex;
    }
}

/**
 * Returns operations that can fit into an "applyOps" entry. The returned operations are
 * serialized to BSON. The operations are given by range ['operationsBegin',
 * 'operationsEnd').
 * Multi-document transactions follow the following constraints for fitting the operations: (1) the
 * resulting "applyOps" entry shouldn't exceed the 16MB limit, unless only one operation is
 * allocated to it; (2) the number of operations is not larger than the maximum number of
 * transaction statements allowed in one entry as defined by
 * 'gMaxNumberOfTransactionOperationsInSingleOplogEntry'. Batched writes (WUOWs that pack writes
 * into a single applyOps outside of a multi-doc transaction) are exempt from the constraints above.
 * If the operations cannot be packed into a single applyOps that's within the BSON size limit
 * (16MB), the batched write will fail with TransactionTooLarge.
 */
std::vector<BSONObj> packOperationsIntoApplyOps(
    OperationContext* opCtx,
    std::vector<repl::ReplOperation>::const_iterator operationsBegin,
    std::vector<repl::ReplOperation>::const_iterator operationsEnd) {
    // Conservative BSON array element overhead assuming maximum 6 digit array index.
    constexpr size_t kBSONArrayElementOverhead{8};
    tassert(6278503,
            "gMaxNumberOfTransactionOperationsInSingleOplogEntry should be positive number",
            gMaxNumberOfTransactionOperationsInSingleOplogEntry > 0);
    std::vector<BSONObj> operations;
    size_t totalOperationsSize{0};
    for (auto operationIter = operationsBegin; operationIter != operationsEnd; ++operationIter) {
        const auto& operation = *operationIter;

        if (TransactionParticipant::get(opCtx)) {
            // Stop packing when either number of transaction operations is reached, or when the
            // next one would make the total size of operations larger than the maximum BSON Object
            // User Size. We rely on the headroom between BSONObjMaxUserSize and
            // BSONObjMaxInternalSize to cover the BSON overhead and the other "applyOps" entry
            // fields. But if a single operation in the set exceeds BSONObjMaxUserSize, we still fit
            // it, as a single max-length operation should be able to be packed into an "applyOps"
            // entry.
            if (operations.size() ==
                    static_cast<size_t>(gMaxNumberOfTransactionOperationsInSingleOplogEntry) ||
                (operations.size() > 0 &&
                 (totalOperationsSize + DurableOplogEntry::getDurableReplOperationSize(operation) >
                  BSONObjMaxUserSize))) {
                break;
            }
        } else {
            // This a batched write, so we don't break the batch into multiple applyOps. It is the
            // reponsibility of the caller to generate a batch that fits within a single applyOps.
            // If the batch doesn't fit within an applyOps, we throw a TransactionTooLarge later
            // on when serializing to BSON.
        }
        auto serializedOperation = operation.toBSON();
        totalOperationsSize += static_cast<size_t>(serializedOperation.objsize());

        // Add BSON array element overhead since operations will ultimately be packed into BSON
        // array.
        totalOperationsSize += kBSONArrayElementOverhead;

        operations.emplace_back(std::move(serializedOperation));
    }
    return operations;
}

/**
 * Returns oplog slots to be used for "applyOps" oplog entries, BSON serialized operations, their
 * assignments to "applyOps" entries, and oplog slots to be used for writing pre- and post- image
 * oplog entries for the transaction consisting of 'operations'. Allocates oplog slots from
 * 'oplogSlots'. The 'numberOfPrePostImagesToWrite' is the number of CRUD operations that have a
 * pre-image to write as a noop oplog entry. The 'prepare' indicates if the function is called when
 * preparing a transaction.
 */
OpObserver::ApplyOpsOplogSlotAndOperationAssignment
getApplyOpsOplogSlotAndOperationAssignmentForTransaction(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& oplogSlots,
    size_t numberOfPrePostImagesToWrite,
    bool prepare,
    std::vector<repl::ReplOperation>& operations) {
    if (operations.empty()) {
        return {{}, {}, 0 /*numberOfOplogSlotsUsed*/};
    }
    tassert(6278504, "Insufficient number of oplogSlots", operations.size() <= oplogSlots.size());

    std::vector<OplogSlot> prePostImageOplogEntryOplogSlots;
    std::vector<OpObserver::ApplyOpsOplogSlotAndOperationAssignment::ApplyOpsEntry> applyOpsEntries;
    const auto operationCount = operations.size();
    auto oplogSlotIter = oplogSlots.begin();
    auto getNextOplogSlot = [&]() {
        tassert(6278505, "Unexpected end of oplog slot vector", oplogSlotIter != oplogSlots.end());
        return *oplogSlotIter++;
    };

    auto isMigratingTenant = [&opCtx]() {
        return static_cast<bool>(repl::tenantMigrationRecipientInfo(opCtx));
    };

    // We never want to store pre-images or post-images when we're migrating oplog entries from
    // another replica set.
    if (numberOfPrePostImagesToWrite > 0 && !isMigratingTenant()) {
        for (size_t operationIdx = 0; operationIdx < operationCount; ++operationIdx) {
            auto& statement = operations[operationIdx];
            if (statement.isChangeStreamPreImageRecordedInOplog() ||
                (statement.isPreImageRecordedForRetryableInternalTransaction() &&
                 statement.getNeedsRetryImage() != repl::RetryImageEnum::kPreImage)) {
                tassert(6278506, "Expected a pre-image", !statement.getPreImage().isEmpty());
                auto oplogSlot = getNextOplogSlot();
                prePostImageOplogEntryOplogSlots.push_back(oplogSlot);
                statement.setPreImageOpTime(oplogSlot);
            }
            if (!statement.getPostImage().isEmpty() &&
                statement.getNeedsRetryImage() != repl::RetryImageEnum::kPostImage) {
                auto oplogSlot = getNextOplogSlot();
                prePostImageOplogEntryOplogSlots.push_back(oplogSlot);
                statement.setPostImageOpTime(oplogSlot);
            }
        }
    }

    auto hasNeedsRetryImage = [](const repl::ReplOperation& operation) {
        return static_cast<bool>(operation.getNeedsRetryImage());
    };

    // Assign operations to "applyOps" entries.
    for (auto operationIt = operations.begin(); operationIt != operations.end();) {
        auto applyOpsOperations = packOperationsIntoApplyOps(opCtx, operationIt, operations.end());
        const auto opCountWithNeedsRetryImage =
            std::count_if(operationIt, operationIt + applyOpsOperations.size(), hasNeedsRetryImage);
        if (opCountWithNeedsRetryImage > 0) {
            // Reserve a slot for a forged no-op entry.
            getNextOplogSlot();
        }
        operationIt += applyOpsOperations.size();
        applyOpsEntries.emplace_back(
            OpObserver::ApplyOpsOplogSlotAndOperationAssignment::ApplyOpsEntry{
                getNextOplogSlot(), std::move(applyOpsOperations)});
    }

    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    tassert(6501800,
            "batched writes must generate a single applyOps entry",
            !batchedWriteContext.writesAreBatched() || applyOpsEntries.size() == 1);

    // In the special case of writing the implicit 'prepare' oplog entry, we use the last reserved
    // oplog slot. This may mean we skipped over some reserved slots, but there's no harm in that.
    if (prepare) {
        applyOpsEntries.back().oplogSlot = oplogSlots.back();
    }
    return {std::move(prePostImageOplogEntryOplogSlots),
            std::move(applyOpsEntries),
            static_cast<size_t>(oplogSlotIter - oplogSlots.begin())};
}

/**
 * Writes change stream pre-images for transaction 'operations'. The 'applyOpsOperationAssignment'
 * contains a representation of "applyOps" entries to be written for the transaction. The
 * 'operationTime' is wall clock time of the operations used for the pre-image documents.
 */
void writeChangeStreamPreImagesForTransaction(
    OperationContext* opCtx,
    const std::vector<repl::ReplOperation>& operations,
    const OpObserver::ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    Date_t operationTime) {
    // This function must be called from an outer WriteUnitOfWork in order to be rolled back upon
    // reaching the exception.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    auto applyOpsEntriesIt = applyOpsOperationAssignment.applyOpsEntries.begin();
    for (auto operationIter = operations.begin(); operationIter != operations.end();) {
        tassert(6278507,
                "Unexpected end of applyOps entries vector",
                applyOpsEntriesIt != applyOpsOperationAssignment.applyOpsEntries.end());
        const auto& applyOpsEntry = *applyOpsEntriesIt++;
        const auto operationSequenceEnd = operationIter + applyOpsEntry.operations.size();
        writeChangeStreamPreImagesForApplyOpsEntries(opCtx,
                                                     operationIter,
                                                     operationSequenceEnd,
                                                     applyOpsEntry.oplogSlot.getTimestamp(),
                                                     operationTime);
        operationIter = operationSequenceEnd;
    }
}

// Accepts an empty BSON builder and appends the given transaction statements to an 'applyOps' array
// field (and their corresponding statement ids to 'stmtIdsWritten'). The transaction statements are
// represented as range ['stmtBegin', 'stmtEnd') and BSON serialized objects 'operations'. If any of
// the statements has a pre-image or post-image that needs to be stored in the image collection,
// stores it to 'imageToWrite'.
void packTransactionStatementsForApplyOps(
    BSONObjBuilder* applyOpsBuilder,
    std::vector<StmtId>* stmtIdsWritten,
    boost::optional<std::pair<repl::RetryImageEnum, BSONObj>>* imageToWrite,
    std::vector<repl::ReplOperation>::iterator stmtBegin,
    std::vector<repl::ReplOperation>::iterator stmtEnd,
    const std::vector<BSONObj>& operations) {
    tassert(6278508,
            "Number of operations does not match the number of transaction statements",
            operations.size() == static_cast<size_t>(stmtEnd - stmtBegin));
    auto setImageToWrite = [&](const repl::ReplOperation& stmt) {
        uassert(6054001,
                str::stream() << NamespaceString::kConfigImagesNamespace
                              << " can only store the pre or post image of one "
                                 "findAndModify operation for each "
                                 "transaction",
                !(*imageToWrite));
        switch (*stmt.getNeedsRetryImage()) {
            case repl::RetryImageEnum::kPreImage: {
                invariant(!stmt.getPreImage().isEmpty());
                *imageToWrite = std::make_pair(repl::RetryImageEnum::kPreImage, stmt.getPreImage());
                break;
            }
            case repl::RetryImageEnum::kPostImage: {
                invariant(!stmt.getPostImage().isEmpty());
                *imageToWrite =
                    std::make_pair(repl::RetryImageEnum::kPostImage, stmt.getPostImage());
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    };

    std::vector<repl::ReplOperation>::iterator stmtIter;
    auto operationsIter = operations.begin();
    BSONArrayBuilder opsArray(applyOpsBuilder->subarrayStart("applyOps"_sd));
    for (stmtIter = stmtBegin; stmtIter != stmtEnd; stmtIter++) {
        const auto& stmt = *stmtIter;
        opsArray.append(*operationsIter++);
        const auto stmtIds = stmt.getStatementIds();
        stmtIdsWritten->insert(stmtIdsWritten->end(), stmtIds.begin(), stmtIds.end());
        if (stmt.getNeedsRetryImage()) {
            setImageToWrite(stmt);
        }
    }
    try {
        // BSONArrayBuilder will throw a BSONObjectTooLarge exception if we exceeded the max BSON
        // size.
        opsArray.done();
    } catch (const AssertionException& e) {
        // Change the error code to TransactionTooLarge if it is BSONObjectTooLarge.
        uassert(ErrorCodes::TransactionTooLarge,
                e.reason(),
                e.code() != ErrorCodes::BSONObjectTooLarge);
        throw;
    }
}

// Logs one applyOps entry on a prepared transaction, or an unprepared transaction's commit, or on
// committing a WUOW that is not necessarily tied to a multi-document transaction. It may update the
// transactions table on multi-document transactions. Assumes that the given BSON builder object
// already has  an 'applyOps' field appended pointing to the desired array of ops i.e. { "applyOps"
// : [op1, op2, ...] }
//
// @param txnState the 'state' field of the transaction table entry update.  @param startOpTime the
// optime of the 'startOpTime' field of the transaction table entry update. If boost::none, no
// 'startOpTime' field will be included in the new transaction table entry. Only meaningful if
// 'updateTxnTable' is true.  @param updateTxnTable determines whether the transactions table will
// updated after the oplog entry is written.
//
// Returns the optime of the written oplog entry.
OpTimeBundle logApplyOps(OperationContext* opCtx,
                         MutableOplogEntry* oplogEntry,
                         boost::optional<DurableTxnStateEnum> txnState,
                         boost::optional<repl::OpTime> startOpTime,
                         std::vector<StmtId> stmtIdsWritten,
                         const bool updateTxnTable) {
    if (!stmtIdsWritten.empty()) {
        invariant(isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId()));
    }

    const auto txnRetryCounter = opCtx->getTxnRetryCounter();

    invariant(bool(txnRetryCounter) == bool(TransactionParticipant::get(opCtx)));

    oplogEntry->setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry->setNss({"admin", "$cmd"});
    // Batched writes (that is, WUOWs with 'groupOplogEntries') are not associated with a txnNumber,
    // so do not emit an lsid either.
    oplogEntry->setSessionId(opCtx->getTxnNumber() ? opCtx->getLogicalSessionId() : boost::none);
    oplogEntry->setTxnNumber(opCtx->getTxnNumber());
    if (txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        oplogEntry->getOperationSessionInfo().setTxnRetryCounter(*txnRetryCounter);
    }

    try {
        OpTimeBundle times;
        times.writeOpTime = logOperation(opCtx, oplogEntry, false /*assignWallClockTime*/);
        times.wallClockTime = oplogEntry->getWallClockTime();
        if (updateTxnTable) {
            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setLastWriteOpTime(times.writeOpTime);
            sessionTxnRecord.setLastWriteDate(times.wallClockTime);
            sessionTxnRecord.setState(txnState);
            sessionTxnRecord.setStartOpTime(startOpTime);
            if (txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
                sessionTxnRecord.setTxnRetryCounter(*txnRetryCounter);
            }
            onWriteOpCompleted(opCtx, std::move(stmtIdsWritten), sessionTxnRecord);
        }
        return times;
    } catch (const AssertionException& e) {
        // Change the error code to TransactionTooLarge if it is BSONObjectTooLarge.
        uassert(ErrorCodes::TransactionTooLarge,
                e.reason(),
                e.code() != ErrorCodes::BSONObjectTooLarge);
        throw;
    }
    MONGO_UNREACHABLE;
}

// Logs applyOps oplog entries for preparing a transaction, committing an unprepared
// transaction, or committing a WUOW that is not necessarily related to a multi-document
// transaction. This includes the in-progress 'partialTxn' oplog entries followed by the implicit
// prepare or commit entry. If the 'prepare' argument is true, it will log entries for a prepared
// transaction. Otherwise, it logs entries for an unprepared transaction. The total number of oplog
// entries written will be <= the size of the given 'stmts' vector, and will depend on how many
// transaction statements are given, the data size of each statement, and the
// 'maxNumberOfTransactionOperationsInSingleOplogEntry' server parameter.
//
// This function expects that the size of 'oplogSlots' be at least as big as the size of 'stmts' in
// the worst case, where each operation requires an applyOps entry of its own. If there are more
// oplog slots than applyOps operations are written, the number of oplog slots corresponding to the
// number of applyOps written will be used. It also expects that the vector of given statements is
// non-empty.
//
// The 'applyOpsOperationAssignment' contains BSON serialized transaction statements, their
// assigment to "applyOps" oplog entries, and oplog slots to be used for writing pre- and post-
// image oplog entries for a transaction.
//
// In the case of writing entries for a prepared transaction, the last oplog entry (i.e. the
// implicit prepare) will always be written using the last oplog slot given, even if this means
// skipping over some reserved slots.
//
// The number of oplog entries written is returned.
int logOplogEntries(
    OperationContext* opCtx,
    std::vector<repl::ReplOperation>* stmts,
    const std::vector<OplogSlot>& oplogSlots,
    const OpObserver::ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    boost::optional<ImageBundle>* prePostImageToWriteToImageCollection,
    size_t numberOfPrePostImagesToWrite,
    bool prepare,
    Date_t wallClockTime) {
    invariant(!stmts->empty());

    // Storage transaction commit is the last place inside a transaction that can throw an
    // exception. In order to safely allow exceptions to be thrown at that point, this function must
    // be called from an outer WriteUnitOfWork in order to be rolled back upon reaching the
    // exception.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    const auto txnParticipant = TransactionParticipant::get(opCtx);
    OpTimeBundle prevWriteOpTime;

    // Writes to the oplog only require a Global intent lock. Guaranteed by
    // OplogSlotReserver.
    invariant(opCtx->lockState()->isWriteLocked());

    if (txnParticipant) {
        prevWriteOpTime.writeOpTime = txnParticipant.getLastWriteOpTime();
    }
    auto currPrePostImageOplogEntryOplogSlot =
        applyOpsOperationAssignment.prePostImageOplogEntryOplogSlots.begin();

    // We never want to store pre-images or post-images when we're migrating oplog entries from
    // another replica set.
    const auto& migrationRecipientInfo = repl::tenantMigrationRecipientInfo(opCtx);

    auto logPrePostImageNoopEntry = [&](const repl::ReplOperation& statement,
                                        const BSONObj& imageDoc) {
        auto slot = *currPrePostImageOplogEntryOplogSlot;
        ++currPrePostImageOplogEntryOplogSlot;

        MutableOplogEntry imageEntry;
        imageEntry.setSessionId(*opCtx->getLogicalSessionId());
        imageEntry.setTxnNumber(*opCtx->getTxnNumber());
        imageEntry.setStatementIds(statement.getStatementIds());
        imageEntry.setOpType(repl::OpTypeEnum::kNoop);
        imageEntry.setObject(imageDoc);
        imageEntry.setNss(statement.getNss());
        imageEntry.setUuid(statement.getUuid());
        imageEntry.setOpTime(slot);
        imageEntry.setDestinedRecipient(statement.getDestinedRecipient());

        logOperation(opCtx, &imageEntry);
    };

    if (numberOfPrePostImagesToWrite > 0 && !migrationRecipientInfo) {
        for (auto& statement : *stmts) {
            if (statement.isChangeStreamPreImageRecordedInOplog() ||
                (statement.isPreImageRecordedForRetryableInternalTransaction() &&
                 statement.getNeedsRetryImage() != repl::RetryImageEnum::kPreImage)) {
                invariant(!statement.getPreImage().isEmpty());

                // Note that 'needsRetryImage' stores the image kind that needs to stored in the
                // image collection. Therefore, when 'needsRetryImage' is equal to kPreImage, the
                // pre-image will be written to the image collection (after all the applyOps oplog
                // entries are written).
                logPrePostImageNoopEntry(statement, statement.getPreImage());
            }
            if (!statement.getPostImage().isEmpty() &&
                statement.getNeedsRetryImage() != repl::RetryImageEnum::kPostImage) {
                // Likewise, when 'needsRetryImage' is equal to kPostImage, the post-image will be
                // written to the image collection (after all the applyOps oplog entries are
                // written).
                logPrePostImageNoopEntry(statement, statement.getPostImage());
            }
        }
    }

    // Stores the statement ids of all write statements in the transaction.
    std::vector<StmtId> stmtIdsWritten;

    // At the beginning of each loop iteration below, 'stmtsIter' will always point to the
    // first statement of the sequence of remaining, unpacked transaction statements. If all
    // statements have been packed, it should point to stmts.end(), which is the loop's
    // termination condition.
    auto stmtsIter = stmts->begin();
    auto applyOpsIter = applyOpsOperationAssignment.applyOpsEntries.begin();
    while (stmtsIter != stmts->end()) {
        tassert(6278509,
                "Not enough \"applyOps\" entries",
                applyOpsIter != applyOpsOperationAssignment.applyOpsEntries.end());
        auto& applyOpsEntry = *applyOpsIter++;
        BSONObjBuilder applyOpsBuilder;
        boost::optional<std::pair<repl::RetryImageEnum, BSONObj>> imageToWrite;

        const auto nextStmt = stmtsIter + applyOpsEntry.operations.size();
        packTransactionStatementsForApplyOps(&applyOpsBuilder,
                                             &stmtIdsWritten,
                                             &imageToWrite,
                                             stmtsIter,
                                             nextStmt,
                                             applyOpsEntry.operations);

        // If we packed the last op, then the next oplog entry we log should be the implicit
        // commit or implicit prepare, i.e. we omit the 'partialTxn' field.
        auto firstOp = stmtsIter == stmts->begin();
        auto lastOp = nextStmt == stmts->end();

        auto implicitCommit = lastOp && !prepare;
        auto implicitPrepare = lastOp && prepare;
        auto isPartialTxn = !lastOp;

        if (imageToWrite) {
            uassert(6054002,
                    str::stream() << NamespaceString::kConfigImagesNamespace
                                  << " can only store the pre or post image of one "
                                     "findAndModify operation for each "
                                     "transaction",
                    !(*prePostImageToWriteToImageCollection));
        }

        if (isPartialTxn || (imageToWrite && !prepare)) {
            // Partial transactions and unprepared transactions with pre or post image stored in the
            // image collection create/reserve multiple oplog entries in the same WriteUnitOfWork.
            // Because of this, such transactions will set multiple timestamps, violating the
            // multi timestamp constraint. It's safe to ignore the multi timestamp constraints here
            // as additional rollback logic is in place for this case.
            opCtx->recoveryUnit()->ignoreAllMultiTimestampConstraints();
        }

        // A 'prepare' oplog entry should never include a 'partialTxn' field.
        invariant(!(isPartialTxn && implicitPrepare));
        if (implicitPrepare) {
            applyOpsBuilder.append("prepare", true);
        }
        if (isPartialTxn) {
            applyOpsBuilder.append("partialTxn", true);
        }

        // The 'count' field gives the total number of individual operations in the
        // transaction, and is included on a non-initial implicit commit or prepare entry.
        if (lastOp && !firstOp) {
            applyOpsBuilder.append("count", static_cast<long long>(stmts->size()));
        }

        // For both prepared and unprepared transactions (but not for batched writes) update the
        // transactions table on the first and last op.
        auto updateTxnTable = txnParticipant && (firstOp || lastOp);

        // The first optime of the transaction is always the first oplog slot, except in the
        // case of a single prepare oplog entry.
        auto firstOpTimeOfTxn =
            (implicitPrepare && firstOp) ? oplogSlots.back() : oplogSlots.front();

        // We always write the startOpTime field, which is the first optime of the
        // transaction, except when transitioning to 'committed' state, in which it should
        // no longer be set.
        auto startOpTime = boost::make_optional(!implicitCommit, firstOpTimeOfTxn);

        MutableOplogEntry oplogEntry;
        oplogEntry.setOpTime(applyOpsEntry.oplogSlot);
        if (txnParticipant) {
            oplogEntry.setPrevWriteOpTimeInTransaction(prevWriteOpTime.writeOpTime);
        }
        oplogEntry.setWallClockTime(wallClockTime);
        oplogEntry.setObject(applyOpsBuilder.done());
        auto txnState = isPartialTxn
            ? DurableTxnStateEnum::kInProgress
            : (implicitPrepare ? DurableTxnStateEnum::kPrepared : DurableTxnStateEnum::kCommitted);
        prevWriteOpTime = logApplyOps(opCtx,
                                      &oplogEntry,
                                      txnState,
                                      startOpTime,
                                      (lastOp ? std::move(stmtIdsWritten) : std::vector<StmtId>{}),
                                      updateTxnTable);

        hangAfterLoggingApplyOpsForTransaction.pauseWhileSet();

        if (imageToWrite) {
            invariant(!(*prePostImageToWriteToImageCollection));
            *prePostImageToWriteToImageCollection =
                ImageBundle{imageToWrite->first,
                            imageToWrite->second,
                            prevWriteOpTime.writeOpTime.getTimestamp()};
        }

        // Advance the iterator to the beginning of the remaining unpacked statements.
        stmtsIter = nextStmt;
    }

    return applyOpsOperationAssignment.numberOfOplogSlotsUsed;
}

void logCommitOrAbortForPreparedTransaction(OperationContext* opCtx,
                                            MutableOplogEntry* oplogEntry,
                                            DurableTxnStateEnum durableState) {
    const auto txnRetryCounter = *opCtx->getTxnRetryCounter();

    oplogEntry->setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry->setNss({"admin", "$cmd"});
    oplogEntry->setSessionId(opCtx->getLogicalSessionId());
    oplogEntry->setTxnNumber(opCtx->getTxnNumber());
    if (!isDefaultTxnRetryCounter(txnRetryCounter)) {
        oplogEntry->getOperationSessionInfo().setTxnRetryCounter(txnRetryCounter);
    }
    oplogEntry->setPrevWriteOpTimeInTransaction(
        TransactionParticipant::get(opCtx).getLastWriteOpTime());

    // There should not be a parent WUOW outside of this one. This guarantees the safety of the
    // write conflict retry loop.
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // We must not have a maximum lock timeout, since writing the commit or abort oplog entry for a
    // prepared transaction must always succeed.
    invariant(!opCtx->lockState()->hasMaxLockTimeout());

    writeConflictRetry(
        opCtx, "onPreparedTransactionCommitOrAbort", NamespaceString::kRsOplogNamespace.ns(), [&] {
            // Writes to the oplog only require a Global intent lock. Guaranteed by
            // OplogSlotReserver.
            invariant(opCtx->lockState()->isWriteLocked());

            WriteUnitOfWork wuow(opCtx);
            const auto oplogOpTime = logOperation(opCtx, oplogEntry);
            invariant(oplogEntry->getOpTime().isNull() || oplogEntry->getOpTime() == oplogOpTime);

            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setLastWriteOpTime(oplogOpTime);
            sessionTxnRecord.setLastWriteDate(oplogEntry->getWallClockTime());
            sessionTxnRecord.setState(durableState);
            if (!isDefaultTxnRetryCounter(txnRetryCounter)) {
                sessionTxnRecord.setTxnRetryCounter(txnRetryCounter);
            }
            onWriteOpCompleted(opCtx, {}, sessionTxnRecord);
            wuow.commit();
        });
}

}  // namespace

void OpObserverImpl::onUnpreparedTransactionCommit(OperationContext* opCtx,
                                                   std::vector<repl::ReplOperation>* statements,
                                                   size_t numberOfPrePostImagesToWrite) {
    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    // It is possible that the transaction resulted in no changes.  In that case, we should
    // not write an empty applyOps entry.
    if (statements->empty())
        return;

    repl::OpTime commitOpTime;
    // Reserve all the optimes in advance, so we only need to get the optime mutex once.  We
    // reserve enough entries for all statements in the transaction.
    auto oplogSlots =
        repl::getNextOpTimes(opCtx, statements->size() + numberOfPrePostImagesToWrite);

    // Throw TenantMigrationConflict error if the database for the transaction statements is being
    // migrated. We only need check the namespace of the first statement since a transaction's
    // statements must all be for the same tenant.
    tenant_migration_access_blocker::checkIfCanWriteOrThrow(
        opCtx, statements->begin()->getNss().db(), oplogSlots.back().getTimestamp());

    if (MONGO_unlikely(hangAndFailUnpreparedCommitAfterReservingOplogSlot.shouldFail())) {
        hangAndFailUnpreparedCommitAfterReservingOplogSlot.pauseWhileSet(opCtx);
        uasserted(51268, "hangAndFailUnpreparedCommitAfterReservingOplogSlot fail point enabled");
    }

    // Serialize transaction statements to BSON and determine their assignment to "applyOps"
    // entries.
    const auto applyOpsOplogSlotAndOperationAssignment =
        getApplyOpsOplogSlotAndOperationAssignmentForTransaction(
            opCtx, oplogSlots, numberOfPrePostImagesToWrite, false /*prepare*/, *statements);
    const auto wallClockTime = getWallClockTimeForOpLog(opCtx);

    // Log in-progress entries for the transaction along with the implicit commit.
    boost::optional<ImageBundle> imageToWrite;
    int numOplogEntries = logOplogEntries(opCtx,
                                          statements,
                                          oplogSlots,
                                          applyOpsOplogSlotAndOperationAssignment,
                                          &imageToWrite,
                                          numberOfPrePostImagesToWrite,
                                          false /* prepare*/,
                                          wallClockTime);

    // Write change stream pre-images. At this point the pre-images will be written at the
    // transaction commit timestamp as driven (implicitly) by the last written "applyOps" oplog
    // entry.
    writeChangeStreamPreImagesForTransaction(
        opCtx, *statements, applyOpsOplogSlotAndOperationAssignment, wallClockTime);

    if (imageToWrite) {
        writeToImageCollection(opCtx,
                               *opCtx->getLogicalSessionId(),
                               imageToWrite->timestamp,
                               imageToWrite->imageKind,
                               imageToWrite->imageDoc);
    }

    commitOpTime = oplogSlots[numOplogEntries - 1];
    invariant(!commitOpTime.isNull());
    shardObserveTransactionPrepareOrUnpreparedCommit(opCtx, *statements, commitOpTime);
}

void OpObserverImpl::onBatchedWriteStart(OperationContext* opCtx) {}

void OpObserverImpl::onBatchedWriteCommit(OperationContext* opCtx) {
    if (repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
            repl::ReplicationCoordinator::modeReplSet ||
        !opCtx->writesAreReplicated()) {
        return;
    }

    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    auto& batchedOps = batchedWriteContext.getBatchedOperations(opCtx);

    if (!batchedOps.size()) {
        return;
    }

    // Reserve all the optimes in advance, so we only need to get the optime mutex once.  We
    // reserve enough entries for all statements in the transaction.
    auto oplogSlots = repl::getNextOpTimes(opCtx, batchedOps.size());

    // Throw TenantMigrationConflict error if the database for the transaction statements is being
    // migrated. We only need check the namespace of the first statement since a transaction's
    // statements must all be for the same tenant.
    tenant_migration_access_blocker::checkIfCanWriteOrThrow(
        opCtx, batchedOps.begin()->getNss().db(), oplogSlots.back().getTimestamp());

    auto noPrePostImage = boost::optional<ImageBundle>(boost::none);

    // Serialize batched statements to BSON and determine their assignment to "applyOps"
    // entries.
    const auto applyOpsOplogSlotAndOperationAssignment =
        getApplyOpsOplogSlotAndOperationAssignmentForTransaction(
            opCtx, oplogSlots, 0 /*numberOfPrePostImagesToWrite*/, false /*prepare*/, batchedOps);
    const auto wallClockTime = getWallClockTimeForOpLog(opCtx);
    logOplogEntries(opCtx,
                    &batchedOps,
                    oplogSlots,
                    applyOpsOplogSlotAndOperationAssignment,
                    &noPrePostImage,
                    0 /* numberOfPrePostImagesToWrite */,
                    false,
                    wallClockTime);
}

void OpObserverImpl::onBatchedWriteAbort(OperationContext* opCtx) {}

void OpObserverImpl::onPreparedTransactionCommit(
    OperationContext* opCtx,
    OplogSlot commitOplogEntryOpTime,
    Timestamp commitTimestamp,
    const std::vector<repl::ReplOperation>& statements) noexcept {
    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    invariant(!commitTimestamp.isNull());

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpTime(commitOplogEntryOpTime);

    CommitTransactionOplogObject cmdObj;
    cmdObj.setCommitTimestamp(commitTimestamp);
    oplogEntry.setObject(cmdObj.toBSON());

    logCommitOrAbortForPreparedTransaction(opCtx, &oplogEntry, DurableTxnStateEnum::kCommitted);
}

std::unique_ptr<OpObserver::ApplyOpsOplogSlotAndOperationAssignment>
OpObserverImpl::preTransactionPrepare(OperationContext* opCtx,
                                      const std::vector<OplogSlot>& reservedSlots,
                                      size_t numberOfPrePostImagesToWrite,
                                      Date_t wallClockTime,
                                      std::vector<repl::ReplOperation>* statements) {
    auto applyOpsOplogSlotAndOperationAssignment =
        getApplyOpsOplogSlotAndOperationAssignmentForTransaction(
            opCtx, reservedSlots, numberOfPrePostImagesToWrite, true /*prepare*/, *statements);
    writeChangeStreamPreImagesForTransaction(
        opCtx, *statements, applyOpsOplogSlotAndOperationAssignment, wallClockTime);
    return std::make_unique<OpObserver::ApplyOpsOplogSlotAndOperationAssignment>(
        std::move(applyOpsOplogSlotAndOperationAssignment));
}

void OpObserverImpl::onTransactionPrepare(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    std::vector<repl::ReplOperation>* statements,
    const ApplyOpsOplogSlotAndOperationAssignment* applyOpsOperationAssignment,
    size_t numberOfPrePostImagesToWrite,
    Date_t wallClockTime) {
    invariant(!reservedSlots.empty());
    const auto prepareOpTime = reservedSlots.back();
    invariant(opCtx->getTxnNumber());
    invariant(!prepareOpTime.isNull());
    tassert(6278510,
            "Operation assignments to applyOps entries should be present",
            applyOpsOperationAssignment);

    // Don't write oplog entry on secondaries.
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    {
        // We should have reserved enough slots.
        invariant(reservedSlots.size() >= statements->size());
        TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

        writeConflictRetry(
            opCtx, "onTransactionPrepare", NamespaceString::kRsOplogNamespace.ns(), [&] {
                // Writes to the oplog only require a Global intent lock. Guaranteed by
                // OplogSlotReserver.
                invariant(opCtx->lockState()->isWriteLocked());

                WriteUnitOfWork wuow(opCtx);
                // It is possible that the transaction resulted in no changes, In that case, we
                // should not write any operations other than the prepare oplog entry.
                if (!statements->empty()) {
                    // We had reserved enough oplog slots for the worst case where each operation
                    // produced one oplog entry.  When operations are smaller and can be packed, we
                    // will waste the extra slots.  The implicit prepare oplog entry will still use
                    // the last reserved slot, because the transaction participant has already used
                    // that as the prepare time.
                    boost::optional<ImageBundle> imageToWrite;
                    logOplogEntries(opCtx,
                                    statements,
                                    reservedSlots,
                                    *applyOpsOperationAssignment,
                                    &imageToWrite,
                                    numberOfPrePostImagesToWrite,
                                    true /* prepare */,
                                    wallClockTime);
                    if (imageToWrite) {
                        writeToImageCollection(opCtx,
                                               *opCtx->getLogicalSessionId(),
                                               imageToWrite->timestamp,
                                               imageToWrite->imageKind,
                                               imageToWrite->imageDoc);
                    }
                } else {
                    // Log an empty 'prepare' oplog entry.
                    // We need to have at least one reserved slot.
                    invariant(reservedSlots.size() > 0);
                    BSONObjBuilder applyOpsBuilder;
                    BSONArrayBuilder opsArray(applyOpsBuilder.subarrayStart("applyOps"_sd));
                    opsArray.done();
                    applyOpsBuilder.append("prepare", true);

                    auto oplogSlot = reservedSlots.front();
                    MutableOplogEntry oplogEntry;
                    oplogEntry.setOpTime(oplogSlot);
                    oplogEntry.setPrevWriteOpTimeInTransaction(repl::OpTime());
                    oplogEntry.setObject(applyOpsBuilder.done());
                    oplogEntry.setWallClockTime(wallClockTime);
                    logApplyOps(opCtx,
                                &oplogEntry,
                                DurableTxnStateEnum::kPrepared,
                                oplogSlot,
                                {},
                                true /* updateTxnTable */);
                }
                wuow.commit();
            });
    }

    shardObserveTransactionPrepareOrUnpreparedCommit(opCtx, *statements, prepareOpTime);
}

void OpObserverImpl::onTransactionAbort(OperationContext* opCtx,
                                        boost::optional<OplogSlot> abortOplogEntryOpTime) {
    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant);

    if (!abortOplogEntryOpTime) {
        invariant(!txnParticipant.transactionIsCommitted());
        return;
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpTime(*abortOplogEntryOpTime);

    AbortTransactionOplogObject cmdObj;
    oplogEntry.setObject(cmdObj.toBSON());

    logCommitOrAbortForPreparedTransaction(opCtx, &oplogEntry, DurableTxnStateEnum::kAborted);
}

void OpObserverImpl::_onReplicationRollback(OperationContext* opCtx,
                                            const RollbackObserverInfo& rbInfo) {
    // Reset the key manager cache.
    auto validator = LogicalTimeValidator::get(opCtx);
    if (validator) {
        validator->resetKeyManagerCache();
    }

    // Check if the shard identity document rolled back.
    if (rbInfo.shardIdentityRolledBack) {
        fassertFailedNoTrace(50712);
    }

    // Force the default read/write concern cache to reload on next access in case the defaults
    // document was rolled back.
    ReadWriteConcernDefaults::get(opCtx).invalidate();

    stdx::unordered_set<NamespaceString> timeseriesNamespaces;
    for (const auto& ns : rbInfo.rollbackNamespaces) {
        if (ns.isTimeseriesBucketsCollection()) {
            timeseriesNamespaces.insert(ns.getTimeseriesViewNamespace());
        }
    }
    BucketCatalog::get(opCtx).clear([&timeseriesNamespaces](const NamespaceString& bucketNs) {
        return timeseriesNamespaces.contains(bucketNs);
    });
}

}  // namespace mongo
