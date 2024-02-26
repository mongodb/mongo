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

#include <boost/smart_ptr.hpp>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/parsed_writes_common.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/update/update_util.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failAllFindAndModify);
MONGO_FAIL_POINT_DEFINE(hangBeforeFindAndModifyPerformsUpdate);

void validate(const write_ops::FindAndModifyCommandRequest& request) {
    uassert(ErrorCodes::FailedToParse,
            "Either an update or remove=true must be specified",
            request.getRemove().value_or(false) || request.getUpdate());
    if (request.getRemove().value_or(false)) {
        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both an update and remove=true",
                !request.getUpdate());

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both upsert=true and remove=true ",
                !request.getUpsert() || !*request.getUpsert());

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both new=true and remove=true; 'remove' always returns the deleted "
                "document",
                !request.getNew() || !*request.getNew());

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify arrayFilters and remove=true",
                !request.getArrayFilters());
    }

    if (request.getUpdate() &&
        request.getUpdate()->type() == write_ops::UpdateModification::Type::kPipeline &&
        request.getArrayFilters()) {
        uasserted(ErrorCodes::FailedToParse, "Cannot specify arrayFilters and a pipeline update");
    }
}

void makeDeleteRequest(OperationContext* opCtx,
                       const write_ops::FindAndModifyCommandRequest& request,
                       bool explain,
                       DeleteRequest* requestOut) {
    requestOut->setQuery(request.getQuery());
    requestOut->setProj(request.getFields().value_or(BSONObj()));
    requestOut->setLegacyRuntimeConstants(
        request.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    requestOut->setLet(request.getLet());
    requestOut->setSort(request.getSort().value_or(BSONObj()));
    requestOut->setHint(request.getHint());
    requestOut->setCollation(request.getCollation().value_or(BSONObj()));
    requestOut->setMulti(false);
    requestOut->setReturnDeleted(true);  // Always return the old value.
    requestOut->setIsExplain(explain);

    requestOut->setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    requestOut->setIsTimeseriesNamespace(request.getIsTimeseriesNamespace());
}

write_ops::FindAndModifyCommandReply buildResponse(
    const boost::optional<UpdateResult>& updateResult,
    bool isRemove,
    const boost::optional<BSONObj>& value) {
    write_ops::FindAndModifyLastError lastError;
    if (isRemove) {
        lastError.setNumDocs(value ? 1 : 0);
    } else {
        invariant(updateResult);
        lastError.setNumDocs(!updateResult->upsertedId.isEmpty() ? 1 : updateResult->numMatched);
        lastError.setUpdatedExisting(updateResult->numMatched > 0);

        // Note we have to use the upsertedId from the update result here, rather than 'value'
        // because the _id field could have been excluded by a projection.
        if (!updateResult->upsertedId.isEmpty()) {
            lastError.setUpserted(IDLAnyTypeOwned(updateResult->upsertedId.firstElement()));
        }
    }

    write_ops::FindAndModifyCommandReply result;
    result.setLastErrorObject(std::move(lastError));
    result.setValue(value);
    return result;
}

void assertCanWrite_inlock(OperationContext* opCtx, const NamespaceString& nss) {
    uassert(ErrorCodes::NotWritablePrimary,
            str::stream() << "Not primary while running findAndModify command on collection "
                          << nss.toStringForErrorMsg(),
            repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

    CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
        ->checkShardVersionOrThrow(opCtx);
}

void recordStatsForTopCommand(OperationContext* opCtx) {
    auto curOp = CurOp::get(opCtx);
    Top::get(opCtx->getClient()->getServiceContext())
        .record(opCtx,
                curOp->getNSS(),
                curOp->getLogicalOp(),
                Top::LockType::WriteLocked,
                durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                curOp->isCommand(),
                curOp->getReadWriteType());
}

void checkIfTransactionOnCappedColl(const CollectionPtr& coll, bool inTransaction) {
    if (coll && coll->isCapped()) {
        uassert(
            ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Collection '" << coll->ns().toStringForErrorMsg()
                          << "' is a capped collection. Writes in transactions are not allowed on "
                             "capped collections.",
            !inTransaction);
    }
}

class CmdFindAndModify : public write_ops::FindAndModifyCmdVersion1Gen<CmdFindAndModify> {
public:
    std::string help() const final {
        return "{ findAndModify: \"collection\", query: {processed:false}, update: {$set: "
               "{processed:true}}, new: true}\n"
               "{ findAndModify: \"collection\", query: {processed:false}, remove: true, sort: "
               "{priority:-1}}\n"
               "Either update or remove is required, all other fields have default values.\n"
               "Output is in the \"value\" field\n";
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext* srvContext) const final {
        return Command::AllowedOnSecondary::kNever;
    }

    Command::ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    void collectMetrics(const Request& request) const {
        _updateMetrics->collectMetrics(request);
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        bool supportsReadMirroring() const final {
            return true;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        NamespaceString ns() const final {
            return this->request().getNamespace();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final;

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) final;

        Reply typedRun(OperationContext* opCtx) final;

        void appendMirrorableRequest(BSONObjBuilder* bob) const final;
    };

protected:
    void doInitializeClusterRole(ClusterRole role) override {
        write_ops::FindAndModifyCmdVersion1Gen<CmdFindAndModify>::doInitializeClusterRole(role);
        _updateMetrics.emplace(getName(), role);
    }

private:
    // Update related command execution metrics.
    mutable boost::optional<UpdateMetrics> _updateMetrics;
};
MONGO_REGISTER_COMMAND(CmdFindAndModify).forShard();

void CmdFindAndModify::Invocation::doCheckAuthorization(OperationContext* opCtx) const {
    std::vector<Privilege> privileges;
    const auto& request = this->request();

    ActionSet actions;
    actions.addAction(ActionType::find);

    if (request.getUpdate()) {
        actions.addAction(ActionType::update);
    }
    if (request.getUpsert().value_or(false)) {
        actions.addAction(ActionType::insert);
    }
    if (request.getRemove().value_or(false)) {
        actions.addAction(ActionType::remove);
    }
    if (request.getBypassDocumentValidation().value_or(false)) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    ResourcePattern resource(CommandHelpers::resourcePatternForNamespace(request.getNamespace()));
    uassert(17138,
            "Invalid target namespace " + resource.toString(),
            resource.isExactNamespacePattern());
    privileges.push_back(Privilege(resource, actions));

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to find and modify on database'"
                          << this->request().getDbName().toStringForErrorMsg() << "'",
            AuthorizationSession::get(opCtx->getClient())->isAuthorizedForPrivileges(privileges));
}

void CmdFindAndModify::Invocation::explain(OperationContext* opCtx,
                                           ExplainOptions::Verbosity verbosity,
                                           rpc::ReplyBuilderInterface* result) {
    validate(request());
    const BSONObj& cmdObj = request().toBSON(BSONObj() /* commandPassthroughFields */);

    auto requestAndMsg = [&]() {
        if (request().getEncryptionInformation()) {
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
            }

            if (!request().getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                return processFLEFindAndModifyExplainMongod(opCtx, request());
            }
        }

        return std::pair{request(), OpMsgRequest()};
    }();
    auto request = requestAndMsg.first;

    auto [isTimeseriesViewRequest, nss] = timeseries::isTimeseriesViewRequest(opCtx, request);

    uassertStatusOK(userAllowedWriteNS(opCtx, nss));
    auto const curOp = CurOp::get(opCtx);
    OpDebug* const opDebug = &curOp->debug();
    auto const dbName = request.getDbName();

    if (request.getRemove().value_or(false)) {
        auto deleteRequest = DeleteRequest{};
        deleteRequest.setNsString(nss);
        const bool isExplain = true;
        makeDeleteRequest(opCtx, request, isExplain, &deleteRequest);

        // Explain calls of the findAndModify command are read-only, but we take write
        // locks so that the timing information is more accurate.
        const auto collection =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx, nss, AcquisitionPrerequisites::OperationType::kWrite),
                              MODE_IX);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName.toStringForErrorMsg() << " does not exist",
                DatabaseHolder::get(opCtx)->getDb(opCtx, nss.dbName()));

        if (isTimeseriesViewRequest) {
            timeseries::timeseriesRequestChecks<DeleteRequest>(
                collection.getCollectionPtr(),
                &deleteRequest,
                timeseries::deleteRequestCheckFunction);
            timeseries::timeseriesHintTranslation<DeleteRequest>(collection.getCollectionPtr(),
                                                                 &deleteRequest);
        }

        ParsedDelete parsedDelete(
            opCtx, &deleteRequest, collection.getCollectionPtr(), isTimeseriesViewRequest);
        uassertStatusOK(parsedDelete.parseRequest());

        CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
            ->checkShardVersionOrThrow(opCtx);

        const auto exec =
            uassertStatusOK(getExecutorDelete(opDebug, collection, &parsedDelete, verbosity));

        auto bodyBuilder = result->getBodyBuilder();
        Explain::explainStages(
            exec.get(),
            collection.getCollectionPtr(),
            verbosity,
            BSONObj(),
            SerializationContext::stateCommandReply(request.getSerializationContext()),
            cmdObj,
            &bodyBuilder);
    } else {
        auto updateRequest = UpdateRequest();
        updateRequest.setNamespaceString(nss);
        update::makeUpdateRequest(opCtx, request, verbosity, &updateRequest);

        // Explain calls of the findAndModify command are read-only, but we take write
        // locks so that the timing information is more accurate.
        const auto collection =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx, nss, AcquisitionPrerequisites::OperationType::kWrite),
                              MODE_IX);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName.toStringForErrorMsg() << " does not exist",
                DatabaseHolder::get(opCtx)->getDb(opCtx, nss.dbName()));
        if (isTimeseriesViewRequest) {
            timeseries::timeseriesRequestChecks<UpdateRequest>(
                collection.getCollectionPtr(),
                &updateRequest,
                timeseries::updateRequestCheckFunction);
            timeseries::timeseriesHintTranslation<UpdateRequest>(collection.getCollectionPtr(),
                                                                 &updateRequest);
        }

        ParsedUpdate parsedUpdate(opCtx,
                                  &updateRequest,
                                  collection.getCollectionPtr(),
                                  false /*forgoOpCounterIncrements*/,
                                  isTimeseriesViewRequest);
        uassertStatusOK(parsedUpdate.parseRequest());

        CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
            ->checkShardVersionOrThrow(opCtx);

        const auto exec =
            uassertStatusOK(getExecutorUpdate(opDebug, collection, &parsedUpdate, verbosity));

        auto bodyBuilder = result->getBodyBuilder();
        Explain::explainStages(
            exec.get(),
            collection.getCollectionPtr(),
            verbosity,
            BSONObj(),
            SerializationContext::stateCommandReply(request.getSerializationContext()),
            cmdObj,
            &bodyBuilder);
    }
}

write_ops::FindAndModifyCommandReply CmdFindAndModify::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& req = request();

    validate(req);

    auto& curOp = *CurOp::get(opCtx);

    if (req.getEncryptionInformation().has_value()) {
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp.setShouldOmitDiagnosticInformation_inlock(lk, true);
        }
        if (!req.getEncryptionInformation()->getCrudProcessed().get_value_or(false)) {
            return processFLEFindAndModify(opCtx, req);
        }
    }

    const NamespaceString& nsString = req.getNamespace();
    uassertStatusOK(userAllowedWriteNS(opCtx, nsString));

    static_cast<const CmdFindAndModify*>(definition())->collectMetrics(req);

    auto disableDocumentValidation = req.getBypassDocumentValidation().value_or(false);
    auto fleCrudProcessed = write_ops_exec::getFleCrudProcessed(
        opCtx, req.getEncryptionInformation(), nsString.tenantId());

    DisableDocumentSchemaValidationIfTrue docSchemaValidationDisabler(opCtx,
                                                                      disableDocumentValidation);

    DisableSafeContentValidationIfTrue safeContentValidationDisabler(
        opCtx, disableDocumentValidation, fleCrudProcessed);

    doTransactionValidationForWrites(opCtx, ns());

    const auto stmtId = req.getStmtId().value_or(0);
    if (opCtx->isRetryableWrite()) {
        const auto txnParticipant = TransactionParticipant::get(opCtx);
        if (auto entry = txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
            RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
            RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();

            // Use a SideTransactionBlock since 'parseOplogEntryForFindAndModify' might need to
            // fetch a pre/post image from the oplog and if this is a retry inside an in-progress
            // retryable internal transaction, this 'opCtx' would have an active WriteUnitOfWork
            // and it is illegal to read the the oplog when there is an active WriteUnitOfWork.
            TransactionParticipant::SideTransactionBlock sideTxn(opCtx);
            auto findAndModifyReply = parseOplogEntryForFindAndModify(opCtx, req, *entry);
            findAndModifyReply.setRetriedStmtId(stmtId);

            // Make sure to wait for writeConcern on the opTime that will include this
            // write. Needs to set to the system last opTime to get the latest term in an
            // event when an election happened after the actual write.
            auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
            replClient.setLastOpToSystemLastOpTime(opCtx);

            return findAndModifyReply;
        }
    }

    // Initialize curOp information.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        if (req.getIsTimeseriesNamespace() && nsString.isTimeseriesBucketsCollection()) {
            auto viewNss = nsString.getTimeseriesViewNamespace();
            curOp.setNS_inlock(viewNss);
            curOp.setOpDescription_inlock(timeseries::timeseriesViewCommand(
                unparsedRequest().body, "findAndModify", viewNss.coll()));
        } else {
            curOp.setNS_inlock(nsString);
        }
        curOp.ensureStarted();
    }

    auto sampleId = analyze_shard_key::getOrGenerateSampleId(
        opCtx, ns(), analyze_shard_key::SampledCommandNameEnum::kFindAndModify, req);
    if (sampleId) {
        analyze_shard_key::QueryAnalysisWriter::get(opCtx)
            ->addFindAndModifyQuery(opCtx, *sampleId, req)
            .getAsync([](auto) {});
    }

    if (MONGO_unlikely(failAllFindAndModify.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllFindAndModify failpoint active!");
    }

    const bool inTransaction = opCtx->inMultiDocumentTransaction();

    auto doWork = [&] {
        if (req.getRemove().value_or(false)) {
            DeleteRequest deleteRequest;
            makeDeleteRequest(opCtx, req, false, &deleteRequest);
            deleteRequest.setNsString(nsString);
            if (opCtx->getTxnNumber()) {
                deleteRequest.setStmtId(stmtId);
            }
            boost::optional<BSONObj> docFound;
            write_ops_exec::performDelete(
                opCtx, nsString, &deleteRequest, &curOp, inTransaction, boost::none, docFound);
            recordStatsForTopCommand(opCtx);
            return buildResponse(boost::none, true /* isRemove */, docFound);
        } else {
            if (MONGO_unlikely(hangBeforeFindAndModifyPerformsUpdate.shouldFail())) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeFindAndModifyPerformsUpdate,
                    opCtx,
                    "hangBeforeFindAndModifyPerformsUpdate");
            }

            // Nested retry loop to handle concurrent conflicting upserts with equality
            // match.
            int retryAttempts = 0;
            for (;;) {
                auto updateRequest = UpdateRequest();
                updateRequest.setNamespaceString(nsString);
                const auto verbosity = boost::none;
                update::makeUpdateRequest(opCtx, req, verbosity, &updateRequest);

                if (opCtx->getTxnNumber()) {
                    updateRequest.setStmtIds({stmtId});
                }
                updateRequest.setSampleId(sampleId);

                updateRequest.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
                    req.getAllowShardKeyUpdatesWithoutFullShardKeyInQuery());

                try {
                    boost::optional<BSONObj> docFound;
                    auto updateResult =
                        write_ops_exec::performUpdate(opCtx,
                                                      nsString,
                                                      &curOp,
                                                      inTransaction,
                                                      req.getRemove().value_or(false),
                                                      req.getUpsert().value_or(false),
                                                      boost::none,
                                                      docFound,
                                                      &updateRequest);
                    recordStatsForTopCommand(opCtx);
                    return buildResponse(updateResult, req.getRemove().value_or(false), docFound);

                } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
                    auto cq = uassertStatusOK(
                        parseWriteQueryToCQ(opCtx, nullptr /* expCtx */, updateRequest));
                    if (!write_ops_exec::shouldRetryDuplicateKeyException(
                            updateRequest, *cq, *ex.extraInfo<DuplicateKeyErrorInfo>())) {
                        throw;
                    }

                    ++retryAttempts;
                    logAndBackoff(4721200,
                                  ::mongo::logv2::LogComponent::kWrite,
                                  logv2::LogSeverity::Debug(1),
                                  retryAttempts,
                                  "Caught DuplicateKey exception during findAndModify upsert",
                                  logAttrs(nsString));
                } catch (const ExceptionFor<ErrorCodes::WouldChangeOwningShard>& ex) {
                    if (analyze_shard_key::supportsPersistingSampledQueries(opCtx) &&
                        req.getSampleId()) {
                        // Sample the diff before rethrowing the error since mongos will handle this
                        // update by performing a delete on the shard owning the pre-image doc and
                        // an insert on the shard owning the post-image doc. As a result, this
                        // update will not show up in the OpObserver as an update.
                        auto wouldChangeOwningShardInfo =
                            ex.extraInfo<WouldChangeOwningShardInfo>();
                        invariant(wouldChangeOwningShardInfo);

                        analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                            ->addDiff(*req.getSampleId(),
                                      ns(),
                                      *wouldChangeOwningShardInfo->getUuid(),
                                      wouldChangeOwningShardInfo->getPreImage(),
                                      wouldChangeOwningShardInfo->getPostImage())
                            .getAsync([](auto) {});
                    }
                    throw;
                }
            }
        }
    };

    // No need to call writeConflictRetry() since it does not retry if in a transaction,
    // but calling it can cause WCE to be double counted.
    if (inTransaction) {
        return doWork();
    }
    // Although usually the PlanExecutor handles WCE internally, it will throw WCEs when it
    // is executing a findAndModify. This is done to ensure that we can always match,
    // modify, and return the document under concurrency, if a matching document exists.
    return writeConflictRetry(opCtx, "findAndModify", nsString, doWork);
}

void CmdFindAndModify::Invocation::appendMirrorableRequest(BSONObjBuilder* bob) const {
    const auto& req = request();

    bob->append(FindCommandRequest::kCommandName, req.getNamespace().coll());

    if (!req.getQuery().isEmpty()) {
        bob->append(FindCommandRequest::kFilterFieldName, req.getQuery());
    }
    if (req.getSort()) {
        bob->append(write_ops::FindAndModifyCommandRequest::kSortFieldName, *req.getSort());
    }
    if (req.getCollation()) {
        bob->append(write_ops::FindAndModifyCommandRequest::kCollationFieldName,
                    *req.getCollation());
    }
    if (req.getEncryptionInformation()) {
        bob->append(write_ops::FindAndModifyCommandRequest::kEncryptionInformationFieldName,
                    req.getEncryptionInformation()->toBSON());
    }

    const auto& rawCmd = unparsedRequest().body;
    if (const auto& shardVersion = rawCmd.getField("shardVersion"); !shardVersion.eoo()) {
        bob->append(shardVersion);
    }
    if (const auto& databaseVersion = rawCmd.getField("databaseVersion"); !databaseVersion.eoo()) {
        bob->append(databaseVersion);
    }

    // Prevent the find from returning multiple documents since we can
    bob->append("batchSize", 1);
    bob->append("singleBatch", true);
}

}  // namespace
}  // namespace mongo
