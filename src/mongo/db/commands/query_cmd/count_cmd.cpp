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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/client_cursor/collect_query_stats_mongod.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_diagnostic_printer.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_stats/count_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

std::unique_ptr<ExtensionsCallback> getExtensionsCallback(
    const CollectionOrViewAcquisition& collectionOrView,
    OperationContext* opCtx,
    const NamespaceString& nss) {
    if (collectionOrView.collectionExists()) {
        return std::make_unique<ExtensionsCallbackReal>(ExtensionsCallbackReal(opCtx, &nss));
    }
    return std::make_unique<ExtensionsCallbackNoop>(ExtensionsCallbackNoop());
}

void initStatsTracker(OperationContext* opCtx,
                      const NamespaceString& nss,
                      boost::optional<AutoStatsTracker>& statsTracker) {
    statsTracker.emplace(opCtx,
                         nss,
                         Top::LockType::ReadLocked,
                         AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                         DatabaseProfileSettings::get(opCtx->getServiceContext())
                             .getDatabaseProfileLevel(nss.dbName()));
}

// The # of documents returned is always 1 for the count command.
static constexpr long long kNReturned = 1;

// Failpoint which causes to hang "count" cmd after acquiring the DB lock.
MONGO_FAIL_POINT_DEFINE(hangBeforeCollectionCount);

/**
 * Implements the MongoD side of the count command.
 */
class CmdCount : public CountCmdVersion1Gen<CmdCount> {
public:
    std::string help() const override {
        return "count objects in collection";
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest),
              _ns(request().getNamespaceOrUUID().isNamespaceString()
                      ? request().getNamespaceOrUUID().nss()
                      : shard_role_nocheck::resolveNssWithoutAcquisition(
                            opCtx,
                            request().getNamespaceOrUUID().dbName(),
                            request().getNamespaceOrUUID().uuid())) {
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace specified '" << _ns.toStringForErrorMsg()
                                  << "'",
                    _ns.isValid());
        }

        bool supportsWriteConcern() const final {
            return false;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
            const auto& req = request();
            NamespaceStringOrUUID const& nsOrUUID = req.getNamespaceOrUUID();
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    authSession->isAuthorizedToParseNamespaceElement(nsOrUUID));

            constexpr auto hasTerm = false;
            uassertStatusOK(auth::checkAuthForFind(authSession, _ns, hasTerm));
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* replyBuilder) override {
            // Using explain + count + UUID is not supported here so that there is "feature parity"
            // with mongos, which also does not support using a UUID for count + explain.
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Collection name must be provided. UUID is not valid in this "
                                  << "context",
                    !request().getNamespaceOrUUID().isUUID());

            if (prepareForFLERewrite(opCtx, request().getEncryptionInformation())) {
                processFLECountD(opCtx, _ns, request());
            }

            boost::optional<AutoStatsTracker> statsTracker;
            initStatsTracker(opCtx, _ns, statsTracker);

            // Acquire locks. The RAII object is optional, because in the case of a view, the locks
            // need to be released.
            boost::optional<CollectionOrViewAcquisition> collOrViewAcquisition =
                acquireCollectionOrViewMaybeLockFree(
                    opCtx,
                    CollectionOrViewAcquisitionRequest::fromOpCtx(
                        opCtx, _ns, AcquisitionPrerequisites::OperationType::kRead));

            // Start the query planning timer.
            CurOp::get(opCtx)->beginQueryPlanningTimer();

            auto ns = [&] {
                if (isRawDataOperation(opCtx)) {
                    auto [isTimeseriesViewRequest, ns] =
                        timeseries::isTimeseriesViewRequest(opCtx, request());
                    if (isTimeseriesViewRequest) {
                        initStatsTracker(opCtx, ns, statsTracker);
                        collOrViewAcquisition = acquireCollectionOrViewMaybeLockFree(
                            opCtx,
                            CollectionOrViewAcquisitionRequest::fromOpCtx(
                                opCtx, ns, AcquisitionPrerequisites::OperationType::kRead));
                        return ns;
                    }
                }
                return _ns;
            }();

            if (collOrViewAcquisition) {
                if (collOrViewAcquisition->isView() ||
                    timeseries::requiresViewlessTimeseriesTranslation(opCtx,
                                                                      *collOrViewAcquisition)) {
                    // Relinquish locks. The aggregation command will re-acquire them.
                    collOrViewAcquisition.reset();
                    statsTracker.reset();
                    return runExplainAsAgg(opCtx, request(), verbosity, replyBuilder);
                }
            }

            tassert(10168300,
                    "Expected ShardRole acquisition to be of type collection",
                    collOrViewAcquisition->isCollection());
            const auto& collection = collOrViewAcquisition->getCollection();

            // Create an RAII object that prints the collection's shard key in the case of a tassert
            // or crash.
            ScopedDebugInfo shardKeyDiagnostics(
                "ShardKeyDiagnostics",
                diagnostic_printers::ShardKeyDiagnosticPrinter{
                    collection.getShardingDescription().isSharded()
                        ? collection.getShardingDescription().getKeyPattern()
                        : BSONObj()});

            auto expCtx = makeExpressionContextForGetExecutor(
                opCtx, request().getCollation().value_or(BSONObj()), ns, verbosity);

            // Create an RAII object that prints useful information about the ExpressionContext in
            // the case of a tassert or crash.
            ScopedDebugInfo expCtxDiagnostics(
                "ExpCtxDiagnostics", diagnostic_printers::ExpressionContextPrinter{expCtx});

            const auto extensionsCallback =
                getExtensionsCallback(*collOrViewAcquisition, opCtx, ns);
            auto parsedFind = uassertStatusOK(
                parsed_find_command::parseFromCount(expCtx, request(), *extensionsCallback, ns));

            auto statusWithPlanExecutor =
                getExecutorCount(expCtx, collection, std::move(parsedFind), request());
            uassertStatusOK(statusWithPlanExecutor.getStatus());

            auto exec = std::move(statusWithPlanExecutor.getValue());
            auto bodyBuilder = replyBuilder->getBodyBuilder();

            // Capture diagnostics to be logged in the case of a failure.
            ScopedDebugInfo explainDiagnostics(
                "explainDiagnostics", diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});
            Explain::explainStages(
                exec.get(),
                collection,
                verbosity,
                BSONObj(),
                SerializationContext::stateCommandReply(request().getSerializationContext()),
                request().toBSON(),
                &bodyBuilder);
        }

        NamespaceString ns() const final {
            // Guaranteed to be valid.
            return _ns;
        }

        CountCommandReply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            if (prepareForFLERewrite(opCtx, request().getEncryptionInformation())) {
                processFLECountD(opCtx, _ns, request());
            }

            boost::optional<AutoStatsTracker> statsTracker;
            initStatsTracker(opCtx, _ns, statsTracker);

            // Acquire locks. The RAII object is optional, because in the case of a view, the locks
            // need to be released.
            boost::optional<CollectionOrViewAcquisition> collOrViewAcquisition =
                acquireCollectionOrViewMaybeLockFree(
                    opCtx,
                    CollectionOrViewAcquisitionRequest::fromOpCtx(
                        opCtx, _ns, AcquisitionPrerequisites::OperationType::kRead));

            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &hangBeforeCollectionCount, opCtx, "hangBeforeCollectionCount", []() {}, _ns);

            auto ns = [&] {
                if (isRawDataOperation(opCtx)) {
                    auto [isTimeseriesViewRequest, ns] =
                        timeseries::isTimeseriesViewRequest(opCtx, request());
                    if (isTimeseriesViewRequest) {
                        initStatsTracker(opCtx, ns, statsTracker);
                        collOrViewAcquisition = acquireCollectionOrViewMaybeLockFree(
                            opCtx,
                            CollectionOrViewAcquisitionRequest::fromOpCtx(
                                opCtx, ns, AcquisitionPrerequisites::OperationType::kRead));
                        return ns;
                    }
                }
                return _ns;
            }();

            // Start the query planning timer.
            auto curOp = CurOp::get(opCtx);
            curOp->beginQueryPlanningTimer();

            if (request().getMirrored().value_or(false)) {
                const auto& invocation = CommandInvocation::get(opCtx);
                invocation->markMirrored();
            } else {
                analyzeShardKeyIfNeeded(opCtx, request());
            }

            auto expCtx =
                makeExpressionContextForGetExecutor(opCtx,
                                                    request().getCollation().value_or(BSONObj()),
                                                    ns,
                                                    boost::none /* verbosity*/);

            // Create an RAII object that prints useful information about the ExpressionContext in
            // the case of a tassert or crash.
            ScopedDebugInfo expCtxDiagnostics(
                "ExpCtxDiagnostics", diagnostic_printers::ExpressionContextPrinter{expCtx});

            const auto extensionsCallback =
                getExtensionsCallback(*collOrViewAcquisition, opCtx, ns);
            auto parsedFind = uassertStatusOK(
                parsed_find_command::parseFromCount(expCtx, request(), *extensionsCallback, ns));

            registerRequestForQueryStats(
                opCtx, expCtx, curOp, *collOrViewAcquisition, request(), *parsedFind);

            if (collOrViewAcquisition) {
                if (collOrViewAcquisition->isView() ||
                    timeseries::requiresViewlessTimeseriesTranslation(opCtx,
                                                                      *collOrViewAcquisition)) {
                    // Relinquish locks. The aggregation command will re-acquire them.
                    collOrViewAcquisition.reset();
                    statsTracker.reset();
                    return runCountAsAgg(opCtx, request());
                }
            }

            tassert(10168301,
                    "Expected ShardRole acquisition to be of type collection",
                    collOrViewAcquisition->isCollection());
            const auto& collection = collOrViewAcquisition->getCollection();

            // Create an RAII object that prints the collection's shard key in the case of a tassert
            // or crash.
            ScopedDebugInfo shardKeyDiagnostics(
                "ShardKeyDiagnostics",
                diagnostic_printers::ShardKeyDiagnosticPrinter{
                    collection.getShardingDescription().isSharded()
                        ? collection.getShardingDescription().getKeyPattern()
                        : BSONObj()});

            // Check whether we are allowed to read from this node after acquiring our locks.
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            uassertStatusOK(replCoord->checkCanServeReadsFor(
                opCtx, _ns, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

            auto statusWithPlanExecutor =
                getExecutorCount(expCtx, collection, std::move(parsedFind), request());
            uassertStatusOK(statusWithPlanExecutor.getStatus());

            auto exec = std::move(statusWithPlanExecutor.getValue());
            // Capture diagnostics to be logged in the case of a failure.
            ScopedDebugInfo explainDiagnostics(
                "explainDiagnostics", diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});

            // Store the plan summary string in CurOp.
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                curOp->setPlanSummary(lk, exec->getPlanExplainer().getPlanSummary());
            }
            long long countResult = 0;
            try {
                countResult = exec->executeCount();
            } catch (DBException& exception) {
                auto&& explainer = exec->getPlanExplainer();
                auto&& [stats, _] =
                    explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
                LOGV2_WARNING(8712900,
                              "Plan executor error during count command",
                              "error"_attr = exception.toStatus(),
                              "stats"_attr = redact(stats),
                              "cmd"_attr = redact(request().toBSON()));

                exception.addContext(str::stream()
                                     << "Executor error during count command on namespace: "
                                     << _ns.toStringForErrorMsg());
                throw;
            }
            // Store metrics for current operation.
            recordCurOpMetrics(opCtx, curOp, collection.getCollectionPtr(), *exec);

            // Store profiling data if profiling is enabled.
            collectProfilingDataIfNeeded(curOp, *exec);

            collectQueryStatsMongod(opCtx, expCtx, std::move(curOp->debug().queryStatsInfo.key));

            CountCommandReply reply = buildCountReply(countResult);
            if (curOp->debug().queryStatsInfo.metricsRequested) {
                reply.setMetrics(curOp->debug().getCursorMetrics().toBSON());
            }
            return reply;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            static const Status kSnapshotNotSupported{ErrorCodes::InvalidOptions,
                                                      "read concern snapshot not supported"};
            return {{level == repl::ReadConcernLevel::kSnapshotReadConcern, kSnapshotNotSupported},
                    Status::OK()};
        }

        bool supportsRawData() const override {
            return true;
        }

        bool supportsReadMirroring() const override {
            return true;
        }

        void appendMirrorableRequest(BSONObjBuilder* bob) const override {
            const auto& req = request();

            // Append the keys that can be mirrored.
            if (const auto& nsOrUUID = req.getNamespaceOrUUID(); nsOrUUID.isNamespaceString()) {
                bob->append(CountCommandRequest::kCommandName, nsOrUUID.nss().coll());
            } else {
                uassert(7145300, "expecting nsOrUUID to contain a UUID", nsOrUUID.isUUID());
                bob->append(CountCommandRequest::kCommandName, nsOrUUID.uuid().toBSON());
            }
            bob->append(CountCommandRequest::kQueryFieldName, req.getQuery());
            if (req.getSkip()) {
                bob->append(CountCommandRequest::kSkipFieldName, *req.getSkip());
            }
            if (req.getLimit()) {
                bob->append(CountCommandRequest::kLimitFieldName, *req.getLimit());
            }
            bob->append(CountCommandRequest::kHintFieldName, req.getHint());
            if (req.getCollation()) {
                bob->append(CountCommandRequest::kCollationFieldName, *req.getCollation());
            }
            if (req.getShardVersion()) {
                req.getShardVersion()->serialize(CountCommandRequest::kShardVersionFieldName, bob);
            }
            if (req.getDatabaseVersion()) {
                bob->append(CountCommandRequest::kDatabaseVersionFieldName,
                            req.getDatabaseVersion()->toBSON());
            }
            if (req.getEncryptionInformation()) {
                bob->append(CountCommandRequest::kEncryptionInformationFieldName,
                            req.getEncryptionInformation()->toBSON());
            }
            req.getRawData().serializeToBSON(CountCommandRequest::kRawDataFieldName, bob);
        }

        bool canIgnorePrepareConflicts() const override {
            return true;
        }

    private:
        void registerRequestForQueryStats(OperationContext* opCtx,
                                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          CurOp* curOp,
                                          const CollectionOrViewAcquisition& collectionOrView,
                                          const CountCommandRequest& req,
                                          const ParsedFindCommand& parsedFind) {
            if (feature_flags::gFeatureFlagQueryStatsCountDistinct
                    .isEnabledUseLastLTSFCVWhenUninitialized(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                query_stats::registerRequest(opCtx, _ns, [&]() {
                    return std::make_unique<query_stats::CountKey>(
                        expCtx,
                        parsedFind,
                        req.getLimit().has_value(),
                        req.getSkip().has_value(),
                        req.getReadConcern(),
                        req.getMaxTimeMS().has_value(),
                        collectionOrView.getCollectionType());
                });

                if (req.getIncludeQueryStatsMetrics()) {
                    curOp->debug().queryStatsInfo.metricsRequested = true;
                }
            }
        }

        void recordCurOpMetrics(OperationContext* opCtx,
                                CurOp* curOp,
                                const CollectionPtr& collection,
                                const PlanExecutor& exec) {
            PlanSummaryStats summaryStats;
            exec.getPlanExplainer().getSummaryStats(&summaryStats);
            if (collection) {
                CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
                    collection.get(),
                    summaryStats.collectionScans,
                    summaryStats.collectionScansNonTailable,
                    summaryStats.indexesUsed);
            }
            curOp->debug().setPlanSummaryMetrics(std::move(summaryStats));
            curOp->setEndOfOpMetrics(kNReturned);
        }

        void collectProfilingDataIfNeeded(CurOp* curOp, PlanExecutor& exec) {
            if (!curOp->shouldDBProfile()) {
                return;
            }
            auto&& explainer = exec.getPlanExplainer();
            auto&& [stats, _] =
                explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
            curOp->debug().execStats = std::move(stats);
        }

        void analyzeShardKeyIfNeeded(OperationContext* opCtx, const CountCommandRequest& req) {
            if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                    opCtx, _ns, analyze_shard_key::SampledCommandNameEnum::kCount, req)) {
                analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                    ->addCountQuery(
                        *sampleId, _ns, req.getQuery(), req.getCollation().value_or(BSONObj()))
                    .getAsync([](auto) {});
            }
        }

        // Build the return value for this command.
        CountCommandReply buildCountReply(long long countResult) {
            uassert(7145301, "count value must not be negative", countResult >= 0);

            // Return either BSON int32 or int64, depending on the value of countResult.
            // This is required so that drivers can continue to use a BSON int32 for count
            // values < 2 ^ 31, which is what some client applications may still depend on.
            // int64 is only used when the count value exceeds 2 ^ 31.
            auto count = [](long long countResult) -> std::variant<std::int32_t, std::int64_t> {
                constexpr long long maxIntCountResult = std::numeric_limits<std::int32_t>::max();
                if (countResult < maxIntCountResult) {
                    return static_cast<std::int32_t>(countResult);
                }
                return static_cast<std::int64_t>(countResult);
            }(countResult);

            CountCommandReply reply;
            reply.setCount(count);
            return reply;
        }

        void runExplainAsAgg(OperationContext* opCtx,
                             const RequestType& req,
                             ExplainOptions::Verbosity verbosity,
                             rpc::ReplyBuilderInterface* replyBuilder) {
            auto curOp = CurOp::get(opCtx);
            curOp->debug().queryStatsInfo.disableForSubqueryExecution = true;
            const auto vts = auth::ValidatedTenancyScope::get(opCtx);
            auto viewAggRequest =
                query_request_conversion::asAggregateCommandRequest(req, true /* hasExplain */);
            // An empty PrivilegeVector is acceptable because these privileges are only checked
            // on getMore and explain will not open a cursor.
            auto runStatus = runAggregate(opCtx,
                                          viewAggRequest,
                                          {viewAggRequest},
                                          req.toBSON(),
                                          PrivilegeVector(),
                                          verbosity,
                                          replyBuilder);
            uassertStatusOK(runStatus);
        }

        CountCommandReply runCountAsAgg(OperationContext* opCtx, const RequestType& req) {
            const auto vts = auth::ValidatedTenancyScope::get(opCtx);
            auto aggRequest = query_request_conversion::asAggregateCommandRequest(req);
            auto opMsgAggRequest =
                OpMsgRequestBuilder::create(vts, aggRequest.getDbName(), aggRequest.toBSON());
            BSONObj aggResult = CommandHelpers::runCommandDirectly(opCtx, opMsgAggRequest);

            long long countResult = ViewResponseFormatter(aggResult).getCountValue(
                _ns.dbName().tenantId(),
                SerializationContext::stateCommandReply(req.getSerializationContext()));

            return buildCountReply(countResult);
        }

    private:
        const NamespaceString _ns;
    };

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* serviceContext) const override {
        return Command::AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool shouldAffectReadOptionCounters() const override {
        return true;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }
};
MONGO_REGISTER_COMMAND(CmdCount).forShard();

}  // namespace
}  // namespace mongo
