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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/commands/write_commands_common.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/process_interface/replica_set_node_process_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangInsertBeforeWrite);
MONGO_FAIL_POINT_DEFINE(hangUpdateBeforeWrite);

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
            lastResult == ErrorCodes::ShardCannotRefreshDueToLocksHeld ||
            ErrorCodes::isStaleShardVersionError(lastResult.getStatus()) ||
            ErrorCodes::isTenantMigrationError(lastResult.getStatus()) ||
            lastResult == ErrorCodes::CannotImplicitlyCreateCollection) {
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
        if (replCoord->getSettings().isReplSet()) {
            replyBase.setOpTime(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());
            replyBase.setElectionId(replCoord->getElectionId());
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

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        write_ops::InsertCommandReply typedRun(OperationContext* opCtx) final try {
            // On debug builds, verify that the estimated size of the insert command is at least as
            // large as the size of the actual, serialized insert command. This ensures that the
            // logic which estimates the size of insert commands is correct.
            dassert(write_ops::verifySizeEstimate(request(), &unparsedRequest()));

            doTransactionValidationForWrites(opCtx, ns());
            if (request().getEncryptionInformation().has_value()) {
                {
                    // Flag set here and in fle_crud.cpp since this only executes on a mongod.
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
                }

                if (!request().getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    write_ops::InsertCommandReply insertReply;
                    auto batch = processFLEInsert(opCtx, request(), &insertReply);
                    if (batch == FLEBatchResult::kProcessed) {
                        return insertReply;
                    }
                }
            }

            if (auto [isTimeseriesViewRequest, _] =
                    timeseries::isTimeseriesViewRequest(opCtx, request());
                isTimeseriesViewRequest) {
                // Re-throw parsing exceptions to be consistent with CmdInsert::Invocation's
                // constructor.
                try {
                    return write_ops_exec::performTimeseriesWrites(opCtx, request());
                } catch (DBException& ex) {
                    ex.addContext(str::stream()
                                  << "time-series insert failed: " << ns().toStringForErrorMsg());
                    throw;
                }
            }

            boost::optional<ScopedAdmissionPriorityForLock> priority;
            if (request().getNamespace() == NamespaceString::kConfigSampledQueriesNamespace ||
                request().getNamespace() == NamespaceString::kConfigSampledQueriesDiffNamespace) {
                priority.emplace(shard_role_details::getLocker(opCtx),
                                 AdmissionContext::Priority::kLow);
            }

            if (hangInsertBeforeWrite.shouldFail([&](const BSONObj& data) {
                    const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "ns"_sd);
                    return fpNss == request().getNamespace();
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
};
MONGO_REGISTER_COMMAND(CmdInsert).forShard();

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

        bool isSubjectToIngressAdmissionControl() const override {
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


            bob->append("find", _commandObj["update"].String());
            extractQueryDetails(_updateOpObj, bob);
            bob->append("batchSize", 1);
            bob->append("singleBatch", true);

            if (const auto& shardVersion = _commandObj.getField("shardVersion");
                !shardVersion.eoo()) {
                bob->append(shardVersion);
            }
            if (const auto& databaseVersion = _commandObj.getField("databaseVersion");
                !databaseVersion.eoo()) {
                bob->append(databaseVersion);
            }
            if (const auto& encryptionInfo = _commandObj.getField("encryptionInformation");
                !encryptionInfo.eoo()) {
                bob->append(encryptionInfo);
            }
        }

        write_ops::UpdateCommandReply typedRun(OperationContext* opCtx) final try {
            // On debug builds, verify that the estimated size of the update command is at least as
            // large as the size of the actual, serialized update command. This ensures that the
            // logic which estimates the size of update commands is correct.
            dassert(write_ops::verifySizeEstimate(request(), &unparsedRequest()));

            doTransactionValidationForWrites(opCtx, ns());
            write_ops::UpdateCommandReply updateReply;
            if (request().getEncryptionInformation().has_value()) {
                {
                    // Flag set here and in fle_crud.cpp since this only executes on a mongod.
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
                }
                if (!request().getEncryptionInformation().value().getCrudProcessed()) {
                    return processFLEUpdate(opCtx, request());
                }
            }

            auto [isTimeseriesViewRequest, bucketNs] =
                timeseries::isTimeseriesViewRequest(opCtx, request());
            OperationSource source = isTimeseriesViewRequest ? OperationSource::kTimeseriesUpdate
                                                             : OperationSource::kStandard;

            long long nModified = 0;

            // Tracks the upserted information. The memory of this variable gets moved in the
            // 'postProcessHandler' and should not be accessed afterwards.
            std::vector<write_ops::Upserted> upsertedInfoVec;

            write_ops_exec::WriteResult reply;
            // For retryable updates on time-series collections, we needs to run them in
            // transactions to ensure the multiple writes are replicated atomically.
            bool isTimeseriesRetryableUpdate = isTimeseriesViewRequest &&
                opCtx->isRetryableWrite() && !opCtx->inMultiDocumentTransaction();
            if (isTimeseriesRetryableUpdate) {
                auto executor = serverGlobalParams.clusterRole.has(ClusterRole::None)
                    ? ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(
                          opCtx->getServiceContext())
                    : Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
                ON_BLOCK_EXIT([&] {
                    // Increments the counter if the command contains retries. This is normally done
                    // within write_ops_exec::performUpdates. But for retryable timeseries updates,
                    // we should handle the metrics only once at the caller since each statement
                    // will be run as a separate update command through the internal transaction
                    // API. See write_ops_exec::performUpdates for more details.
                    if (!reply.retriedStmtIds.empty()) {
                        RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
                    }
                });
                write_ops_exec::runTimeseriesRetryableUpdates(
                    opCtx, bucketNs, request(), executor, &reply);
            } else {
                if (hangUpdateBeforeWrite.shouldFail([&](const BSONObj& data) {
                        const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "ns"_sd);
                        return fpNss == request().getNamespace();
                    })) {
                    hangUpdateBeforeWrite.pauseWhileSet();
                }

                reply = write_ops_exec::performUpdates(opCtx, request(), source);
            }

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
            // For time-series retryable updates, the metrics are already incremented when running
            // the internal transaction. Avoids updating them twice.
            if (!isTimeseriesRetryableUpdate) {
                for (auto&& update : request().getUpdates()) {
                    incrementUpdateMetrics(update.getU(),
                                           request().getNamespace(),
                                           _getUpdateMetrics(),
                                           update.getArrayFilters());
                }
            }

            return updateReply;
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
        }

    private:
        UpdateMetrics& _getUpdateMetrics() const {
            return *static_cast<const CmdUpdate&>(*definition())._updateMetrics;
        }

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

            auto [isTimeseriesViewRequest, nss] =
                timeseries::isTimeseriesViewRequest(opCtx, request());

            UpdateRequest updateRequest(request().getUpdates()[0]);
            updateRequest.setNamespaceString(nss);
            if (shouldDoFLERewrite(request())) {
                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
                }

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

            write_ops_exec::explainUpdate(opCtx,
                                          updateRequest,
                                          isTimeseriesViewRequest,
                                          request().getSerializationContext(),
                                          _commandObj,
                                          verbosity,
                                          result);
        }

        BSONObj _commandObj;

        // Holds a shared pointer to the first entry in `updates` array.
        BSONObj _updateOpObj;
    };

protected:
    void doInitializeClusterRole(ClusterRole role) override {
        write_ops::UpdateCmdVersion1Gen<CmdUpdate>::doInitializeClusterRole(role);
        _updateMetrics.emplace(getName(), role);
    }

    // Update related command execution metrics.
    mutable boost::optional<UpdateMetrics> _updateMetrics;
};
MONGO_REGISTER_COMMAND(CmdUpdate).forShard();

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

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        write_ops::DeleteCommandReply typedRun(OperationContext* opCtx) final try {
            // On debug builds, verify that the estimated size of the deletes are at least as large
            // as the actual, serialized size. This ensures that the logic that estimates the size
            // of deletes for batch writes is correct.
            dassert(write_ops::verifySizeEstimate(request(), &unparsedRequest()));

            doTransactionValidationForWrites(opCtx, ns());
            write_ops::DeleteCommandReply deleteReply;
            OperationSource source = OperationSource::kStandard;

            if (request().getEncryptionInformation().has_value()) {
                {
                    // Flag set here and in fle_crud.cpp since this only executes on a mongod.
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
                }

                if (!request().getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    return processFLEDelete(opCtx, request());
                }
            }

            if (auto [isTimeseriesViewRequest, _] =
                    timeseries::isTimeseriesViewRequest(opCtx, request());
                isTimeseriesViewRequest) {
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

            auto [isTimeseriesViewRequest, nss] =
                timeseries::isTimeseriesViewRequest(opCtx, request());

            auto deleteRequest = DeleteRequest{};
            deleteRequest.setNsString(nss);
            deleteRequest.setLegacyRuntimeConstants(request().getLegacyRuntimeConstants().value_or(
                Variables::generateRuntimeConstants(opCtx)));
            deleteRequest.setLet(request().getLet());

            const auto& firstDelete = request().getDeletes()[0];
            BSONObj query = firstDelete.getQ();
            if (shouldDoFLERewrite(request())) {
                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
                }

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

            write_ops_exec::explainDelete(opCtx,
                                          deleteRequest,
                                          isTimeseriesViewRequest,
                                          request().getSerializationContext(),
                                          _commandObj,
                                          verbosity,
                                          result);
        }

        const BSONObj& _commandObj;
    };
};
MONGO_REGISTER_COMMAND(CmdDelete).forShard();

}  // namespace
}  // namespace mongo
