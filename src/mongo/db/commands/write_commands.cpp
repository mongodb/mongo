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
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
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
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangInsertBeforeWrite);

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
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);
    auto coll = catalog->lookupCollectionByNamespace(opCtx, bucketNss);
    return (coll && coll->getTimeseriesOptions());
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
        if (auto error = write_ops_exec::generateError(
                opCtx, result.results[i].getStatus(), i, errors.size())) {
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
            if (request().getEncryptionInformation().has_value()) {
                // Flag set here and in fle_crud.cpp since this only executes on a mongod.
                CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

                if (!request().getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    write_ops::InsertCommandReply insertReply;
                    auto batch = processFLEInsert(opCtx, request(), &insertReply);
                    if (batch == FLEBatchResult::kProcessed) {
                        return insertReply;
                    }
                }
            }

            if (isTimeseries(opCtx, request())) {
                // Re-throw parsing exceptions to be consistent with CmdInsert::Invocation's
                // constructor.
                try {
                    return write_ops_exec::performTimeseriesWrites(opCtx, request());
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
        void doCheckAuthorization(OperationContext* opCtx) const final try {
            auth::checkAuthForInsertCommand(AuthorizationSession::get(opCtx->getClient()),
                                            request().getBypassDocumentValidation(),
                                            request());
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
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
            if (request().getEncryptionInformation().has_value()) {
                // Flag set here and in fle_crud.cpp since this only executes on a mongod.
                CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;
                if (!request().getEncryptionInformation().value().getCrudProcessed()) {
                    return processFLEUpdate(opCtx, request());
                }
            }

            if (isTimeseries(opCtx, request())) {
                uassert(ErrorCodes::OperationNotSupportedInTransaction,
                        str::stream() << "Cannot perform a multi-document transaction on a "
                                         "time-series collection: "
                                      << ns(),
                        !opCtx->inMultiDocumentTransaction());
                source = OperationSource::kTimeseriesUpdate;
            }

            // On debug builds, verify that the estimated size of the updates are at least as large
            // as the actual, serialized size. This ensures that the logic that estimates the size
            // of deletes for batch writes is correct.
            if constexpr (kDebugBuild) {
                for (auto&& updateOp : request().getUpdates()) {
                    invariant(write_ops::verifySizeEstimate(updateOp));
                }
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
                CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

                if (!request().getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    updateRequest.setQuery(
                        processFLEWriteExplainD(opCtx,
                                                write_ops::collationOf(request().getUpdates()[0]),
                                                request(),
                                                updateRequest.getQuery()));
                }
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
                // Flag set here and in fle_crud.cpp since this only executes on a mongod.
                CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

                if (!request().getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    return processFLEDelete(opCtx, request());
                }
            }

            if (isTimeseries(opCtx, request())) {
                uassert(ErrorCodes::OperationNotSupportedInTransaction,
                        str::stream() << "Cannot perform a multi-document transaction on a "
                                         "time-series collection: "
                                      << ns(),
                        !opCtx->inMultiDocumentTransaction());
                source = OperationSource::kTimeseriesDelete;
            }

            // On debug builds, verify that the estimated size of the deletes are at least as large
            // as the actual, serialized size. This ensures that the logic that estimates the size
            // of deletes for batch writes is correct.
            if constexpr (kDebugBuild) {
                for (auto&& deleteOp : request().getDeletes()) {
                    invariant(write_ops::verifySizeEstimate(deleteOp));
                }
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
            auto isRequestToTimeseries = isTimeseries(opCtx, request());
            auto nss = [&] {
                auto nss = request().getNamespace();
                if (!isRequestToTimeseries) {
                    return nss;
                }
                return nss.isTimeseriesBucketsCollection() ? nss
                                                           : nss.makeTimeseriesBucketsNamespace();
            }();
            deleteRequest.setNsString(nss);
            deleteRequest.setLegacyRuntimeConstants(request().getLegacyRuntimeConstants().value_or(
                Variables::generateRuntimeConstants(opCtx)));
            deleteRequest.setLet(request().getLet());

            const auto& firstDelete = request().getDeletes()[0];
            BSONObj query = firstDelete.getQ();
            if (shouldDoFLERewrite(request())) {
                CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

                if (!request().getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    query = processFLEWriteExplainD(
                        opCtx, write_ops::collationOf(firstDelete), request(), query);
                }
            }
            deleteRequest.setQuery(std::move(query));

            deleteRequest.setCollation(write_ops::collationOf(request().getDeletes()[0]));
            deleteRequest.setMulti(firstDelete.getMulti());
            deleteRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
            deleteRequest.setHint(firstDelete.getHint());
            deleteRequest.setIsExplain(true);

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetCollection collection(opCtx, deleteRequest.getNsString(), MODE_IX);

            if (isRequestToTimeseries) {
                uassert(ErrorCodes::NamespaceNotFound,
                        "Could not find time-series buckets collection for write explain",
                        *collection);
                auto timeseriesOptions = collection->getTimeseriesOptions();
                uassert(ErrorCodes::InvalidOptions,
                        "Time-series buckets collection is missing time-series options",
                        timeseriesOptions);

                if (timeseries::isHintIndexKey(firstDelete.getHint())) {
                    deleteRequest.setHint(
                        uassertStatusOK(timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
                            *timeseriesOptions, firstDelete.getHint())));
                }
            }

            ParsedDelete parsedDelete(opCtx,
                                      &deleteRequest,
                                      isRequestToTimeseries && collection
                                          ? collection->getTimeseriesOptions()
                                          : boost::none);
            uassertStatusOK(parsedDelete.parseRequest());

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
