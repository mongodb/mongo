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
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/exec/classic/projection.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_diagnostic_printer.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_type.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/storage_stats.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
// Ticks for server-side Javascript deprecation log messages.
Rarely _samplerFunctionJs, _samplerWhereClause;

MONGO_FAIL_POINT_DEFINE(allowExternalReadsForReverseOplogScanRule);
MONGO_FAIL_POINT_DEFINE(hangBeforeFetcherFindCommandOnOplog);

const auto kTermField = "term"_sd;

/**
 * Fills out the CurOp for "opCtx" with information about this query.
 */
void beginQueryOp(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& queryObj) {
    auto curOp = CurOp::get(opCtx);
    std::lock_guard<Client> lk(*opCtx->getClient());
    curOp->setOpDescription(lk, queryObj);
    curOp->setNS(lk, nss);
}

/**
 * Parses the grammar elements like 'filter', 'sort', and 'projection' from the raw
 * 'FindCommandRequest', and tracks internal state like begining the operation's timer and recording
 * query shape stats (if enabled).
 */
std::unique_ptr<CanonicalQuery> parseQueryAndBeginOperation(
    OperationContext* opCtx,
    const CollectionOrViewAcquisition& collOrViewAcquisition,
    const NamespaceString& nss,
    BSONObj requestBody,
    std::unique_ptr<FindCommandRequest> findCommand) {
    // Fill out curop information.
    beginQueryOp(opCtx, nss, requestBody);

    const auto& collection = collOrViewAcquisition.getCollectionPtr();
    const auto* collator = collection ? collection->getDefaultCollator() : nullptr;
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, *findCommand, collator, allowDiskUseByDefault.load())
                      .tmpDir(boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp")
                      .build();
    expCtx->startExpressionCounters();
    auto parsedRequest = uassertStatusOK(parsed_find_command::parse(
        expCtx,
        {.findCommand = std::move(findCommand),
         .extensionsCallback = ExtensionsCallbackReal(opCtx, &nss),
         .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

    // Initialize system variables before constructing CanonicalQuery as the constructor
    // performs constant-folding optimizations which depend on these agg variables being
    // properly initialized.
    expCtx->initializeReferencedSystemVariables();

    // Compute QueryShapeHash and record it in CurOp.
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest, expCtx);
    }};
    auto queryShapeHash = CurOp::get(opCtx)->debug().ensureQueryShapeHash(
        opCtx, [&]() { return shape_helpers::computeQueryShapeHash(expCtx, deferredShape, nss); });

    // Perform the query settings lookup and attach it to 'expCtx'.
    auto& querySettingsService = query_settings::QuerySettingsService::get(opCtx);
    auto querySettings = querySettingsService.lookupQuerySettingsWithRejectionCheck(
        expCtx, queryShapeHash, nss, parsedRequest->findCommandRequest->getQuerySettings());
    expCtx->setQuerySettingsIfNotPresent(std::move(querySettings));

    // Register query stats collection. Exclude queries with encrypted fields as indicated by the
    // inclusion of encryptionInformation in the request.
    // It is important to do this before canonicalizing and optimizing the query, each of which
    // would alter the query shape.
    if (!parsedRequest->findCommandRequest->getEncryptionInformation()) {
        query_stats::registerRequest(opCtx, nss, [&]() {
            uassertStatusOKWithContext(deferredShape->getStatus(), "Failed to compute query shape");
            return std::make_unique<query_stats::FindKey>(
                expCtx,
                *parsedRequest->findCommandRequest,
                std::move(deferredShape->getValue()),
                collOrViewAcquisition.getCollectionType());
        });

        if (parsedRequest->findCommandRequest->getIncludeQueryStatsMetrics()) {
            CurOp::get(opCtx)->debug().getQueryStatsInfo().metricsRequested = true;
        }
    }

    // Check for server-side javascript usage after parsing is complete and the flags have been set
    // on the expression context.
    if (expCtx->getServerSideJsConfig().where && _samplerWhereClause.tick()) {
        LOGV2_WARNING(8996500,
                      "$where is deprecated. For more information, see "
                      "https://www.mongodb.com/docs/manual/reference/operator/query/where/");
    }

    if (expCtx->getServerSideJsConfig().function && _samplerFunctionJs.tick()) {
        LOGV2_WARNING(
            8996501,
            "$function is deprecated. For more information, see "
            "https://www.mongodb.com/docs/manual/reference/operator/aggregation/function/");
    }

    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = std::move(expCtx),
        .parsedFind = std::move(parsedRequest),
    });
}

/**
 * A command for running .find() queries.
 */
class FindCmd final : public TypedCommand<FindCmd> {
public:
    using Request = FindCommandRequest;

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }
    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "query for documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opQuery;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    /**
     * A find command does not increment the command counter, but rather increments the
     * query counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    bool shouldAffectQueryCounter() const override {
        return true;
    }

    bool shouldAffectReadOptionCounters() const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    class Invocation final : public MinimalInvocationBase {
        using MinimalInvocationBase::request;
        using MinimalInvocationBase::unparsedRequest;

    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : MinimalInvocationBase(opCtx, command, opMsgRequest),
              _ns(request().getNamespaceOrUUID().isNamespaceString()
                      ? request().getNamespaceOrUUID().nss()
                      : NamespaceString(request().getNamespaceOrUUID().dbName())) {
            if (request().getNamespaceOrUUID().isNamespaceString()) {
                CommandHelpers::ensureValidCollectionName(request().getNamespaceOrUUID().nss());
            }
            assertInternalParamsAreSetByInternalClients(opCtx->getClient(), request());
            uassert(ErrorCodes::FailedToParse,
                    "Use of forcedPlanSolutionHash not permitted.",
                    !request().getForcedPlanSolutionHash() ||
                        internalQueryAllowForcedPlanByHash.load());
            if (request().getMirrored().value_or(false)) {
                markMirrored();
            }
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
        }

        bool supportsRawData() const override {
            return true;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return !request().getTerm().has_value();
        }

        bool supportsReadMirroring() const override {
            return true;
        }

        bool canIgnorePrepareConflicts() const override {
            return true;
        }

        bool allowsSpeculativeMajorityReads() const override {
            // Find queries are only allowed to use speculative behavior if the 'allowsSpeculative'
            // flag is passed. The find command will check for this flag internally and fail if
            // necessary.
            return true;
        }

        NamespaceString ns() const override {
            return _ns;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    authSession->isAuthorizedToParseNamespaceElement(
                        unparsedRequest().body.firstElement()));

            const auto hasTerm = request().getTerm().has_value();
            const NamespaceStringOrUUID& nsOrUUID = request().getNamespaceOrUUID();
            if (nsOrUUID.isNamespaceString()) {
                uassert(ErrorCodes::InvalidNamespace,
                        str::stream() << "Namespace " << nsOrUUID.toStringForErrorMsg()
                                      << " is not a valid collection name",
                        nsOrUUID.nss().isValid());
                uassertStatusOK(auth::checkAuthForFind(authSession, nsOrUUID.nss(), hasTerm));
            } else {
                const auto resolvedNss = shard_role_nocheck::resolveNssWithoutAcquisition(
                    opCtx, nsOrUUID.dbName(), nsOrUUID.uuid());
                uassertStatusOK(auth::checkAuthForFind(authSession, resolvedNss, hasTerm));
            }
        }

        /**
         * Entry point for execution of find explain command.
         */
        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* replyBuilder) override {
            // Providing collection UUID for explain is forbidden, see SERVER-38821 and SERVER-38275
            uassert(ErrorCodes::InvalidNamespace,
                    "Providing collection UUID for explain is forbidden",
                    !request().getNamespaceOrUUID().isUUID());

            // We want to start the query planning timer right after parsing. In the explain code
            // path, we have already parsed the FindCommandRequest, so start timing here.
            CurOp::get(opCtx)->beginQueryPlanningTimer();

            auto cmdRequest = query_request_helper::makeFromFindCommand(request());

            // Acquire locks. The RAII object is optional, because in the case of a view, the locks
            // need to be released.
            // TODO SERVER-79175: Make nicer. We need to instantiate the AutoStatsTracker before the
            // acquisition in case it would throw so we can ensure data is written to the profile
            // collection that some test may rely on.
            AutoStatsTracker tracker{opCtx,
                                     _ns,
                                     Top::LockType::ReadLocked,
                                     AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                     DatabaseProfileSettings::get(opCtx->getServiceContext())
                                         .getDatabaseProfileLevel(_ns.dbName())};

            const auto acquisitionRequest = CollectionOrViewAcquisitionRequest::fromOpCtx(
                opCtx, _ns, AcquisitionPrerequisites::kRead);

            boost::optional<CollectionOrViewAcquisition> collectionOrView =
                acquireCollectionOrViewMaybeLockFree(opCtx, acquisitionRequest);

            auto ns = [&] {
                if (isRawDataOperation(opCtx)) {
                    auto [isTimeseriesViewRequest, translatedNs] =
                        timeseries::isTimeseriesViewRequest(opCtx, *cmdRequest);
                    if (isTimeseriesViewRequest) {
                        cmdRequest->setNss(translatedNs);
                        collectionOrView = acquireCollectionOrViewMaybeLockFree(
                            opCtx,
                            CollectionOrViewAcquisitionRequest::fromOpCtx(
                                opCtx, translatedNs, AcquisitionPrerequisites::kRead));
                        return translatedNs;
                    }
                }
                return _ns;
            }();

            // Going forward this operation must never ignore interrupt signals while waiting for
            // lock acquisition. This InterruptibleLockGuard will ensure that waiting for lock
            // re-acquisition after yielding will not ignore interrupt signals. This is necessary to
            // avoid deadlocking with replication rollback, which at the storage layer waits for all
            // cursors to be closed under the global MODE_X lock, after having sent interrupt
            // signals to read operations. This operation must never hold open storage cursors while
            // ignoring interrupt.
            InterruptibleLockGuard interruptibleLockAcquisition(opCtx);

            _rewriteFLEPayloads(opCtx, *cmdRequest);
            auto respSc =
                SerializationContext::stateCommandReply(cmdRequest->getSerializationContext());

            // The collection may be NULL. If so, getExecutor() should handle it by returning an
            // execution tree with an EOFStage.
            const auto& collectionPtr = collectionOrView->getCollectionPtr();
            if (!collectionOrView->isView()) {
                const bool isClusteredCollection = collectionPtr && collectionPtr->isClustered();
                uassertStatusOK(
                    query_request_helper::validateResumeInput(opCtx,
                                                              cmdRequest->getResumeAfter(),
                                                              cmdRequest->getStartAt(),
                                                              isClusteredCollection));
            }
            const auto* collator = collectionPtr ? collectionPtr->getDefaultCollator() : nullptr;
            auto expCtx =
                ExpressionContextBuilder{}
                    .fromRequest(opCtx, *cmdRequest, collator, allowDiskUseByDefault.load())
                    .explain(verbosity)
                    .tmpDir(boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp")
                    .build();
            expCtx->startExpressionCounters();

            // Create an RAII object that prints useful information about the ExpressionContext in
            // the case of a tassert or crash.
            ScopedDebugInfo expCtxDiagnostics(
                "ExpCtxDiagnostics", diagnostic_printers::ExpressionContextPrinter{expCtx});

            auto parsedRequest = uassertStatusOK(parsed_find_command::parse(
                expCtx,
                {.findCommand = std::move(cmdRequest),
                 .extensionsCallback = ExtensionsCallbackReal(opCtx, &ns),
                 .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

            // Initialize system variables before constructing CanonicalQuery as the constructor
            // performs constant-folding optimizations which depend on these agg variables being
            // properly initialized.
            expCtx->initializeReferencedSystemVariables();

            // Compute QueryShapeHash and record it in CurOp.
            query_shape::DeferredQueryShape deferredShape{[&]() {
                return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest,
                                                                              expCtx);
            }};
            auto queryShapeHash = CurOp::get(opCtx)->debug().ensureQueryShapeHash(opCtx, [&]() {
                return shape_helpers::computeQueryShapeHash(expCtx, deferredShape, ns);
            });

            // Perform the query settings lookup and attach it to 'expCtx'.
            auto& querySettingsService = query_settings::QuerySettingsService::get(opCtx);
            auto querySettings = querySettingsService.lookupQuerySettingsWithRejectionCheck(
                expCtx, queryShapeHash, ns, parsedRequest->findCommandRequest->getQuerySettings());
            expCtx->setQuerySettingsIfNotPresent(std::move(querySettings));

            auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
                .expCtx = std::move(expCtx), .parsedFind = std::move(parsedRequest)});

            // If we are running a query against a view or a timeseries collection, redirect this
            // query through the aggregation system.
            if (collectionOrView->isView() ||
                timeseries::requiresViewlessTimeseriesTranslation(opCtx, *collectionOrView)) {
                // Relinquish locks. The aggregation command will re-acquire them.
                collectionOrView.reset();
                CurOp::get(opCtx)->debug().getQueryStatsInfo().disableForSubqueryExecution = true;
                return runFindAsAgg(opCtx, *cq, verbosity, replyBuilder);
            }

            // Create an RAII object that prints the collection's shard key in the case of a tassert
            // or crash.
            auto collShardingDescription =
                collectionOrView->getCollection().getShardingDescription();
            ScopedDebugInfo shardKeyDiagnostics("ShardKeyDiagnostics",
                                                diagnostic_printers::ShardKeyDiagnosticPrinter{
                                                    collShardingDescription.isSharded()
                                                        ? collShardingDescription.getKeyPattern()
                                                        : BSONObj()});

            // Get the execution plan for the query.
            const auto& collection = collectionOrView->getCollection();
            auto exec = uassertStatusOK(getExecutorFind(opCtx,
                                                        MultipleCollectionAccessor{collection},
                                                        std::move(cq),
                                                        PlanYieldPolicy::YieldPolicy::YIELD_AUTO));

            auto bodyBuilder = replyBuilder->getBodyBuilder();
            // Capture diagnostics to be logged in the case of a failure.
            ScopedDebugInfo explainDiagnostics(
                "explainDiagnostics", diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});
            // Got the execution tree. Explain it.
            Explain::explainStages(exec.get(),
                                   collection,
                                   verbosity,
                                   BSONObj(),
                                   respSc,
                                   unparsedRequest().body,
                                   &bodyBuilder);
        }

        /**
         * Helper function which handles outputting the first batch of documents generated by 'exec'
         * in 'firstBatch', returning the number of documents in the batch, and recording the number
         * & size of documents in the batch via 'docUnitsReturned'.
         */
        uint64_t batchedExecute(const size_t batchSize,
                                PlanExecutor* exec,
                                CursorResponseBuilder& firstBatch) {
            BSONObj pbrt = exec->getPostBatchResumeToken();
            size_t numResults = 0;
            bool failedToAppend = false;

            // Capture diagnostics to be logged in the case of a failure.
            ScopedDebugInfo explainDiagnostics("explainDiagnostics",
                                               diagnostic_printers::ExplainDiagnosticPrinter{exec});
            numResults = exec->getNextBatch(
                batchSize,
                FindCommon::BSONObjCursorAppender{
                    true /* alwaysAcceptFirstDoc */, &firstBatch, pbrt, failedToAppend});

            // Use the resume token generated by the last execution of the plan that didn't stash a
            // document, or the latest resume token if we hit EOF/the end of the batch.
            firstBatch.setPostBatchResumeToken(failedToAppend ? pbrt
                                                              : exec->getPostBatchResumeToken());
            return numResults;
        }

        /**
         * Runs a query using the following steps:
         *   --Parsing.
         *   --Acquire locks.
         *   --Plan query, obtaining an executor that can run it.
         *   --Generate the first batch.
         *   --Save state for getMore, transferring ownership of the executor to a ClientCursor.
         *   --Generate response to send to the client.
         */
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* replyBuilder) override {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            // Reads to system-critical collections issued remotely by internal clients should not
            // be deprioritized:
            // - Session collection reads in findRemovedSessions.
            // - Key collection refresh.
            // TODO (SERVER-122847): Remove this code.
            const bool isSystemCriticalNss = (_ns == NamespaceString::kLogicalSessionsNamespace ||
                                              _ns == NamespaceString::kKeysCollectionNamespace);
            boost::optional<admission::execution_control::ScopedTaskTypeNonDeprioritizable>
                systemCriticalTaskType;
            if (!gExecutionControlRemoteSpecification.isEnabledUseLastLTSFCVWhenUninitialized(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
                isSystemCriticalNss && opCtx->getClient()->isInternalClient()) {
                systemCriticalTaskType.emplace(opCtx);
            }

            const BSONObj& cmdObj = unparsedRequest().body;

            const bool isOplogNss = (_ns == NamespaceString::kRsOplogNamespace);

            auto cmdRequest = query_request_helper::makeFromFindCommand(request());

            // Start the query planning timer right after parsing.
            CurOp::get(opCtx)->beginQueryPlanningTimer();

            _rewriteFLEPayloads(opCtx, *cmdRequest);
            auto respSc =
                SerializationContext::stateCommandReply(cmdRequest->getSerializationContext());

            const bool isFindByUUID = cmdRequest->getNamespaceOrUUID().isUUID();
            uassert(ErrorCodes::InvalidOptions,
                    "When using the find command by UUID, the collectionUUID parameter cannot also "
                    "be specified",
                    !isFindByUUID || !cmdRequest->getCollectionUUID());

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "It is illegal to open a tailable cursor in a transaction",
                    !(opCtx->inMultiDocumentTransaction() && cmdRequest->getTailable()));

            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    "The 'readOnce' option is not supported within a transaction.",
                    !txnParticipant || !opCtx->inMultiDocumentTransaction() ||
                        !cmdRequest->getReadOnce());

            // Validate term before acquiring locks, if provided.
            auto term = cmdRequest->getTerm();
            if (term) {
                // Note: updateTerm returns ok if term stayed the same.
                uassertStatusOK(replCoord->updateTerm(opCtx, *term));
            }

            const bool includeMetrics = cmdRequest->getIncludeQueryStatsMetrics();

            // The presence of a term in the request indicates that this is an internal replication
            // oplog read request.
            boost::optional<ScopedAdmissionPriority<ExecutionAdmissionContext>> admissionPriority;
            if (term && isOplogNss) {
                if (MONGO_unlikely(hangBeforeFetcherFindCommandOnOplog.shouldFail())) {
                    LOGV2(10616500,
                          "Hit hangBeforeFetcherFindCommandOnOplog enabled, hanging while set");
                    CurOpFailpointHelpers::waitWhileFailPointEnabled(
                        &hangBeforeFetcherFindCommandOnOplog,
                        opCtx,
                        "hangBeforeFetcherFindCommandOnOplog");
                }
                // We do not want to wait to take tickets for internal (replication) oplog reads.
                // Stalling on ticket acquisition can cause complicated deadlocks. Primaries may
                // depend on data reaching secondaries in order to proceed; and secondaries may get
                // stalled replicating because of an inability to acquire a read ticket.
                admissionPriority.emplace(opCtx, AdmissionContext::Priority::kExempt);
            }

            // If this read represents a reverse oplog scan, we want to bypass oplog visibility
            // rules in the case of secondaries. We normally only read from these nodes at batch
            // boundaries, but in this specific case we should fetch all new entries, to be
            // consistent with any catalog changes that might be observable before the batch is
            // finalized. This special rule for reverse oplog scans is needed by replication
            // initial sync, for the purposes of calculating the stopTimestamp correctly.
            boost::optional<PinReadSourceBlock> pinReadSourceBlock;
            if (isOplogNss) {
                auto reverseScan = false;

                auto cmdSort = cmdRequest->getSort();
                if (!cmdSort.isEmpty()) {
                    BSONElement natural = cmdSort[query_request_helper::kNaturalSortField];
                    if (natural) {
                        reverseScan = natural.safeNumberInt() < 0;
                    }
                }

                auto isInternal = opCtx->getClient()->isInternalClient();
                if (MONGO_unlikely(allowExternalReadsForReverseOplogScanRule.shouldFail())) {
                    isInternal = true;
                }

                if (reverseScan && isInternal) {
                    pinReadSourceBlock.emplace(shard_role_details::getRecoveryUnit(opCtx));
                }
            }

            // Acquire locks. If the query is on a view, we release our locks and convert the query
            // request into an aggregation command.

            // TODO SERVER-79175: Make nicer. We need to instantiate the AutoStatsTracker before the
            // acquisition in case it would throw so we can ensure data is written to the profile
            // collection that some test may rely on. However, we might not know the namespace at
            // this point so it is wrapped in a boost::optional. If the request is with a UUID we
            // instantiate it after, but this is fine as the request should not be for sharded
            // collections.
            boost::optional<AutoStatsTracker> tracker;
            auto const initializeTracker = [&](const NamespaceString& nss) {
                tracker.emplace(opCtx,
                                nss,
                                Top::LockType::ReadLocked,
                                AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                DatabaseProfileSettings::get(opCtx->getServiceContext())
                                    .getDatabaseProfileLevel(nss.dbName()));
            };
            auto const nssOrUUID = cmdRequest->getNamespaceOrUUID();
            if (nssOrUUID.isNamespaceString()) {
                CommandHelpers::ensureValidCollectionName(nssOrUUID.nss());
                initializeTracker(nssOrUUID.nss());
            }
            const auto acquisitionRequest = [&] {
                auto req = CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx, nssOrUUID, AcquisitionPrerequisites::kRead);

                req.expectedUUID = cmdRequest->getCollectionUUID();
                return req;
            }();

            boost::optional<CollectionOrViewAcquisition> collectionOrView =
                acquireCollectionOrViewMaybeLockFree(opCtx, acquisitionRequest);
            const auto origNss = collectionOrView->nss();
            // TODO SERVER-121185: Remove if(isRawDataOperation) branch and the namespace
            // translation once 9.0 becomes last LTS
            if (isRawDataOperation(opCtx)) {
                auto [isTimeseriesViewRequest, translatedNs] =
                    timeseries::isTimeseriesViewRequest(opCtx, *cmdRequest);
                if (isTimeseriesViewRequest) {
                    cmdRequest->setNss(translatedNs);
                    collectionOrView = acquireCollectionOrViewMaybeLockFree(
                        opCtx,
                        CollectionOrViewAcquisitionRequest::fromOpCtx(
                            opCtx, translatedNs, AcquisitionPrerequisites::kRead));
                }
            }
            const NamespaceString nss = collectionOrView->nss();

            if (!tracker) {
                initializeTracker(nss);
            }

            if (!cmdRequest->getMirrored()) {
                if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                        opCtx,
                        ns(),
                        analyze_shard_key::SampledCommandNameEnum::kFind,
                        *cmdRequest)) {
                    analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                        ->addFindQuery(*sampleId,
                                       nss,
                                       cmdRequest->getFilter(),
                                       cmdRequest->getCollation(),
                                       cmdRequest->getLet())
                        .getAsync([](auto) {});
                }
            }

            // Going forward this operation must never ignore interrupt signals while waiting for
            // lock acquisition. This InterruptibleLockGuard will ensure that waiting for lock
            // re-acquisition after yielding will not ignore interrupt signals. This is necessary to
            // avoid deadlocking with replication rollback, which at the storage layer waits for all
            // cursors to be closed under the global MODE_X lock, after having sent interrupt
            // signals to read operations. This operation must never hold open storage cursors while
            // ignoring interrupt.
            InterruptibleLockGuard interruptibleLockAcquisition(opCtx);

            const auto& collectionPtr = collectionOrView->getCollectionPtr();

            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "UUID " << cmdRequest->getNamespaceOrUUID().uuid()
                                  << " specified in query request not found",
                    collectionOrView->collectionExists() || !isFindByUUID);

            bool isClusteredCollection = false;
            if (collectionOrView->collectionExists()) {
                if (isFindByUUID) {
                    // Replace the UUID in the find command with the fully qualified namespace of
                    // the looked up Collection.
                    cmdRequest->setNss(nss);
                }

                // Tailing a replicated capped clustered collection requires majority read concern.
                const bool isTailable = cmdRequest->getTailable();
                const bool isMajorityReadConcern = repl::ReadConcernArgs::get(opCtx).getLevel() ==
                    repl::ReadConcernLevel::kMajorityReadConcern;
                isClusteredCollection = collectionPtr->isClustered();
                const bool isCapped = collectionPtr->isCapped();
                const bool isReplicated = collectionPtr->ns().isReplicated();
                if (isClusteredCollection && isCapped && isReplicated && isTailable) {
                    uassert(ErrorCodes::Error(6049203),
                            "A tailable cursor on a capped clustered collection requires majority "
                            "read concern",
                            isMajorityReadConcern);
                }
            }

            // Views use the aggregation system and the $_resumeAfter/ $_startAt parameter is not
            // allowed. A more descriptive error will be raised later, but we want to validate this
            // parameter before beginning the operation.
            if (!collectionOrView->isView()) {
                uassertStatusOK(
                    query_request_helper::validateResumeInput(opCtx,
                                                              cmdRequest->getResumeAfter(),
                                                              cmdRequest->getStartAt(),
                                                              isClusteredCollection));
            }

            auto cq = parseQueryAndBeginOperation(
                opCtx, *collectionOrView, nss, unparsedRequest().body, std::move(cmdRequest));
            const auto& findCommandReq = cq->getFindCommandRequest();

            // Create an RAII object that prints useful information about the ExpressionContext in
            // the case of a tassert or crash.
            ScopedDebugInfo expCtxDiagnostics(
                "ExpCtxDiagnostics",
                diagnostic_printers::ExpressionContextPrinter{cq->getExpCtx()});

            tassert(7922501,
                    "CanonicalQuery namespace should match catalog namespace",
                    cq->nss() == nss);

            // If we are running a query against a view or a timeseries collection, redirect this
            // query through the aggregation system.
            if (collectionOrView->isView() ||
                timeseries::requiresViewlessTimeseriesTranslation(opCtx, *collectionOrView)) {
                // Relinquish locks. The aggregation command will re-acquire them.
                collectionOrView.reset();
                return runFindAsAgg(opCtx, *cq, boost::none /* verbosity */, replyBuilder);
            }
            // For the purposes of OpDebug's reporting, we only need 'collectionType' to distinguish
            // between view/timeseries/collection. For view/timeseries, 'collectionType' will be set
            // on the agg path taken above. In the normal path (i.e. here), we bypass the
            // getCollectionType() call and hardcode "kCollection" for performance reasons.
            CurOp::get(opCtx)->debug().collectionType = query_shape::CollectionType::kCollection;

            // Create an RAII object that prints the collection's shard key in the case of a tassert
            // or crash.
            auto collShardingDescription =
                collectionOrView->getCollection().getShardingDescription();
            ScopedDebugInfo shardKeyDiagnostics("ShardKeyDiagnostics",
                                                diagnostic_printers::ShardKeyDiagnosticPrinter{
                                                    collShardingDescription.isSharded()
                                                        ? collShardingDescription.getKeyPattern()
                                                        : BSONObj()});

            const auto& collection = collectionOrView->getCollection();

            if (!findCommandReq.getAllowDiskUse().value_or(true)) {
                allowDiskUseFalseCounter.increment();
            }

            // Check whether we are allowed to read from this node after acquiring our locks.
            uassertStatusOK(replCoord->checkCanServeReadsFor(
                opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

            if (findCommandReq.getReadOnce()) {
                // The readOnce option causes any storage-layer cursors created during plan
                // execution to assume read data will not be needed again and need not be cached.
                shard_role_details::getRecoveryUnit(opCtx)->setReadOnce(true);
            }

            // Get the execution plan for the query.
            auto exec = uassertStatusOK(getExecutorFind(opCtx,
                                                        MultipleCollectionAccessor{collection},
                                                        std::move(cq),
                                                        PlanYieldPolicy::YieldPolicy::YIELD_AUTO));

            {
                std::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setPlanSummary(lk, exec->getPlanExplainer().getPlanSummary());
                CurOp::get(opCtx)->debug().queryFramework = exec->getQueryFramework();
            }

            if (!collection.exists()) {
                // No collection. Just fill out curop indicating that there were zero results and
                // there is no ClientCursor id, and then return.
                const long long numResults = 0;
                const CursorId cursorId = 0;
                endQueryOp(opCtx, collectionPtr, *exec, numResults, boost::none, cmdObj);
                CursorResponseBuilder::Options options;
                options.isInitialResponse = true;
                CursorResponseBuilder builder(replyBuilder, options);
                boost::optional<CursorMetrics> metrics = includeMetrics
                    ? boost::make_optional(CurOp::get(opCtx)->debug().getCursorMetrics())
                    : boost::none;
                builder.done(cursorId, origNss, metrics, respSc);
                return;
            }

            FindCommon::waitInFindBeforeMakingBatch(
                opCtx, *exec->getCanonicalQuery(), &shardWaitInFindBeforeMakingBatch);

            const FindCommandRequest& originalFC =
                exec->getCanonicalQuery()->getFindCommandRequest();

            // Stream query results, adding them to a BSONArray as we go.
            CursorResponseBuilder::Options options;
            options.isInitialResponse = true;
            if (!opCtx->inMultiDocumentTransaction()) {
                options.atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
            }
            CursorResponseBuilder firstBatch(replyBuilder, options);

            uint64_t numResults = 0;

            // Enforce that the default batch size is used if not specified. Note: A batch
            // size of 0 means we actually want an empty first batch, unlike in get_more. We
            // also don't pre-allocate space for results here.
            const auto batchSize =
                originalFC.getBatchSize().get_value_or(query_request_helper::getDefaultBatchSize());

            try {
                numResults = batchedExecute(batchSize, exec.get(), firstBatch);
            } catch (DBException& exception) {
                firstBatch.abandon();

                auto&& explainer = exec->getPlanExplainer();
                auto&& [stats, _] =
                    explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
                LOGV2_WARNING(23798,
                              "Plan executor error during find command",
                              "error"_attr = exception.toStatus(),
                              "stats"_attr = redact(stats),
                              "cmd"_attr = redact(cmdObj));

                exception.addContext(str::stream() << "Executor error during find command: "
                                                   << nss.toStringForErrorMsg());
                throw;
            }

            // Set up the cursor for getMore.
            CursorId cursorId = 0;
            if (shouldSaveCursor(opCtx, collectionPtr, exec.get())) {
                ClientCursorPin pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                    opCtx,
                    {std::move(exec),
                     origNss,
                     AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                     APIParameters::get(opCtx),
                     opCtx->getWriteConcern(),
                     repl::ReadConcernArgs::get(opCtx),
                     ReadPreferenceSetting::get(opCtx),
                     unparsedRequest().body,
                     {Privilege(ResourcePattern::forExactNamespace(origNss), ActionType::find)}});
                ScopeGuard deleteCursorOnError([&] {
                    // In case of an error while creating and stashing the cursor we have to delete
                    // the underlying resources since they might have been left in an inconsistent
                    // state.
                    pinnedCursor.deleteUnderlying();
                });
                ON_BLOCK_EXIT([&] {
                    // During destruction of the pinned cursor we may discover that the operation
                    // has been interrupted. This would cause the transaction resources we just
                    // stashed to be released and destroyed. As we hold a reference here to them in
                    // the form of the acquisition which modifies the resources, we must release it
                    // now before destroying the pinned cursor.
                    collectionOrView.reset();
                });
                cursorId = pinnedCursor.getCursor()->cursorid();

                invariant(!exec);
                PlanExecutor* cursorExec = pinnedCursor.getCursor()->getExecutor();

                // State will be restored on getMore.
                cursorExec->saveState();
                cursorExec->detachFromOperationContext();

                // We assume that cursors created through a DBDirectClient are always used from
                // their original OperationContext, so we do not need to move time to and from the
                // cursor.
                if (!opCtx->getClient()->isInDirectClient()) {
                    pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(
                        opCtx->getRemainingMaxTimeMicros());
                }

                // Fill out curop based on the results.
                endQueryOp(opCtx, collectionPtr, *cursorExec, numResults, pinnedCursor, cmdObj);

                stashTransactionResourcesFromOperationContext(opCtx, pinnedCursor.getCursor());

                deleteCursorOnError.dismiss();
            } else {
                ON_BLOCK_EXIT([&] {
                    // We want to destroy the executor as soon as possible to release any resources
                    // locks it may hold.
                    exec.reset();
                    collectionOrView.reset();
                });
                endQueryOp(opCtx, collectionPtr, *exec, numResults, boost::none, cmdObj);
            }

            // Generate the response object to send to the client.
            boost::optional<CursorMetrics> metrics = includeMetrics
                ? boost::make_optional(CurOp::get(opCtx)->debug().getCursorMetrics())
                : boost::none;
            firstBatch.done(cursorId, origNss, metrics, respSc);

            query_request_helper::validateCursorResponse(replyBuilder->getBodyBuilder().asTempObj(),
                                                         auth::ValidatedTenancyScope::get(opCtx),
                                                         origNss.tenantId(),
                                                         respSc);
        }

        void runFindAsAgg(OperationContext* opCtx,
                          const CanonicalQuery& cq,
                          boost::optional<ExplainOptions::Verbosity> verbosity,
                          rpc::ReplyBuilderInterface* replyBuilder) {
            const auto hasExplain = verbosity.has_value();
            auto aggRequest = query_request_conversion::asAggregateCommandRequest(
                cq.getFindCommandRequest(), hasExplain);

            aggRequest.setQuerySettings(cq.getExpCtx()->getQuerySettings());

            // An empty PrivilegeVector for explain is acceptable because these privileges are only
            // checked on getMore and explain will not open a cursor.
            const auto privileges = verbosity ? PrivilegeVector{}
                                              : uassertStatusOK(auth::getPrivilegesForAggregate(
                                                    opCtx,
                                                    AuthorizationSession::get(opCtx->getClient()),
                                                    aggRequest.getNamespace(),
                                                    aggRequest,
                                                    false));
            // This will do view definition resolution for views and timeseries things for
            // timeseries queries.
            const auto status = runAggregate(opCtx,
                                             aggRequest,
                                             {aggRequest},
                                             unparsedRequest().body,
                                             privileges,
                                             verbosity,
                                             replyBuilder);
            if (status.code() == ErrorCodes::InvalidPipelineOperator) {
                uasserted(ErrorCodes::InvalidPipelineOperator,
                          str::stream{} << "Unsupported operator in converted pipeline: "
                                        << status.reason());
            }
            uassertStatusOK(status);
        }

        void appendMirrorableRequest(BSONObjBuilder* bob) const override {
            // Filter the keys that can be mirrored
            static const auto kMirrorableKeys = [] {
                BSONObjBuilder keyBob;
                keyBob.append("find", 1);
                keyBob.append("filter", 1);
                keyBob.append("skip", 1);
                keyBob.append("limit", 1);
                keyBob.append("sort", 1);
                keyBob.append("hint", 1);
                keyBob.append("collation", 1);
                keyBob.append("min", 1);
                keyBob.append("max", 1);
                keyBob.append("shardVersion", 1);
                keyBob.append("databaseVersion", 1);
                keyBob.append("encryptionInformation", 1);
                keyBob.append("rawData", 1);
                return keyBob.obj();
            }();

            unparsedRequest().body.filterFieldsUndotted(bob, kMirrorableKeys, true);

            // Tell the find to only return a single batch
            bob->append("batchSize", 1);
            bob->append("singleBatch", true);
        }

    private:
        const NamespaceString _ns;

        void _rewriteFLEPayloads(OperationContext* opCtx, FindCommandRequest& cmdRequest) {
            // Rewrite any FLE find payloads that exist in the query if this is a FLE 2 query.
            if (prepareForFLERewrite(opCtx, cmdRequest.getEncryptionInformation())) {
                tassert(9483400,
                        "Expecting a namespace string for find_cmd",
                        cmdRequest.getNamespaceOrUUID().isNamespaceString());
                processFLEFindD(opCtx, cmdRequest.getNamespaceOrUUID().nss(), &cmdRequest);
            }
        }
    };
};
MONGO_REGISTER_COMMAND(FindCmd).forShard();

}  // namespace
}  // namespace mongo
