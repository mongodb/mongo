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


#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/commands/write_commands_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/doc_validation_error.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_stats.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangWriteBeforeWaitingForMigrationDecision);
MONGO_FAIL_POINT_DEFINE(hangInsertBeforeWrite);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeCommit);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeWrite);
MONGO_FAIL_POINT_DEFINE(failUnorderedTimeseriesInsert);

void redactTooLongLog(mutablebson::Document* cmdObj, StringData fieldName) {
    namespace mmb = mutablebson;
    mmb::Element root = cmdObj->root();
    mmb::Element field = root.findFirstChildNamed(fieldName);

    // If the cmdObj is too large, it will be a "too big" message given by CachedBSONObj.get()
    if (!field.ok()) {
        return;
    }

    // Redact the log if there are more than one documents or operations.
    if (field.countChildren() > 1) {
        field.setValueInt(field.countChildren()).transitional_ignore();
    }
}

bool shouldSkipOutput(OperationContext* opCtx) {
    const WriteConcernOptions& writeConcern = opCtx->getWriteConcern();
    return writeConcern.isUnacknowledged() &&
        (writeConcern.syncMode == WriteConcernOptions::SyncMode::NONE ||
         writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET);
}

/**
 * Returns true if 'ns' is a time-series collection. That is, this namespace is backed by a
 * time-series buckets collection.
 */
template <class Request>
bool isTimeseries(OperationContext* opCtx, const Request& request) {
    uassert(5916400,
            "'isTimeseriesNamespace' parameter can only be set when the request is sent on "
            "system.buckets namespace",
            !request.getIsTimeseriesNamespace() ||
                request.getNamespace().isTimeseriesBucketsCollection());
    const auto bucketNss = request.getIsTimeseriesNamespace()
        ? request.getNamespace()
        : request.getNamespace().makeTimeseriesBucketsNamespace();

    // If the buckets collection exists now, the time-series insert path will check for the
    // existence of the buckets collection later on with a lock.
    // If this check is concurrent with the creation of a time-series collection and the buckets
    // collection does not yet exist, this check may return false unnecessarily. As a result, an
    // insert attempt into the time-series namespace will either succeed or fail, depending on who
    // wins the race.
    return CollectionCatalog::get(opCtx)
        ->lookupCollectionByNamespaceForRead(opCtx, bucketNss)
        .get();
}

NamespaceString makeTimeseriesBucketsNamespace(const NamespaceString& nss) {
    return nss.isTimeseriesBucketsCollection() ? nss : nss.makeTimeseriesBucketsNamespace();
}

/**
 * Transforms a single time-series insert to an update request on an existing bucket.
 */
write_ops::UpdateOpEntry makeTimeseriesUpdateOpEntry(
    OperationContext* opCtx,
    std::shared_ptr<BucketCatalog::WriteBatch> batch,
    const BSONObj& metadata) {
    BSONObjBuilder updateBuilder;
    {
        if (!batch->min().isEmpty() || !batch->max().isEmpty()) {
            BSONObjBuilder controlBuilder(updateBuilder.subobjStart(
                str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "control"));
            if (!batch->min().isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "min", batch->min());
            }
            if (!batch->max().isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "max", batch->max());
            }
        }
    }
    {
        // doc_diff::kSubDiffSectionFieldPrefix + <field name> => {<index_0>: ..., <index_1>: ...}
        StringDataMap<BSONObjBuilder> dataFieldBuilders;
        auto metadataElem = metadata.firstElement();
        DecimalCounter<uint32_t> count(batch->numPreviouslyCommittedMeasurements());
        for (const auto& doc : batch->measurements()) {
            for (const auto& elem : doc) {
                auto key = elem.fieldNameStringData();
                if (metadataElem && key == metadataElem.fieldNameStringData()) {
                    continue;
                }
                auto& builder = dataFieldBuilders[key];
                builder.appendAs(elem, count);
            }
            ++count;
        }

        // doc_diff::kSubDiffSectionFieldPrefix + <field name>
        BSONObjBuilder dataBuilder(updateBuilder.subobjStart("sdata"));
        BSONObjBuilder newDataFieldsBuilder;
        for (auto& pair : dataFieldBuilders) {
            // Existing 'data' fields with measurements require different treatment from fields
            // not observed before (missing from control.min and control.max).
            if (batch->newFieldNamesToBeInserted().count(pair.first)) {
                newDataFieldsBuilder.append(pair.first, pair.second.obj());
            }
        }
        auto newDataFields = newDataFieldsBuilder.obj();
        if (!newDataFields.isEmpty()) {
            dataBuilder.append(doc_diff::kInsertSectionFieldName, newDataFields);
        }
        for (auto& pair : dataFieldBuilders) {
            // Existing 'data' fields with measurements require different treatment from fields
            // not observed before (missing from control.min and control.max).
            if (!batch->newFieldNamesToBeInserted().count(pair.first)) {
                dataBuilder.append(doc_diff::kSubDiffSectionFieldPrefix + pair.first.toString(),
                                   BSON(doc_diff::kInsertSectionFieldName << pair.second.obj()));
            }
        }
    }
    write_ops::UpdateModification::DiffOptions options;
    options.mustCheckExistenceForInsertOperations =
        static_cast<bool>(repl::tenantMigrationInfo(opCtx));
    write_ops::UpdateModification u(
        updateBuilder.obj(), write_ops::UpdateModification::DeltaTag{}, options);
    write_ops::UpdateOpEntry update(BSON("_id" << batch->bucket().id), std::move(u));
    invariant(!update.getMulti(), batch->bucket().id.toString());
    invariant(!update.getUpsert(), batch->bucket().id.toString());
    return update;
}

/**
 * Transforms a single time-series insert to an update request on an existing bucket.
 */
write_ops::UpdateOpEntry makeTimeseriesCompressionOpEntry(
    OperationContext* opCtx,
    const OID& bucketId,
    write_ops::UpdateModification::TransformFunc compressionFunc) {
    write_ops::UpdateModification u(std::move(compressionFunc));
    write_ops::UpdateOpEntry update(BSON("_id" << bucketId), std::move(u));
    invariant(!update.getMulti(), bucketId.toString());
    invariant(!update.getUpsert(), bucketId.toString());
    return update;
}

/**
 * Returns the document for inserting a new bucket.
 */
BSONObj makeTimeseriesInsertDocument(std::shared_ptr<BucketCatalog::WriteBatch> batch,
                                     const BSONObj& metadata) {
    using namespace timeseries;

    auto metadataElem = metadata.firstElement();

    StringDataMap<BSONObjBuilder> dataBuilders;
    DecimalCounter<uint32_t> count;
    for (const auto& doc : batch->measurements()) {
        for (const auto& elem : doc) {
            auto key = elem.fieldNameStringData();
            if (metadataElem && key == metadataElem.fieldNameStringData()) {
                continue;
            }
            dataBuilders[key].appendAs(elem, count);
        }
        ++count;
    }

    BSONObjBuilder builder;
    builder.append("_id", batch->bucket().id);
    {
        BSONObjBuilder bucketControlBuilder(builder.subobjStart("control"));
        bucketControlBuilder.append(kBucketControlVersionFieldName,
                                    kTimeseriesControlDefaultVersion);
        bucketControlBuilder.append(kBucketControlMinFieldName, batch->min());
        bucketControlBuilder.append(kBucketControlMaxFieldName, batch->max());

        if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                serverGlobalParams.featureCompatibility)) {
            bucketControlBuilder.append(kBucketControlClosedFieldName, false);
        }
    }
    if (metadataElem) {
        builder.appendAs(metadataElem, kBucketMetaFieldName);
    }
    {
        BSONObjBuilder bucketDataBuilder(builder.subobjStart(kBucketDataFieldName));
        for (auto& dataBuilder : dataBuilders) {
            bucketDataBuilder.append(dataBuilder.first, dataBuilder.second.obj());
        }
    }

    return builder.obj();
}

/**
 * Returns true if the time-series write is retryable.
 */
bool isTimeseriesWriteRetryable(OperationContext* opCtx) {
    if (!opCtx->getTxnNumber()) {
        return false;
    }

    if (opCtx->inMultiDocumentTransaction()) {
        return false;
    }

    return true;
}

void getOpTimeAndElectionId(OperationContext* opCtx,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId) {
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
    const auto replMode = replCoord->getReplicationMode();

    *opTime = replMode != repl::ReplicationCoordinator::modeNone
        ? boost::make_optional(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp())
        : boost::none;
    *electionId = replMode == repl::ReplicationCoordinator::modeReplSet
        ? boost::make_optional(replCoord->getElectionId())
        : boost::none;
}

boost::optional<std::pair<Status, bool>> checkFailUnorderedTimeseriesInsertFailPoint(
    const BSONObj& metadata) {
    bool canContinue = true;
    if (MONGO_unlikely(failUnorderedTimeseriesInsert.shouldFail(
            [&metadata, &canContinue](const BSONObj& data) {
                BSONElementComparator comp(BSONElementComparator::FieldNamesMode::kIgnore, nullptr);
                if (auto continueElem = data["canContinue"]) {
                    canContinue = data["canContinue"].trueValue();
                }
                return comp.compare(data["metadata"], metadata.firstElement()) == 0;
            }))) {
        return std::make_pair(
            Status(ErrorCodes::FailPointEnabled,
                   "Failed unordered time-series insert due to failUnorderedTimeseriesInsert fail "
                   "point"),
            canContinue);
    }
    return boost::none;
}

boost::optional<write_ops::WriteError> generateError(OperationContext* opCtx,
                                                     const Status& status,
                                                     int index,
                                                     size_t numErrors) {
    if (status.isOK()) {
        return boost::none;
    }

    boost::optional<Status> overwrittenStatus;

    if (status == ErrorCodes::TenantMigrationConflict) {
        hangWriteBeforeWaitingForMigrationDecision.pauseWhileSet(opCtx);

        overwrittenStatus.emplace(
            tenant_migration_access_blocker::handleTenantMigrationConflict(opCtx, status));

        // Interruption errors encountered during batch execution fail the entire batch, so throw on
        // such errors here for consistency.
        if (ErrorCodes::isInterruption(*overwrittenStatus)) {
            uassertStatusOK(*overwrittenStatus);
        }

        // Tenant migration errors, similarly to migration errors consume too much space in the
        // ordered:false responses and get truncated. Since the call to
        // 'handleTenantMigrationConflict' above replaces the original status, we need to manually
        // truncate the new reason if the original 'status' was also truncated.
        if (status.reason().empty()) {
            overwrittenStatus = overwrittenStatus->withReason("");
        }
    }

    constexpr size_t kMaxErrorReasonsToReport = 1;
    constexpr size_t kMaxErrorSizeToReportAfterMaxReasonsReached = 1024 * 1024;

    if (numErrors > kMaxErrorReasonsToReport) {
        size_t errorSize =
            overwrittenStatus ? overwrittenStatus->reason().size() : status.reason().size();
        if (errorSize > kMaxErrorSizeToReportAfterMaxReasonsReached)
            overwrittenStatus =
                overwrittenStatus ? overwrittenStatus->withReason("") : status.withReason("");
    }

    if (overwrittenStatus)
        return write_ops::WriteError(index, std::move(*overwrittenStatus));
    else
        return write_ops::WriteError(index, status);
}

template <typename T>
boost::optional<write_ops::WriteError> generateError(OperationContext* opCtx,
                                                     const StatusWith<T>& result,
                                                     int index,
                                                     size_t numErrors) {
    return generateError(opCtx, result.getStatus(), index, numErrors);
}

/**
 * Contains hooks that are used by 'populateReply' method.
 */
struct PopulateReplyHooks {
    // Called for each 'SingleWriteResult' processed by 'populateReply' method.
    std::function<void(const SingleWriteResult&, int)> singleWriteResultHandler;

    // Called after all 'SingleWriteResult' processing is completed by 'populateReply' method.
    // This is called as the last method.
    std::function<void()> postProcessHandler;
};

/**
 * Method to populate a write command reply message. It takes 'result' parameter as an input
 * source and populate the fields of 'cmdReply'.
 */
template <typename CommandReplyType>
void populateReply(OperationContext* opCtx,
                   bool continueOnError,
                   size_t opsInBatch,
                   write_ops_exec::WriteResult result,
                   CommandReplyType* cmdReply,
                   boost::optional<PopulateReplyHooks> hooks = boost::none) {
    invariant(cmdReply);

    if (shouldSkipOutput(opCtx))
        return;

    if (continueOnError) {
        invariant(!result.results.empty());
        const auto& lastResult = result.results.back();

        if (lastResult == ErrorCodes::StaleDbVersion ||
            ErrorCodes::isStaleShardVersionError(lastResult.getStatus()) ||
            ErrorCodes::isTenantMigrationError(lastResult.getStatus())) {
            // For ordered:false commands we need to duplicate these error results for all ops
            // after we stopped. See handleError() in write_ops_exec.cpp for more info.
            //
            // Omit the reason from the duplicate unordered responses so it doesn't consume BSON
            // object space
            result.results.resize(opsInBatch, lastResult.getStatus().withReason(""));
        }
    }

    long long nVal = 0;
    std::vector<write_ops::WriteError> errors;
    for (size_t i = 0; i < result.results.size(); ++i) {
        if (auto error = generateError(opCtx, result.results[i], i, errors.size())) {
            errors.emplace_back(std::move(*error));
            continue;
        }

        const auto& opResult = result.results[i].getValue();
        nVal += opResult.getN();  // Always there.

        // Handle custom processing of each result.
        if (hooks && hooks->singleWriteResultHandler)
            hooks->singleWriteResultHandler(opResult, i);
    }

    auto& replyBase = cmdReply->getWriteCommandReplyBase();

    replyBase.setN(nVal);
    if (!result.retriedStmtIds.empty()) {
        replyBase.setRetriedStmtIds(std::move(result.retriedStmtIds));
    }
    if (!errors.empty()) {
        replyBase.setWriteErrors(std::move(errors));
    }

    // writeConcernError field is handled by command processor.

    {
        // Undocumented repl fields that mongos depends on.
        auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
        const auto replMode = replCoord->getReplicationMode();
        if (replMode != repl::ReplicationCoordinator::modeNone) {
            replyBase.setOpTime(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());

            if (replMode == repl::ReplicationCoordinator::modeReplSet) {
                replyBase.setElectionId(replCoord->getElectionId());
            }
        }
    }

    if (hooks && hooks->postProcessHandler)
        hooks->postProcessHandler();
}

class CmdInsert final : public write_ops::InsertCmdVersion1Gen<CmdInsert> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    void snipForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "documents");
    }

    std::string help() const final {
        return "insert documents";
    }

    ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest) {
            InsertOp::validate(request());
        }

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        write_ops::InsertCommandReply typedRun(OperationContext* opCtx) final try {
            doTransactionValidationForWrites(opCtx, ns());

            if (request().getEncryptionInformation().has_value() &&
                !request().getEncryptionInformation()->getCrudProcessed()) {
                write_ops::InsertCommandReply insertReply;
                auto batch = processFLEInsert(opCtx, request(), &insertReply);
                if (batch == FLEBatchResult::kProcessed) {
                    return insertReply;
                }
            }

            if (isTimeseries(opCtx, request())) {
                // Re-throw parsing exceptions to be consistent with CmdInsert::Invocation's
                // constructor.
                try {
                    return _performTimeseriesWrites(opCtx);
                } catch (DBException& ex) {
                    ex.addContext(str::stream() << "time-series insert failed: " << ns().ns());
                    throw;
                }
            }

            if (hangInsertBeforeWrite.shouldFail([&](const BSONObj& data) {
                    const auto ns = data.getStringField("ns");
                    return ns == request().getNamespace().toString();
                })) {
                hangInsertBeforeWrite.pauseWhileSet();
            }

            auto reply = write_ops_exec::performInserts(opCtx, request());

            write_ops::InsertCommandReply insertReply;
            populateReply(opCtx,
                          !request().getWriteCommandRequestBase().getOrdered(),
                          request().getDocuments().size(),
                          std::move(reply),
                          &insertReply);

            return insertReply;
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
        }

    private:
        using TimeseriesBatches =
            std::vector<std::pair<std::shared_ptr<BucketCatalog::WriteBatch>, size_t>>;
        using TimeseriesStmtIds = stdx::unordered_map<OID, std::vector<StmtId>, OID::Hasher>;
        struct TimeseriesSingleWriteResult {
            StatusWith<SingleWriteResult> result;
            bool canContinue = true;
        };

        void doCheckAuthorization(OperationContext* opCtx) const final try {
            auth::checkAuthForInsertCommand(AuthorizationSession::get(opCtx->getClient()),
                                            request().getBypassDocumentValidation(),
                                            request());
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
        }

        BucketCatalog::CombineWithInsertsFromOtherClients
        _canCombineTimeseriesInsertWithOtherClients(OperationContext* opCtx) const {
            return isTimeseriesWriteRetryable(opCtx) || request().getOrdered()
                ? BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow
                : BucketCatalog::CombineWithInsertsFromOtherClients::kAllow;
        }

        TimeseriesSingleWriteResult _getTimeseriesSingleWriteResult(
            write_ops_exec::WriteResult&& reply) const {
            invariant(reply.results.size() == 1,
                      str::stream() << "Unexpected number of results (" << reply.results.size()
                                    << ") for insert on time-series collection " << ns());

            return {std::move(reply.results[0]), reply.canContinue};
        }

        write_ops::WriteCommandRequestBase _makeTimeseriesWriteOpBase(
            std::vector<StmtId>&& stmtIds) const {
            write_ops::WriteCommandRequestBase base;

            // The schema validation configured in the bucket collection is intended for direct
            // operations by end users and is not applicable here.
            base.setBypassDocumentValidation(true);

            if (!stmtIds.empty()) {
                base.setStmtIds(std::move(stmtIds));
            }

            return base;
        }

        write_ops::InsertCommandRequest _makeTimeseriesInsertOp(
            std::shared_ptr<BucketCatalog::WriteBatch> batch,
            const BSONObj& metadata,
            std::vector<StmtId>&& stmtIds) const {
            write_ops::InsertCommandRequest op{makeTimeseriesBucketsNamespace(ns()),
                                               {makeTimeseriesInsertDocument(batch, metadata)}};
            op.setWriteCommandRequestBase(_makeTimeseriesWriteOpBase(std::move(stmtIds)));
            return op;
        }

        write_ops::UpdateCommandRequest _makeTimeseriesUpdateOp(
            OperationContext* opCtx,
            std::shared_ptr<BucketCatalog::WriteBatch> batch,
            const BSONObj& metadata,
            std::vector<StmtId>&& stmtIds) const {
            write_ops::UpdateCommandRequest op(
                makeTimeseriesBucketsNamespace(ns()),
                {makeTimeseriesUpdateOpEntry(opCtx, batch, metadata)});
            op.setWriteCommandRequestBase(_makeTimeseriesWriteOpBase(std::move(stmtIds)));
            return op;
        }

        write_ops::UpdateCommandRequest _makeTimeseriesCompressionOp(
            OperationContext* opCtx,
            const OID& bucketId,
            write_ops::UpdateModification::TransformFunc compressionFunc) const {
            write_ops::UpdateCommandRequest op(
                makeTimeseriesBucketsNamespace(ns()),
                {makeTimeseriesCompressionOpEntry(opCtx, bucketId, std::move(compressionFunc))});

            write_ops::WriteCommandRequestBase base;
            // The schema validation configured in the bucket collection is intended for direct
            // operations by end users and is not applicable here.
            base.setBypassDocumentValidation(true);

            // Timeseries compression operation is not a user operation and should not use a
            // statement id from any user op. Set to Uninitialized to bypass.
            base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});

            op.setWriteCommandRequestBase(std::move(base));
            return op;
        }

        /**
         * Returns the status and whether the request can continue.
         */
        TimeseriesSingleWriteResult _performTimeseriesInsert(
            OperationContext* opCtx,
            std::shared_ptr<BucketCatalog::WriteBatch> batch,
            const BSONObj& metadata,
            std::vector<StmtId>&& stmtIds) const {
            if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
                return {status->first, status->second};
            }
            return _getTimeseriesSingleWriteResult(write_ops_exec::performInserts(
                opCtx,
                _makeTimeseriesInsertOp(batch, metadata, std::move(stmtIds)),
                OperationSource::kTimeseriesInsert));
        }

        /**
         * Returns the status and whether the request can continue.
         */
        TimeseriesSingleWriteResult _performTimeseriesUpdate(
            OperationContext* opCtx,
            std::shared_ptr<BucketCatalog::WriteBatch> batch,
            const BSONObj& metadata,
            std::vector<StmtId>&& stmtIds) const {
            if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
                return {status->first, status->second};
            }

            return _getTimeseriesSingleWriteResult(write_ops_exec::performUpdates(
                opCtx,
                _makeTimeseriesUpdateOp(opCtx, batch, metadata, std::move(stmtIds)),
                OperationSource::kTimeseriesInsert));
        }

        TimeseriesSingleWriteResult _performTimeseriesBucketCompression(
            OperationContext* opCtx, const BucketCatalog::ClosedBucket& closedBucket) const {
            if (!feature_flags::gTimeseriesBucketCompression.isEnabled(
                    serverGlobalParams.featureCompatibility)) {
                return {SingleWriteResult(), true};
            }

            // Buckets with just a single measurement is not worth compressing.
            if (closedBucket.numMeasurements <= 1) {
                return {SingleWriteResult(), true};
            }

            bool validateCompression = gValidateTimeseriesCompression.load();

            boost::optional<int> beforeSize;
            TimeseriesStats::CompressedBucketInfo compressionStats;

            auto bucketCompressionFunc = [&](const BSONObj& bucketDoc) -> boost::optional<BSONObj> {
                beforeSize = bucketDoc.objsize();
                // Reset every time we run to ensure we never use a stale value
                compressionStats = {};
                auto compressed = timeseries::compressBucket(bucketDoc,
                                                             closedBucket.timeField,
                                                             ns(),
                                                             closedBucket.eligibleForReopening,
                                                             validateCompression);
                if (compressed.compressedBucket) {
                    // If compressed object size is larger than uncompressed, skip compression
                    // update.
                    if (compressed.compressedBucket->objsize() >= *beforeSize) {
                        LOGV2_DEBUG(5857802,
                                    1,
                                    "Skipping time-series bucket compression, compressed object is "
                                    "larger than original",
                                    "originalSize"_attr = bucketDoc.objsize(),
                                    "compressedSize"_attr = compressed.compressedBucket->objsize());
                        return boost::none;
                    }

                    compressionStats.size = compressed.compressedBucket->objsize();
                    compressionStats.numInterleaveRestarts = compressed.numInterleavedRestarts;
                } else if (compressed.decompressionFailed) {
                    compressionStats.decompressionFailed = true;
                }

                return compressed.compressedBucket;
            };

            auto compressionOp =
                _makeTimeseriesCompressionOp(opCtx, closedBucket.bucketId, bucketCompressionFunc);
            auto result = _getTimeseriesSingleWriteResult(
                write_ops_exec::performUpdates(opCtx, compressionOp, OperationSource::kStandard));

            // Report stats, if we fail before running the transform function then just skip
            // reporting.
            if (beforeSize) {
                compressionStats.result = result.result.getStatus();

                // Report stats for the bucket collection
                auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(
                    opCtx, compressionOp.getNamespace());
                if (coll) {
                    const auto& stats = TimeseriesStats::get(coll.get());
                    stats.onBucketClosed(*beforeSize, compressionStats);
                }
            }

            return result;
        }

        /**
         * Returns whether the request can continue.
         */
        bool _commitTimeseriesBucket(OperationContext* opCtx,
                                     std::shared_ptr<BucketCatalog::WriteBatch> batch,
                                     size_t start,
                                     size_t index,
                                     std::vector<StmtId>&& stmtIds,
                                     std::vector<write_ops::WriteError>* errors,
                                     boost::optional<repl::OpTime>* opTime,
                                     boost::optional<OID>* electionId,
                                     std::vector<size_t>* docsToRetry) const try {
            auto& bucketCatalog = BucketCatalog::get(opCtx);

            auto metadata = bucketCatalog.getMetadata(batch->bucket());
            auto status = bucketCatalog.prepareCommit(batch);
            if (!status.isOK()) {
                invariant(batch->finished());
                docsToRetry->push_back(index);
                return true;
            }

            hangTimeseriesInsertBeforeWrite.pauseWhileSet();

            const auto docId = batch->bucket().id;
            const bool performInsert = batch->numPreviouslyCommittedMeasurements() == 0;
            if (performInsert) {
                const auto output =
                    _performTimeseriesInsert(opCtx, batch, metadata, std::move(stmtIds));
                if (auto error =
                        generateError(opCtx, output.result, start + index, errors->size())) {
                    errors->emplace_back(std::move(*error));
                    bucketCatalog.abort(batch, output.result.getStatus());
                    return output.canContinue;
                }

                invariant(output.result.getValue().getN() == 1,
                          str::stream()
                              << "Expected 1 insertion of document with _id '" << docId
                              << "', but found " << output.result.getValue().getN() << ".");
            } else {
                const auto output =
                    _performTimeseriesUpdate(opCtx, batch, metadata, std::move(stmtIds));
                if (auto error =
                        generateError(opCtx, output.result, start + index, errors->size())) {
                    errors->emplace_back(std::move(*error));
                    bucketCatalog.abort(batch, output.result.getStatus());
                    return output.canContinue;
                }

                invariant(output.result.getValue().getNModified() == 1,
                          str::stream()
                              << "Expected 1 update of document with _id '" << docId
                              << "', but found " << output.result.getValue().getNModified() << ".");
            }

            getOpTimeAndElectionId(opCtx, opTime, electionId);

            auto closedBucket =
                bucketCatalog.finish(batch, BucketCatalog::CommitInfo{*opTime, *electionId});

            if (closedBucket) {
                // If this write closed a bucket, compress the bucket
                auto output = _performTimeseriesBucketCompression(opCtx, *closedBucket);
                if (auto error =
                        generateError(opCtx, output.result, start + index, errors->size())) {
                    errors->emplace_back(std::move(*error));
                    return output.canContinue;
                }
            }
            return true;
        } catch (const DBException& ex) {
            BucketCatalog::get(opCtx).abort(batch, ex.toStatus());
            throw;
        }

        enum struct TimeseriesAtomicWriteResult {
            kSuccess,
            kContinuableError,
            kNonContinuableError,
        };

        TimeseriesAtomicWriteResult _commitTimeseriesBucketsAtomically(
            OperationContext* opCtx,
            TimeseriesBatches* batches,
            TimeseriesStmtIds&& stmtIds,
            std::vector<write_ops::WriteError>* errors,
            boost::optional<repl::OpTime>* opTime,
            boost::optional<OID>* electionId) const {
            auto& bucketCatalog = BucketCatalog::get(opCtx);

            std::vector<std::reference_wrapper<std::shared_ptr<BucketCatalog::WriteBatch>>>
                batchesToCommit;

            for (auto& [batch, _] : *batches) {
                if (batch->claimCommitRights()) {
                    batchesToCommit.push_back(batch);
                }
            }

            if (batchesToCommit.empty()) {
                return TimeseriesAtomicWriteResult::kSuccess;
            }

            // Sort by bucket so that preparing the commit for each batch cannot deadlock.
            std::sort(batchesToCommit.begin(), batchesToCommit.end(), [](auto left, auto right) {
                return left.get()->bucket().id < right.get()->bucket().id;
            });

            Status abortStatus = Status::OK();
            ScopeGuard batchGuard{[&] {
                for (auto batch : batchesToCommit) {
                    if (batch.get()) {
                        bucketCatalog.abort(batch, abortStatus);
                    }
                }
            }};

            try {
                std::vector<write_ops::InsertCommandRequest> insertOps;
                std::vector<write_ops::UpdateCommandRequest> updateOps;

                for (auto batch : batchesToCommit) {
                    auto metadata = bucketCatalog.getMetadata(batch.get()->bucket());
                    auto prepareCommitStatus = bucketCatalog.prepareCommit(batch);
                    if (!prepareCommitStatus.isOK()) {
                        abortStatus = prepareCommitStatus;
                        return TimeseriesAtomicWriteResult::kContinuableError;
                    }

                    if (batch.get()->numPreviouslyCommittedMeasurements() == 0) {
                        insertOps.push_back(_makeTimeseriesInsertOp(
                            batch, metadata, std::move(stmtIds[batch.get()->bucket().id])));
                    } else {
                        updateOps.push_back(_makeTimeseriesUpdateOp(
                            opCtx, batch, metadata, std::move(stmtIds[batch.get()->bucket().id])));
                    }
                }

                hangTimeseriesInsertBeforeWrite.pauseWhileSet();

                auto result =
                    write_ops_exec::performAtomicTimeseriesWrites(opCtx, insertOps, updateOps);
                if (!result.isOK()) {
                    abortStatus = result;
                    return TimeseriesAtomicWriteResult::kContinuableError;
                }

                getOpTimeAndElectionId(opCtx, opTime, electionId);

                bool compressClosedBuckets = true;
                for (auto batch : batchesToCommit) {
                    auto closedBucket = bucketCatalog.finish(
                        batch, BucketCatalog::CommitInfo{*opTime, *electionId});
                    batch.get().reset();

                    if (!closedBucket || !compressClosedBuckets) {
                        continue;
                    }

                    // If this write closed a bucket, compress the bucket
                    auto ret = _performTimeseriesBucketCompression(opCtx, *closedBucket);
                    if (!ret.result.isOK()) {
                        // Don't try to compress any other buckets if we fail. We're not allowed to
                        // do more write operations.
                        compressClosedBuckets = false;
                    }
                    if (!ret.canContinue) {
                        abortStatus = ret.result.getStatus();
                        return TimeseriesAtomicWriteResult::kNonContinuableError;
                    }
                }
            } catch (const DBException& ex) {
                abortStatus = ex.toStatus();
                throw;
            }

            batchGuard.dismiss();
            return TimeseriesAtomicWriteResult::kSuccess;
        }

        // For sharded time-series collections, we need to use the granularity from the config
        // server (through shard filtering information) as the source of truth for the current
        // granularity value, due to the possible inconsistency in the process of granularity
        // updates.
        static void _rebuildOptionsWithGranularityFromConfigServer(
            OperationContext* opCtx,
            TimeseriesOptions& timeSeriesOptions,
            const NamespaceString& bucketsNs) {
            AutoGetCollectionForRead coll(opCtx, bucketsNs);
            auto collDesc =
                CollectionShardingState::get(opCtx, bucketsNs)->getCollectionDescription(opCtx);
            if (collDesc.isSharded()) {
                tassert(6102801,
                        "Sharded time-series buckets collection is missing time-series fields",
                        collDesc.getTimeseriesFields());
                auto granularity = collDesc.getTimeseriesFields()->getGranularity();
                auto bucketSpan = timeseries::getMaxSpanSecondsFromGranularity(granularity);
                timeSeriesOptions.setGranularity(granularity);
                timeSeriesOptions.setBucketMaxSpanSeconds(bucketSpan);
            }
        }

        std::tuple<TimeseriesBatches,
                   TimeseriesStmtIds,
                   size_t /* numInserted */,
                   bool /* canContinue */>
        _insertIntoBucketCatalog(OperationContext* opCtx,
                                 size_t start,
                                 size_t numDocs,
                                 const std::vector<size_t>& indices,
                                 std::vector<write_ops::WriteError>* errors,
                                 bool* containsRetry) const {
            auto& bucketCatalog = BucketCatalog::get(opCtx);

            auto bucketsNs = makeTimeseriesBucketsNamespace(ns());
            // Holding this shared pointer to the collection guarantees that the collator is not
            // invalidated.
            auto bucketsColl =
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(opCtx, bucketsNs);
            uassert(ErrorCodes::NamespaceNotFound,
                    "Could not find time-series buckets collection for write",
                    bucketsColl);
            uassert(ErrorCodes::InvalidOptions,
                    "Time-series buckets collection is missing time-series options",
                    bucketsColl->getTimeseriesOptions());

            auto timeSeriesOptions = *bucketsColl->getTimeseriesOptions();

            boost::optional<Status> rebuildOptionsError;
            try {
                _rebuildOptionsWithGranularityFromConfigServer(opCtx, timeSeriesOptions, bucketsNs);
            } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>& ex) {
                // This could occur when the shard version attached to the request is for the time
                // series namespace (unsharded), which is compared to the shard version of the
                // bucket namespace. Consequently, every single entry fails but the whole operation
                // succeeds.

                rebuildOptionsError = ex.toStatus();

                auto& oss{OperationShardingState::get(opCtx)};
                oss.setShardingOperationFailedStatus(ex.toStatus());
            }

            TimeseriesBatches batches;
            TimeseriesStmtIds stmtIds;
            bool canContinue = true;

            auto insert = [&](size_t index) {
                invariant(start + index < request().getDocuments().size());

                if (rebuildOptionsError) {
                    const auto error{
                        generateError(opCtx, *rebuildOptionsError, start + index, errors->size())};
                    errors->emplace_back(std::move(*error));
                    return false;
                }

                auto stmtId = request().getStmtIds()
                    ? request().getStmtIds()->at(start + index)
                    : request().getStmtId().value_or(0) + start + index;

                if (isTimeseriesWriteRetryable(opCtx) &&
                    TransactionParticipant::get(opCtx).checkStatementExecutedNoOplogEntryFetch(
                        opCtx, stmtId)) {
                    RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                    *containsRetry = true;
                    return true;
                }

                auto result = bucketCatalog.insert(
                    opCtx,
                    ns().isTimeseriesBucketsCollection() ? ns().getTimeseriesViewNamespace() : ns(),
                    bucketsColl->getDefaultCollator(),
                    timeSeriesOptions,
                    request().getDocuments()[start + index],
                    _canCombineTimeseriesInsertWithOtherClients(opCtx));

                if (auto error = generateError(opCtx, result, start + index, errors->size())) {
                    errors->emplace_back(std::move(*error));
                    return false;
                } else {
                    const auto& batch = result.getValue().batch;
                    batches.emplace_back(batch, index);
                    if (isTimeseriesWriteRetryable(opCtx)) {
                        stmtIds[batch->bucket().id].push_back(stmtId);
                    }
                }

                // If this insert closed buckets, rewrite to be a compressed column. If we cannot
                // perform write operations at this point the bucket will be left uncompressed.
                for (const auto& closedBucket : result.getValue().closedBuckets) {
                    if (!canContinue) {
                        break;
                    }

                    // If this write closed a bucket, compress the bucket
                    auto ret = _performTimeseriesBucketCompression(opCtx, closedBucket);
                    if (auto error =
                            generateError(opCtx, ret.result, start + index, errors->size())) {
                        // Bucket compression only fail when we may not try to perform any other
                        // write operation. When handleError() inside write_ops_exec.cpp return
                        // false.
                        errors->emplace_back(std::move(*error));
                        canContinue = false;
                        return false;
                    }
                    canContinue = ret.canContinue;
                }

                return true;
            };

            if (!indices.empty()) {
                std::for_each(indices.begin(), indices.end(), insert);
            } else {
                for (size_t i = 0; i < numDocs; i++) {
                    if (!insert(i) && request().getOrdered()) {
                        return {std::move(batches), std::move(stmtIds), i, canContinue};
                    }
                }
            }

            return {std::move(batches),
                    std::move(stmtIds),
                    request().getDocuments().size(),
                    canContinue};
        }

        void _getTimeseriesBatchResults(OperationContext* opCtx,
                                        const TimeseriesBatches& batches,
                                        size_t start,
                                        size_t indexOfLastProcessedBatch,
                                        bool canContinue,
                                        std::vector<write_ops::WriteError>* errors,
                                        boost::optional<repl::OpTime>* opTime,
                                        boost::optional<OID>* electionId,
                                        std::vector<size_t>* docsToRetry = nullptr) const {
            boost::optional<write_ops::WriteError> lastError;
            if (!errors->empty()) {
                lastError = errors->back();
            }

            for (size_t itr = 0; itr < batches.size(); ++itr) {
                const auto& [batch, index] = batches[itr];
                if (!batch) {
                    continue;
                }

                // If there are any unprocessed batches, we mark them as error with the last known
                // error.
                if (itr > indexOfLastProcessedBatch && batch->claimCommitRights()) {
                    BucketCatalog::get(opCtx).abort(batch, lastError->getStatus());
                    errors->emplace_back(start + index, lastError->getStatus());
                    continue;
                }

                auto swCommitInfo = batch->getResult();
                if (swCommitInfo.getStatus() == ErrorCodes::TimeseriesBucketCleared) {
                    tassert(6023102, "the 'docsToRetry' cannot be null", docsToRetry);
                    docsToRetry->push_back(index);
                    continue;
                }
                if (auto error = generateError(
                        opCtx, swCommitInfo.getStatus(), start + index, errors->size())) {
                    errors->emplace_back(std::move(*error));
                    continue;
                }

                const auto& commitInfo = swCommitInfo.getValue();
                if (commitInfo.opTime) {
                    *opTime = std::max(opTime->value_or(repl::OpTime()), *commitInfo.opTime);
                }
                if (commitInfo.electionId) {
                    *electionId = std::max(electionId->value_or(OID()), *commitInfo.electionId);
                }
            }

            // If we cannot continue the request, we should convert all the 'docsToRetry' into an
            // error.
            if (!canContinue && docsToRetry) {
                for (auto&& index : *docsToRetry) {
                    errors->emplace_back(start + index, lastError->getStatus());
                }
                docsToRetry->clear();
            }
        }

        TimeseriesAtomicWriteResult _performOrderedTimeseriesWritesAtomically(
            OperationContext* opCtx,
            std::vector<write_ops::WriteError>* errors,
            boost::optional<repl::OpTime>* opTime,
            boost::optional<OID>* electionId,
            bool* containsRetry) const {
            auto [batches, stmtIds, numInserted, canContinue] = _insertIntoBucketCatalog(
                opCtx, 0, request().getDocuments().size(), {}, errors, containsRetry);
            if (!canContinue) {
                return TimeseriesAtomicWriteResult::kNonContinuableError;
            }

            hangTimeseriesInsertBeforeCommit.pauseWhileSet();

            auto result = _commitTimeseriesBucketsAtomically(
                opCtx, &batches, std::move(stmtIds), errors, opTime, electionId);
            if (result != TimeseriesAtomicWriteResult::kSuccess) {
                return result;
            }

            _getTimeseriesBatchResults(
                opCtx, batches, 0, batches.size(), true, errors, opTime, electionId);

            return TimeseriesAtomicWriteResult::kSuccess;
        }

        /**
         * Returns the number of documents that were inserted.
         */
        size_t _performOrderedTimeseriesWrites(OperationContext* opCtx,
                                               std::vector<write_ops::WriteError>* errors,
                                               boost::optional<repl::OpTime>* opTime,
                                               boost::optional<OID>* electionId,
                                               bool* containsRetry) const {
            auto result = _performOrderedTimeseriesWritesAtomically(
                opCtx, errors, opTime, electionId, containsRetry);
            switch (result) {
                case TimeseriesAtomicWriteResult::kSuccess:
                    return request().getDocuments().size();
                case TimeseriesAtomicWriteResult::kNonContinuableError:
                    // If we can't continue, we know that 0 were inserted since this function should
                    // guarantee that the inserts are atomic.
                    return 0;
                case TimeseriesAtomicWriteResult::kContinuableError:
                    break;
                default:
                    MONGO_UNREACHABLE;
            }

            for (size_t i = 0; i < request().getDocuments().size(); ++i) {
                _performUnorderedTimeseriesWritesWithRetries(
                    opCtx, i, 1, errors, opTime, electionId, containsRetry);
                if (!errors->empty()) {
                    return i;
                }
            }

            return request().getDocuments().size();
        }

        /**
         * Writes to the underlying system.buckets collection. Returns the indices, of the batch
         * which were attempted in an update operation, but found no bucket to update. These indices
         * can be passed as the 'indices' parameter in a subsequent call to this function, in order
         * to to be retried.
         */
        std::vector<size_t> _performUnorderedTimeseriesWrites(
            OperationContext* opCtx,
            size_t start,
            size_t numDocs,
            const std::vector<size_t>& indices,
            std::vector<write_ops::WriteError>* errors,
            boost::optional<repl::OpTime>* opTime,
            boost::optional<OID>* electionId,
            bool* containsRetry) const {
            auto [batches, bucketStmtIds, _, canContinue] =
                _insertIntoBucketCatalog(opCtx, start, numDocs, indices, errors, containsRetry);

            hangTimeseriesInsertBeforeCommit.pauseWhileSet();

            std::vector<size_t> docsToRetry;

            if (!canContinue) {
                return docsToRetry;
            }

            size_t itr = 0;
            for (; itr < batches.size(); ++itr) {
                auto& [batch, index] = batches[itr];
                if (batch->claimCommitRights()) {
                    auto stmtIds = isTimeseriesWriteRetryable(opCtx)
                        ? std::move(bucketStmtIds[batch->bucket().id])
                        : std::vector<StmtId>{};

                    canContinue = _commitTimeseriesBucket(opCtx,
                                                          batch,
                                                          start,
                                                          index,
                                                          std::move(stmtIds),
                                                          errors,
                                                          opTime,
                                                          electionId,
                                                          &docsToRetry);
                    batch.reset();
                    if (!canContinue) {
                        break;
                    }
                }
            }

            _getTimeseriesBatchResults(
                opCtx, batches, 0, itr, canContinue, errors, opTime, electionId, &docsToRetry);
            tassert(6023101,
                    "the 'docsToRetry' cannot exist when the request cannot be continued",
                    canContinue || docsToRetry.empty());
            return docsToRetry;
        }

        void _performUnorderedTimeseriesWritesWithRetries(
            OperationContext* opCtx,
            size_t start,
            size_t numDocs,
            std::vector<write_ops::WriteError>* errors,
            boost::optional<repl::OpTime>* opTime,
            boost::optional<OID>* electionId,
            bool* containsRetry) const {
            std::vector<size_t> docsToRetry;
            do {
                docsToRetry = _performUnorderedTimeseriesWrites(
                    opCtx, start, numDocs, docsToRetry, errors, opTime, electionId, containsRetry);
            } while (!docsToRetry.empty());
        }

        write_ops::InsertCommandReply _performTimeseriesWrites(OperationContext* opCtx) const {
            auto& curOp = *CurOp::get(opCtx);
            ON_BLOCK_EXIT([&] {
                // This is the only part of finishCurOp we need to do for inserts because they reuse
                // the top-level curOp. The rest is handled by the top-level entrypoint.
                curOp.done();
                Top::get(opCtx->getServiceContext())
                    .record(opCtx,
                            ns().ns(),
                            LogicalOp::opInsert,
                            Top::LockType::WriteLocked,
                            durationCount<Microseconds>(curOp.elapsedTimeExcludingPauses()),
                            curOp.isCommand(),
                            curOp.getReadWriteType());
            });

            uassert(
                ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot insert into a time-series collection in a multi-document "
                                 "transaction: "
                              << ns(),
                !opCtx->inMultiDocumentTransaction());

            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                curOp.setNS_inlock(ns().ns());
                curOp.setLogicalOp_inlock(LogicalOp::opInsert);
                curOp.ensureStarted();
                curOp.debug().additiveMetrics.ninserted = 0;
            }

            std::vector<write_ops::WriteError> errors;
            boost::optional<repl::OpTime> opTime;
            boost::optional<OID> electionId;
            bool containsRetry = false;

            write_ops::InsertCommandReply insertReply;
            auto& baseReply = insertReply.getWriteCommandReplyBase();

            if (request().getOrdered()) {
                baseReply.setN(_performOrderedTimeseriesWrites(
                    opCtx, &errors, &opTime, &electionId, &containsRetry));
            } else {
                _performUnorderedTimeseriesWritesWithRetries(opCtx,
                                                             0,
                                                             request().getDocuments().size(),
                                                             &errors,
                                                             &opTime,
                                                             &electionId,
                                                             &containsRetry);
                baseReply.setN(request().getDocuments().size() - errors.size());
            }

            if (!errors.empty()) {
                baseReply.setWriteErrors(std::move(errors));
            }
            if (opTime) {
                baseReply.setOpTime(*opTime);
            }
            if (electionId) {
                baseReply.setElectionId(*electionId);
            }
            if (containsRetry) {
                RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
            }

            curOp.debug().additiveMetrics.ninserted = baseReply.getN();
            globalOpCounters.gotInserts(baseReply.getN());

            return insertReply;
        }
    };
} cmdInsert;

class CmdUpdate final : public write_ops::UpdateCmdVersion1Gen<CmdUpdate> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    void snipForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "updates");
    }

    std::string help() const final {
        return "update documents";
    }

    ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest), _commandObj(opMsgRequest.body) {
            UpdateOp::validate(request());

            invariant(_commandObj.isOwned());

            // Extend the lifetime of `updates` to allow asynchronous mirroring.
            if (auto seq = opMsgRequest.getSequence("updates"_sd); seq && !seq->objs.empty()) {
                // Current design ignores contents of `updates` array except for the first entry.
                // Assuming identical collation for all elements in `updates`, future design could
                // use the disjunction primitive (i.e, `$or`) to compile all queries into a single
                // filter. Such a design also requires a sound way of combining hints.
                invariant(seq->objs.front().isOwned());
                _updateOpObj = seq->objs.front();
            }
        }

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        bool getBypass() const {
            return request().getBypassDocumentValidation();
        }

        bool supportsReadMirroring() const override {
            return true;
        }

        void appendMirrorableRequest(BSONObjBuilder* bob) const override {
            auto extractQueryDetails = [](const BSONObj& update, BSONObjBuilder* bob) -> void {
                // "filter", "hint", and "collation" fields are optional.
                if (update.isEmpty())
                    return;

                // The constructor verifies the following.
                invariant(update.isOwned());

                if (update.hasField("q"))
                    bob->append("filter", update["q"].Obj());
                if (update.hasField("hint") && !update["hint"].Obj().isEmpty())
                    bob->append("hint", update["hint"].Obj());
                if (update.hasField("collation") && !update["collation"].Obj().isEmpty())
                    bob->append("collation", update["collation"].Obj());
            };

            invariant(!_commandObj.isEmpty());

            if (const auto& shardVersion = _commandObj.getField("shardVersion");
                !shardVersion.eoo()) {
                bob->append(shardVersion);
            }

            bob->append("find", _commandObj["update"].String());
            extractQueryDetails(_updateOpObj, bob);
            bob->append("batchSize", 1);
            bob->append("singleBatch", true);
        }

        write_ops::UpdateCommandReply typedRun(OperationContext* opCtx) final try {
            doTransactionValidationForWrites(opCtx, ns());
            write_ops::UpdateCommandReply updateReply;
            OperationSource source = OperationSource::kStandard;

            if (request().getEncryptionInformation().has_value() &&
                !request().getEncryptionInformation().value().getCrudProcessed()) {
                return processFLEUpdate(opCtx, request());
            }

            if (isTimeseries(opCtx, request())) {
                uassert(ErrorCodes::InvalidOptions,
                        "Time-series updates are not enabled",
                        feature_flags::gTimeseriesUpdatesAndDeletes.isEnabled(
                            serverGlobalParams.featureCompatibility));
                uassert(ErrorCodes::OperationNotSupportedInTransaction,
                        str::stream() << "Cannot perform a multi-document transaction on a "
                                         "time-series collection: "
                                      << ns(),
                        !opCtx->inMultiDocumentTransaction());
                source = OperationSource::kTimeseriesUpdate;
            }

            long long nModified = 0;

            // Tracks the upserted information. The memory of this variable gets moved in the
            // 'postProcessHandler' and should not be accessed afterwards.
            std::vector<write_ops::Upserted> upsertedInfoVec;

            auto reply = write_ops_exec::performUpdates(opCtx, request(), source);

            // Handler to process each 'SingleWriteResult'.
            auto singleWriteHandler = [&](const SingleWriteResult& opResult, int index) {
                nModified += opResult.getNModified();
                BSONSizeTracker upsertInfoSizeTracker;

                if (auto idElement = opResult.getUpsertedId().firstElement())
                    upsertedInfoVec.emplace_back(write_ops::Upserted(index, idElement));
            };

            // Handler to do the post-processing.
            auto postProcessHandler = [&]() {
                updateReply.setNModified(nModified);
                if (!upsertedInfoVec.empty())
                    updateReply.setUpserted(std::move(upsertedInfoVec));
            };

            populateReply(opCtx,
                          !request().getWriteCommandRequestBase().getOrdered(),
                          request().getUpdates().size(),
                          std::move(reply),
                          &updateReply,
                          PopulateReplyHooks{singleWriteHandler, postProcessHandler});

            // Collect metrics.
            for (auto&& update : request().getUpdates()) {
                // If this was a pipeline style update, record that pipeline-style was used and
                // which stages were being used.
                auto& updateMod = update.getU();
                if (updateMod.type() == write_ops::UpdateModification::Type::kPipeline) {
                    AggregateCommandRequest aggCmd(request().getNamespace(),
                                                   updateMod.getUpdatePipeline());
                    LiteParsedPipeline pipeline(aggCmd);
                    pipeline.tickGlobalStageCounters();
                    CmdUpdate::updateMetrics.incrementExecutedWithAggregationPipeline();
                }

                // If this command had arrayFilters option, record that it was used.
                if (update.getArrayFilters()) {
                    CmdUpdate::updateMetrics.incrementExecutedWithArrayFilters();
                }
            }

            return updateReply;
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final try {
            auth::checkAuthForUpdateCommand(AuthorizationSession::get(opCtx->getClient()),
                                            request().getBypassDocumentValidation(),
                                            request());
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    request().getUpdates().size() == 1);

            UpdateRequest updateRequest(request().getUpdates()[0]);
            updateRequest.setNamespaceString(request().getNamespace());
            if (shouldDoFLERewrite(request())) {
                updateRequest.setQuery(
                    processFLEWriteExplainD(opCtx,
                                            write_ops::collationOf(request().getUpdates()[0]),
                                            request(),
                                            updateRequest.getQuery()));
            }
            updateRequest.setLegacyRuntimeConstants(request().getLegacyRuntimeConstants().value_or(
                Variables::generateRuntimeConstants(opCtx)));
            updateRequest.setLetParameters(request().getLet());
            updateRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
            updateRequest.setExplain(verbosity);

            const ExtensionsCallbackReal extensionsCallback(opCtx,
                                                            &updateRequest.getNamespaceString());
            ParsedUpdate parsedUpdate(opCtx, &updateRequest, extensionsCallback);
            uassertStatusOK(parsedUpdate.parseRequest());

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetCollection collection(opCtx, request().getNamespace(), MODE_IX);

            auto exec = uassertStatusOK(getExecutorUpdate(&CurOp::get(opCtx)->debug(),
                                                          &collection.getCollection(),
                                                          &parsedUpdate,
                                                          verbosity));
            auto bodyBuilder = result->getBodyBuilder();
            Explain::explainStages(exec.get(),
                                   collection.getCollection(),
                                   verbosity,
                                   BSONObj(),
                                   _commandObj,
                                   &bodyBuilder);
        }

        BSONObj _commandObj;

        // Holds a shared pointer to the first entry in `updates` array.
        BSONObj _updateOpObj;
    };

    // Update related command execution metrics.
    static UpdateMetrics updateMetrics;
} cmdUpdate;

UpdateMetrics CmdUpdate::updateMetrics{"update"};

class CmdDelete final : public write_ops::DeleteCmdVersion1Gen<CmdDelete> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    void snipForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "deletes");
    }

    std::string help() const final {
        return "delete documents";
    }

    ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest), _commandObj(opMsgRequest.body) {
            DeleteOp::validate(request());
        }

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        write_ops::DeleteCommandReply typedRun(OperationContext* opCtx) final try {
            doTransactionValidationForWrites(opCtx, ns());
            write_ops::DeleteCommandReply deleteReply;
            OperationSource source = OperationSource::kStandard;

            if (request().getEncryptionInformation().has_value()) {
                return processFLEDelete(opCtx, request());
            }

            if (isTimeseries(opCtx, request())) {
                uassert(ErrorCodes::InvalidOptions,
                        "Time-series deletes are not enabled",
                        feature_flags::gTimeseriesUpdatesAndDeletes.isEnabled(
                            serverGlobalParams.featureCompatibility));
                uassert(ErrorCodes::OperationNotSupportedInTransaction,
                        str::stream() << "Cannot perform a multi-document transaction on a "
                                         "time-series collection: "
                                      << ns(),
                        !opCtx->inMultiDocumentTransaction());
                source = OperationSource::kTimeseriesDelete;
            }

            auto reply = write_ops_exec::performDeletes(opCtx, request(), source);
            populateReply(opCtx,
                          !request().getWriteCommandRequestBase().getOrdered(),
                          request().getDeletes().size(),
                          std::move(reply),
                          &deleteReply);

            return deleteReply;
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final try {
            auth::checkAuthForDeleteCommand(AuthorizationSession::get(opCtx->getClient()),
                                            request().getBypassDocumentValidation(),
                                            request());
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    request().getDeletes().size() == 1);

            auto deleteRequest = DeleteRequest{};
            deleteRequest.setNsString(request().getNamespace());
            deleteRequest.setLegacyRuntimeConstants(request().getLegacyRuntimeConstants().value_or(
                Variables::generateRuntimeConstants(opCtx)));
            deleteRequest.setLet(request().getLet());

            BSONObj query = request().getDeletes()[0].getQ();
            if (shouldDoFLERewrite(request())) {
                query = processFLEWriteExplainD(
                    opCtx, write_ops::collationOf(request().getDeletes()[0]), request(), query);
            }
            deleteRequest.setQuery(std::move(query));

            deleteRequest.setCollation(write_ops::collationOf(request().getDeletes()[0]));
            deleteRequest.setMulti(request().getDeletes()[0].getMulti());
            deleteRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
            deleteRequest.setHint(request().getDeletes()[0].getHint());
            deleteRequest.setIsExplain(true);

            ParsedDelete parsedDelete(opCtx, &deleteRequest);
            uassertStatusOK(parsedDelete.parseRequest());

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetCollection collection(opCtx, request().getNamespace(), MODE_IX);

            // Explain the plan tree.
            auto exec = uassertStatusOK(getExecutorDelete(&CurOp::get(opCtx)->debug(),
                                                          &collection.getCollection(),
                                                          &parsedDelete,
                                                          verbosity));
            auto bodyBuilder = result->getBodyBuilder();
            Explain::explainStages(exec.get(),
                                   collection.getCollection(),
                                   verbosity,
                                   BSONObj(),
                                   _commandObj,
                                   &bodyBuilder);
        }

        const BSONObj& _commandObj;
    };
} cmdDelete;

}  // namespace
}  // namespace mongo
