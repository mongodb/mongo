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

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/commands/write_commands/write_commands_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/doc_validation_error.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
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
#include "mongo/db/repl/tenant_migration_committed_info.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangWriteBeforeWaitingForMigrationDecision);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeCommit);
MONGO_FAIL_POINT_DEFINE(failTimeseriesInsert);

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
    return writeConcern.wMode.empty() && writeConcern.wNumNodes == 0 &&
        (writeConcern.syncMode == WriteConcernOptions::SyncMode::NONE ||
         writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET);
}

/**
 * Returns true if 'ns' refers to a time-series collection.
 */
bool isTimeseries(OperationContext* opCtx, const NamespaceString& ns) {
    auto viewCatalog = DatabaseHolder::get(opCtx)->getViewCatalog(opCtx, ns.db());
    if (!viewCatalog) {
        return false;
    }

    auto view = viewCatalog->lookupWithoutValidatingDurableViews(opCtx, ns.ns());
    if (!view) {
        return false;
    }

    return view->timeseries().has_value();
}

// Default for control.version in time-series bucket collection.
const int kTimeseriesControlVersion = 1;

/**
 * Transforms a single time-series insert to an update request on an existing bucket.
 */
write_ops::UpdateOpEntry makeTimeseriesUpdateOpEntry(const BucketCatalog::BucketId& bucketId,
                                                     const BucketCatalog::CommitData& data,
                                                     const BSONObj& metadata) {
    BSONObjBuilder updateBuilder;
    {
        if (!data.bucketMin.isEmpty() || !data.bucketMax.isEmpty()) {
            BSONObjBuilder controlBuilder(updateBuilder.subobjStart(
                str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "control"));
            if (!data.bucketMin.isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "min", data.bucketMin);
            }
            if (!data.bucketMax.isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "max", data.bucketMax);
            }
        }
    }
    {
        // doc_diff::kSubDiffSectionFieldPrefix + <field name> => {<index_0>: ..., <index_1>: ...}
        StringDataMap<BSONObjBuilder> dataFieldBuilders;
        auto metadataElem = metadata.firstElement();
        DecimalCounter<uint32_t> count(data.numCommittedMeasurements);
        for (const auto& doc : data.docs) {
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
            if (data.newFieldNamesToBeInserted.count(pair.first)) {
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
            if (!data.newFieldNamesToBeInserted.count(pair.first)) {
                dataBuilder.append(doc_diff::kSubDiffSectionFieldPrefix + pair.first.toString(),
                                   BSON(doc_diff::kInsertSectionFieldName << pair.second.obj()));
            }
        }
    }
    write_ops::UpdateModification u(updateBuilder.obj(), write_ops::UpdateModification::DiffTag{});
    write_ops::UpdateOpEntry update(BSON("_id" << *bucketId), std::move(u));
    invariant(!update.getMulti(), bucketId->toString());
    invariant(!update.getUpsert(), bucketId->toString());
    return update;
}

/**
 * Returns the single-element array to use as the vector of documents for inserting a new bucket.
 */
BSONArray makeTimeseriesInsertDocument(const BucketCatalog::BucketId& bucketId,
                                       const BucketCatalog::CommitData& data,
                                       const BSONObj& metadata) {
    auto metadataElem = metadata.firstElement();

    StringDataMap<BSONObjBuilder> dataBuilders;
    DecimalCounter<uint32_t> count;
    for (const auto& doc : data.docs) {
        for (const auto& elem : doc) {
            auto key = elem.fieldNameStringData();
            if (metadataElem && key == metadataElem.fieldNameStringData()) {
                continue;
            }
            dataBuilders[key].appendAs(elem, count);
        }
        ++count;
    }

    BSONArrayBuilder builder;
    {
        BSONObjBuilder bucketBuilder(builder.subobjStart());
        bucketBuilder.append("_id", *bucketId);
        {
            BSONObjBuilder bucketControlBuilder(bucketBuilder.subobjStart("control"));
            bucketControlBuilder.append("version", kTimeseriesControlVersion);
            bucketControlBuilder.append("min", data.bucketMin);
            bucketControlBuilder.append("max", data.bucketMax);
        }
        if (metadataElem) {
            bucketBuilder.appendAs(metadataElem, "meta");
        }
        {
            BSONObjBuilder bucketDataBuilder(bucketBuilder.subobjStart("data"));
            for (auto& dataBuilder : dataBuilders) {
                bucketDataBuilder.append(dataBuilder.first, dataBuilder.second.obj());
            }
        }
    }

    return builder.arr();
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

bool checkFailTimeseriesInsertFailPoint(const BSONObj& metadata) {
    bool shouldFailInsert = false;
    failTimeseriesInsert.executeIf(
        [&](const BSONObj&) { shouldFailInsert = true; },
        [&](const BSONObj& data) {
            BSONElementComparator comp(BSONElementComparator::FieldNamesMode::kIgnore, nullptr);
            return comp.compare(data["metadata"], metadata.firstElement()) == 0;
        });
    return shouldFailInsert;
}

boost::optional<BSONObj> generateError(OperationContext* opCtx,
                                       const StatusWith<SingleWriteResult>& result,
                                       int index,
                                       size_t numErrors) {
    auto status = result.getStatus();
    if (status.isOK()) {
        return boost::none;
    }

    auto errorMessage = [numErrors, errorSize = size_t(0)](StringData rawMessage) mutable {
        // Start truncating error messages once both of these limits are exceeded.
        constexpr size_t kErrorSizeTruncationMin = 1024 * 1024;
        constexpr size_t kErrorCountTruncationMin = 2;
        if (errorSize >= kErrorSizeTruncationMin && numErrors >= kErrorCountTruncationMin) {
            return ""_sd;
        }

        errorSize += rawMessage.size();
        return rawMessage;
    };

    BSONSizeTracker errorsSizeTracker;
    BSONObjBuilder error(errorsSizeTracker);
    error.append("index", index);
    if (auto staleInfo = status.extraInfo<StaleConfigInfo>()) {
        error.append("code", int(ErrorCodes::StaleShardVersion));  // Different from exception!
        {
            BSONObjBuilder errInfo(error.subobjStart("errInfo"));
            staleInfo->serialize(&errInfo);
        }
    } else if (ErrorCodes::DocumentValidationFailure == status.code() && status.extraInfo()) {
        auto docValidationError =
            status.extraInfo<doc_validation_error::DocumentValidationFailureInfo>();
        error.append("code", static_cast<int>(ErrorCodes::DocumentValidationFailure));
        error.append("errInfo", docValidationError->getDetails());
    } else if (ErrorCodes::isTenantMigrationError(status.code())) {
        if (ErrorCodes::TenantMigrationConflict == status.code()) {
            auto migrationConflictInfo = status.extraInfo<TenantMigrationConflictInfo>();

            hangWriteBeforeWaitingForMigrationDecision.pauseWhileSet(opCtx);

            auto mtab = migrationConflictInfo->getTenantMigrationAccessBlocker();

            auto migrationStatus =
                mtab->waitUntilCommittedOrAborted(opCtx, migrationConflictInfo->getOperationType());
            error.append("code", static_cast<int>(migrationStatus.code()));

            // We want to append an empty errmsg for the errors after the first one, so let the
            // code below that appends errmsg do that.
            if (status.reason() != "") {
                error.append("errmsg", errorMessage(migrationStatus.reason()));
            }
            if (migrationStatus.extraInfo()) {
                error.append("errInfo",
                             migrationStatus.extraInfo<TenantMigrationCommittedInfo>()->toBSON());
            }
        } else {
            error.append("code", int(status.code()));
            if (status.extraInfo()) {
                error.append("errInfo", status.extraInfo<TenantMigrationCommittedInfo>()->toBSON());
            }
        }
    } else {
        error.append("code", int(status.code()));
        if (auto const extraInfo = status.extraInfo()) {
            extraInfo->serialize(&error);
        }
    }

    // Skip appending errmsg if it has already been appended like in the case of
    // TenantMigrationConflict.
    if (!error.hasField("errmsg")) {
        error.append("errmsg", errorMessage(status.reason()));
    }
    return error.obj();
}

template <typename CommandReplyType>
class WriteCommand : public Command {
public:
    explicit WriteCommand(StringData name) : Command(name) {}

protected:
    class InvocationBase;

private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    ReadWriteType getReadWriteType() const final {
        return ReadWriteType::kWrite;
    }
};

template <typename CommandReplyType>
class WriteCommand<CommandReplyType>::InvocationBase : public CommandInvocation {
public:
    InvocationBase(const WriteCommand* writeCommand, const OpMsgRequest& request)
        : CommandInvocation(writeCommand), _request(&request) {}

protected:
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
        std::vector<BSONObj> errors;

        for (size_t i = 0; i < result.results.size(); i++) {
            if (auto error = generateError(opCtx, result.results[i], i, errors.size())) {
                errors.push_back(*error);
                continue;
            }

            const auto& opResult = result.results[i].getValue();
            nVal += opResult.getN();  // Always there.

            // Handle custom processing of each result.
            if (hooks && hooks->singleWriteResultHandler)
                hooks->singleWriteResultHandler(opResult, i);
        }

        auto& replyBase = cmdReply->getWriteReplyBase();
        replyBase.setN(nVal);

        if (!errors.empty()) {
            replyBase.setWriteErrors(errors);
        }

        // writeConcernError field is handled by command processor.

        {
            // Undocumented repl fields that mongos depends on.
            auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
            const auto replMode = replCoord->getReplicationMode();
            if (replMode != repl::ReplicationCoordinator::modeNone) {
                replyBase.setOpTime(
                    repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());

                if (replMode == repl::ReplicationCoordinator::modeReplSet) {
                    replyBase.setElectionId(replCoord->getElectionId());
                }
            }
        }

        // Call the called-defined post processing handler.
        if (hooks && hooks->postProcessHandler)
            hooks->postProcessHandler();
    }

    /**
     * Returns true if the retryable time-series write has been executed.
     */
    bool isRetryableTimeseriesWriteExecuted(OperationContext* opCtx,
                                            const write_ops::Insert& insert,
                                            CommandReplyType* reply) const {
        if (!isTimeseriesWriteRetryable(opCtx)) {
            return false;
        }

        if (insert.getDocuments().empty()) {
            return false;
        }

        auto txnParticipant = TransactionParticipant::get(opCtx);
        const auto& writeCommandBase = insert.getWriteCommandBase();

        uassert(
            ErrorCodes::OperationFailed,
            str::stream() << "Retryable time-series insert operations are limited to one document "
                             "per command request",
            insert.getDocuments().size() == 1U);

        auto stmtId = write_ops::getStmtIdForWriteAt(writeCommandBase, 0);
        if (!txnParticipant.checkStatementExecutedNoOplogEntryFetch(stmtId)) {
            return false;
        }

        auto& baseReply = reply->getWriteReplyBase();

        // This retryable write has been executed previously. Fill in command reply before
        // returning.
        baseReply.setN(1);

        auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
        if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet) {
            baseReply.setOpTime(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());
            baseReply.setElectionId(replCoord->getElectionId());
        }

        auto retryStats = RetryableWritesStats::get(opCtx);
        retryStats->incrementRetriedStatementsCount();
        retryStats->incrementRetriedCommandsCount();

        return true;
    }

private:
    // Customization point for 'doCheckAuthorization'.
    virtual void doCheckAuthorizationImpl(AuthorizationSession* authzSession) const = 0;

    // Customization point for 'run'.
    virtual CommandReplyType runImpl(OperationContext* opCtx) = 0;

    void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) final {
        try {
            _transactionChecks(opCtx);
            BSONObjBuilder bob = result->getBodyBuilder();
            CommandReplyType cmdReply = runImpl(opCtx);
            cmdReply.serialize(&bob);
            CommandHelpers::extractOrAppendOk(bob);
        } catch (const DBException& ex) {
            LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
            throw;
        }
    }

    bool supportsWriteConcern() const final {
        return true;
    }

    void doCheckAuthorization(OperationContext* opCtx) const final {
        try {
            doCheckAuthorizationImpl(AuthorizationSession::get(opCtx->getClient()));
        } catch (const DBException& e) {
            LastError::get(opCtx->getClient()).setLastError(e.code(), e.reason());
            throw;
        }
    }

    void _transactionChecks(OperationContext* opCtx) const {
        if (!opCtx->inMultiDocumentTransaction())
            return;
        uassert(50791,
                str::stream() << "Cannot write to system collection " << ns().toString()
                              << " within a transaction.",
                !ns().isSystem() || ns().isPrivilegeCollection());
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassert(50790,
                str::stream() << "Cannot write to unreplicated collection " << ns().toString()
                              << " within a transaction.",
                !replCoord->isOplogDisabledFor(opCtx, ns()));
    }

    const OpMsgRequest* _request;
};

class CmdInsert final : public WriteCommand<write_ops::InsertReply> {
public:
    CmdInsert() : WriteCommand("insert") {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    class Invocation final : public InvocationBase {
    public:
        Invocation(const WriteCommand* cmd, const OpMsgRequest& request)
            : InvocationBase(cmd, request), _batch(InsertOp::parse(request)) {}

        const auto& request() const {
            return _batch;
        }

        bool getBypass() const {
            return request().getBypassDocumentValidation();
        }


    private:
        NamespaceString ns() const override {
            return _batch.getNamespace();
        }

        void doCheckAuthorizationImpl(AuthorizationSession* authzSession) const override {
            auth::checkAuthForInsertCommand(authzSession, getBypass(), _batch);
        }

        StatusWith<SingleWriteResult> _getTimeseriesSingleWriteResult(
            const write_ops_exec::WriteResult& reply) const {
            invariant(reply.results.size() == 1,
                      str::stream() << "Unexpected number of results (" << reply.results.size()
                                    << ") for insert on time-series collection " << ns());

            return reply.results[0];
        }

        StatusWith<SingleWriteResult> _performTimeseriesInsert(
            OperationContext* opCtx,
            const BucketCatalog::BucketId& bucketId,
            const BucketCatalog::CommitData& data,
            const BSONObj& metadata) const {
            if (checkFailTimeseriesInsertFailPoint(metadata)) {
                return {ErrorCodes::FailPointEnabled,
                        "Failed time-series insert due to failTimeseriesInsert fail point"};
            }

            auto bucketsNs = ns().makeTimeseriesBucketsNamespace();

            BSONObjBuilder builder;
            builder.append(write_ops::Insert::kCommandName, bucketsNs.coll());
            // The schema validation configured in the bucket collection is intended for direct
            // operations by end users and is not applicable here.
            builder.append(write_ops::Insert::kBypassDocumentValidationFieldName, true);

            // Statement IDs are not meaningful because of the way we combine and convert inserts
            // for the bucket collection. A retryable write is the only situation where it is
            // appropriate to forward statement IDs.
            if (isTimeseriesWriteRetryable(opCtx)) {
                if (auto stmtId = _batch.getStmtId()) {
                    builder.append(write_ops::Insert::kStmtIdFieldName, *stmtId);
                } else if (auto stmtIds = _batch.getStmtIds()) {
                    builder.append(write_ops::Insert::kStmtIdsFieldName, *stmtIds);
                }
            }

            builder.append(write_ops::Insert::kDocumentsFieldName,
                           makeTimeseriesInsertDocument(bucketId, data, metadata));

            auto request = OpMsgRequest::fromDBAndBody(bucketsNs.db(), builder.obj());
            auto timeseriesInsertBatch = InsertOp::parse(request);

            return _getTimeseriesSingleWriteResult(
                write_ops_exec::performInserts(opCtx, timeseriesInsertBatch));
        }

        StatusWith<SingleWriteResult> _performTimeseriesUpdate(
            OperationContext* opCtx,
            const BucketCatalog::BucketId& bucketId,
            const BucketCatalog::CommitData& data,
            const BSONObj& metadata) const {
            if (checkFailTimeseriesInsertFailPoint(metadata)) {
                return {ErrorCodes::FailPointEnabled,
                        "Failed time-series insert due to failTimeseriesInsert fail point"};
            }

            auto update = makeTimeseriesUpdateOpEntry(bucketId, data, metadata);
            write_ops::Update timeseriesUpdateBatch(ns().makeTimeseriesBucketsNamespace(),
                                                    {update});

            write_ops::WriteCommandBase writeCommandBase;
            // The schema validation configured in the bucket collection is intended for direct
            // operations by end users and is not applicable here.
            writeCommandBase.setBypassDocumentValidation(true);

            // Statement IDs are not meaningful because of the way we combine and convert inserts
            // for the bucket collection. A retryable write is the only situation where it is
            // appropriate to forward statement IDs.
            if (isTimeseriesWriteRetryable(opCtx)) {
                if (auto stmtId = _batch.getStmtId()) {
                    writeCommandBase.setStmtId(*stmtId);
                } else if (auto stmtIds = _batch.getStmtIds()) {
                    writeCommandBase.setStmtIds(*stmtIds);
                }
            }

            timeseriesUpdateBatch.setWriteCommandBase(std::move(writeCommandBase));

            return _getTimeseriesSingleWriteResult(
                write_ops_exec::performUpdates(opCtx, timeseriesUpdateBatch));
        }

        void _commitTimeseriesBucket(OperationContext* opCtx,
                                     const BucketCatalog::BucketId& bucketId,
                                     size_t index,
                                     std::vector<BSONObj>* errors,
                                     boost::optional<repl::OpTime>* opTime,
                                     boost::optional<OID>* electionId,
                                     std::vector<size_t>* updatesToRetryAsInserts) const {
            auto& bucketCatalog = BucketCatalog::get(opCtx);

            auto metadata = bucketCatalog.getMetadata(bucketId);
            auto data = bucketCatalog.commit(bucketId);
            while (!data.docs.empty()) {
                auto result = data.numCommittedMeasurements == 0
                    ? _performTimeseriesInsert(opCtx, bucketId, data, metadata)
                    : _performTimeseriesUpdate(opCtx, bucketId, data, metadata);

                if (data.numCommittedMeasurements != 0 && result.isOK() &&
                    result.getValue().getNModified() == 0) {
                    // No bucket was found to update, meaning that it was manually removed.
                    bucketCatalog.clear(bucketId);
                    updatesToRetryAsInserts->push_back(index);
                    return;
                }

                if (auto error = generateError(opCtx, result, index, errors->size())) {
                    errors->push_back(*error);
                }

                auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
                const auto replMode = replCoord->getReplicationMode();

                *opTime = replMode != repl::ReplicationCoordinator::modeNone
                    ? boost::make_optional(
                          repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp())
                    : boost::none;
                *electionId = replMode == repl::ReplicationCoordinator::modeReplSet
                    ? boost::make_optional(replCoord->getElectionId())
                    : boost::none;

                data = bucketCatalog.commit(
                    bucketId, BucketCatalog::CommitInfo{std::move(result), *opTime, *electionId});
            }
        }

        /**
         * Writes to the underlying system.buckets collection. Returns the indices of the batch
         * which were attempted in an update operation, but found no bucket to update. These indices
         * can be passed as the optional 'indices' parameter in a subsequent call to this function,
         * in order to to be retried as inserts.
         */
        std::vector<size_t> _performUnorderedTimeseriesWrites(
            OperationContext* opCtx,
            size_t start,
            size_t numDocs,
            std::vector<BSONObj>* errors,
            boost::optional<repl::OpTime>* opTime,
            boost::optional<OID>* electionId,
            const boost::optional<std::vector<size_t>>& indices = boost::none) const {
            auto& bucketCatalog = BucketCatalog::get(opCtx);

            std::vector<std::pair<BucketCatalog::BucketId, size_t>> bucketsToCommit;
            std::vector<std::pair<Future<BucketCatalog::CommitInfo>, size_t>> bucketsToWaitOn;
            auto insert = [&](size_t index) {
                auto [bucketId, commitInfo] =
                    bucketCatalog.insert(opCtx, ns(), _batch.getDocuments()[start + index]);
                if (commitInfo) {
                    bucketsToWaitOn.push_back({std::move(*commitInfo), index});
                } else {
                    bucketsToCommit.push_back({std::move(bucketId), index});
                }
            };

            if (indices) {
                std::for_each(indices->begin(), indices->end(), insert);
            } else {
                for (size_t i = 0; i < numDocs; i++) {
                    insert(i);
                }
            }

            hangTimeseriesInsertBeforeCommit.pauseWhileSet();

            std::vector<size_t> updatesToRetryAsInserts;

            for (const auto& [bucketId, index] : bucketsToCommit) {
                _commitTimeseriesBucket(opCtx,
                                        bucketId,
                                        start + index,
                                        errors,
                                        opTime,
                                        electionId,
                                        &updatesToRetryAsInserts);
            }

            for (const auto& [future, index] : bucketsToWaitOn) {
                auto swCommitInfo = future.getNoThrow(opCtx);
                if (!swCommitInfo.isOK()) {
                    invariant(swCommitInfo.getStatus() == ErrorCodes::TimeseriesBucketCleared,
                              str::stream()
                                  << "Got unexpected error (" << swCommitInfo.getStatus()
                                  << ") waiting for time-series bucket to be committed for " << ns()
                                  << ": " << redact(_batch.toBSON({})));

                    updatesToRetryAsInserts.push_back(index);
                    continue;
                }

                const auto& commitInfo = swCommitInfo.getValue();
                if (auto error =
                        generateError(opCtx, commitInfo.result, start + index, errors->size())) {
                    errors->push_back(*error);
                }
                if (commitInfo.opTime) {
                    *opTime = std::max(opTime->value_or(repl::OpTime()), *commitInfo.opTime);
                }
                if (commitInfo.electionId) {
                    *electionId = std::max(electionId->value_or(OID()), *commitInfo.electionId);
                }
            }

            return updatesToRetryAsInserts;
        }

        void _performTimeseriesWritesSubset(OperationContext* opCtx,
                                            size_t start,
                                            size_t numDocs,
                                            std::vector<BSONObj>* errors,
                                            boost::optional<repl::OpTime>* opTime,
                                            boost::optional<OID>* electionId) const {
            auto updatesToRetryAsInserts = _performUnorderedTimeseriesWrites(
                opCtx, start, numDocs, errors, opTime, electionId);
            invariant(
                _performUnorderedTimeseriesWrites(
                    opCtx, start, numDocs, errors, opTime, electionId, updatesToRetryAsInserts)
                    .empty(),
                str::stream() << "Time-series insert on " << ns()
                              << " unexpectedly returned returned updates to retry as inserts "
                                 "after already retrying updates as inserts: "
                              << redact(_batch.toBSON({})));
        }

        void _performTimeseriesWrites(OperationContext* opCtx,
                                      write_ops::InsertReply* insertReply) const {

            if (isRetryableTimeseriesWriteExecuted(opCtx, _batch, insertReply)) {
                return;
            }

            std::vector<BSONObj> errors;
            boost::optional<repl::OpTime> opTime;
            boost::optional<OID> electionId;

            auto& baseReply = insertReply->getWriteReplyBase();

            if (_batch.getOrdered()) {
                for (size_t i = 0; i < _batch.getDocuments().size(); ++i) {
                    _performTimeseriesWritesSubset(opCtx, i, 1, &errors, &opTime, &electionId);
                    if (!errors.empty()) {
                        baseReply.setN(i);
                        break;
                    }
                }
            } else {
                _performTimeseriesWritesSubset(
                    opCtx, 0, _batch.getDocuments().size(), &errors, &opTime, &electionId);
                baseReply.setN(_batch.getDocuments().size() - errors.size());
            }

            if (!errors.empty()) {
                baseReply.setWriteErrors(errors);
            }
            if (opTime) {
                baseReply.setOpTime(*opTime);
            }
            if (electionId) {
                baseReply.setElectionId(*electionId);
            }
        }

        write_ops::InsertReply runImpl(OperationContext* opCtx) override {
            write_ops::InsertReply insertReply;

            if (isTimeseries(opCtx, ns())) {
                // Re-throw parsing exceptions to be consistent with CmdInsert::Invocation's
                // constructor.
                try {
                    _performTimeseriesWrites(opCtx, &insertReply);
                } catch (DBException& ex) {
                    ex.addContext(str::stream() << "time-series insert failed: " << ns().ns());
                    throw;
                }

                return insertReply;
            }
            auto reply = write_ops_exec::performInserts(opCtx, _batch);

            populateReply(opCtx,
                          !_batch.getWriteCommandBase().getOrdered(),
                          _batch.getDocuments().size(),
                          std::move(reply),
                          &insertReply);

            return insertReply;
        }

        write_ops::Insert _batch;
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext*,
                                             const OpMsgRequest& request) override {
        return std::make_unique<Invocation>(this, request);
    }

    void snipForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "documents");
    }

    std::string help() const final {
        return "insert documents";
    }
} cmdInsert;

class CmdUpdate final : public WriteCommand<write_ops::UpdateReply> {
public:
    CmdUpdate() : WriteCommand("update"), _updateMetrics{"update"} {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    class Invocation final : public InvocationBase {
    public:
        Invocation(const WriteCommand* cmd,
                   const OpMsgRequest& request,
                   UpdateMetrics* updateMetrics)
            : InvocationBase(cmd, request),
              _batch(UpdateOp::parse(request)),
              _commandObj(request.body),
              _updateMetrics{updateMetrics} {

            invariant(_commandObj.isOwned());
            invariant(_updateMetrics);

            // Extend the lifetime of `updates` to allow asynchronous mirroring.
            if (auto seq = request.getSequence("updates"_sd); seq && !seq->objs.empty()) {
                // Current design ignores contents of `updates` array except for the first entry.
                // Assuming identical collation for all elements in `updates`, future design could
                // use the disjunction primitive (i.e, `$or`) to compile all queries into a single
                // filter. Such a design also requires a sound way of combining hints.
                invariant(seq->objs.front().isOwned());
                _updateOpObj = seq->objs.front();
            }
        }

        const auto& request() const {
            return _batch;
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

            bob->append("find", _commandObj["update"].String());
            extractQueryDetails(_updateOpObj, bob);
            bob->append("batchSize", 1);
            bob->append("singleBatch", true);
        }

    private:
        NamespaceString ns() const override {
            return _batch.getNamespace();
        }

        void doCheckAuthorizationImpl(AuthorizationSession* authzSession) const override {
            auth::checkAuthForUpdateCommand(authzSession, getBypass(), _batch);
        }

        write_ops::UpdateReply runImpl(OperationContext* opCtx) override {
            write_ops::UpdateReply updateReply;
            long long nModified = 0;

            // Tracks the upserted information. The memory of this variable gets moved in the
            // 'postProcessHandler' and should not be accessed afterwards.
            std::vector<write_ops::Upserted> upsertedInfoVec;

            auto reply = write_ops_exec::performUpdates(opCtx, _batch);

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
                          !_batch.getWriteCommandBase().getOrdered(),
                          _batch.getUpdates().size(),
                          std::move(reply),
                          &updateReply,
                          PopulateReplyHooks{singleWriteHandler, postProcessHandler});

            // Collect metrics.
            for (auto&& update : _batch.getUpdates()) {
                // If this was a pipeline style update, record that pipeline-style was used and
                // which stages were being used.
                auto& updateMod = update.getU();
                if (updateMod.type() == write_ops::UpdateModification::Type::kPipeline) {
                    AggregateCommand request(_batch.getNamespace(), updateMod.getUpdatePipeline());
                    LiteParsedPipeline pipeline(request);
                    pipeline.tickGlobalStageCounters();
                    _updateMetrics->incrementExecutedWithAggregationPipeline();
                }

                // If this command had arrayFilters option, record that it was used.
                if (update.getArrayFilters()) {
                    _updateMetrics->incrementExecutedWithArrayFilters();
                }
            }

            return updateReply;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    _batch.getUpdates().size() == 1);

            UpdateRequest updateRequest(_batch.getUpdates()[0]);
            updateRequest.setNamespaceString(_batch.getNamespace());
            updateRequest.setLegacyRuntimeConstants(_batch.getLegacyRuntimeConstants().value_or(
                Variables::generateRuntimeConstants(opCtx)));
            updateRequest.setLetParameters(_batch.getLet());
            updateRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
            updateRequest.setExplain(verbosity);

            const ExtensionsCallbackReal extensionsCallback(opCtx,
                                                            &updateRequest.getNamespaceString());
            ParsedUpdate parsedUpdate(opCtx, &updateRequest, extensionsCallback);
            uassertStatusOK(parsedUpdate.parseRequest());

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetCollection collection(opCtx, _batch.getNamespace(), MODE_IX);

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

        write_ops::Update _batch;

        BSONObj _commandObj;

        // Holds a shared pointer to the first entry in `updates` array.
        BSONObj _updateOpObj;

        // Update related command execution metrics.
        UpdateMetrics* const _updateMetrics;
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext*, const OpMsgRequest& request) {
        return std::make_unique<Invocation>(this, request, &_updateMetrics);
    }

    void snipForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "updates");
    }

    std::string help() const final {
        return "update documents";
    }

    // Update related command execution metrics.
    UpdateMetrics _updateMetrics;
} cmdUpdate;

class CmdDelete final : public WriteCommand<write_ops::DeleteReply> {
public:
    CmdDelete() : WriteCommand("delete") {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    class Invocation final : public InvocationBase {
    public:
        Invocation(const WriteCommand* cmd, const OpMsgRequest& request)
            : InvocationBase(cmd, request),
              _batch(DeleteOp::parse(request)),
              _commandObj(request.body) {}

        const auto& request() const {
            return _batch;
        }

        bool getBypass() const {
            return request().getBypassDocumentValidation();
        }


    private:
        NamespaceString ns() const override {
            return _batch.getNamespace();
        }

        void doCheckAuthorizationImpl(AuthorizationSession* authzSession) const override {
            auth::checkAuthForDeleteCommand(authzSession, getBypass(), _batch);
        }

        write_ops::DeleteReply runImpl(OperationContext* opCtx) override {
            write_ops::DeleteReply deleteReply;

            auto reply = write_ops_exec::performDeletes(opCtx, _batch);
            populateReply(opCtx,
                          !_batch.getWriteCommandBase().getOrdered(),
                          _batch.getDeletes().size(),
                          std::move(reply),
                          &deleteReply);

            return deleteReply;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    _batch.getDeletes().size() == 1);

            auto deleteRequest = DeleteRequest{};
            deleteRequest.setNsString(_batch.getNamespace());
            deleteRequest.setLegacyRuntimeConstants(_batch.getLegacyRuntimeConstants().value_or(
                Variables::generateRuntimeConstants(opCtx)));
            deleteRequest.setLet(_batch.getLet());
            deleteRequest.setQuery(_batch.getDeletes()[0].getQ());
            deleteRequest.setCollation(write_ops::collationOf(_batch.getDeletes()[0]));
            deleteRequest.setMulti(_batch.getDeletes()[0].getMulti());
            deleteRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
            deleteRequest.setHint(_batch.getDeletes()[0].getHint());
            deleteRequest.setIsExplain(true);

            ParsedDelete parsedDelete(opCtx, &deleteRequest);
            uassertStatusOK(parsedDelete.parseRequest());

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetCollection collection(opCtx, _batch.getNamespace(), MODE_IX);

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

        write_ops::Delete _batch;

        const BSONObj& _commandObj;
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext*,
                                             const OpMsgRequest& request) override {
        return std::make_unique<Invocation>(this, request);
    }

    void snipForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "deletes");
    }

    std::string help() const final {
        return "delete documents";
    }
} cmdDelete;

}  // namespace
}  // namespace mongo
