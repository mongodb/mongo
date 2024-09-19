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
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/collection_type.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/command_diagnostic_printer.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/storage_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/sharding_state.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace {
// Ticks for server-side Javascript deprecation log messages.
Rarely _samplerFunctionJs, _samplerWhereClause;

MONGO_FAIL_POINT_DEFINE(allowExternalReadsForReverseOplogScanRule);

const auto kTermField = "term"_sd;


boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const FindCommandRequest& findCommand,
    const CollectionPtr& collPtr,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    std::unique_ptr<CollatorInterface> collator;
    if (!findCommand.getCollation().isEmpty()) {
        collator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                       ->makeFromBSON(findCommand.getCollation()));
    } else if (collPtr && collPtr->getDefaultCollator()) {
        // The 'collPtr' will be null for views, but we don't need to worry about views here. The
        // views will get rewritten into aggregate command and will regenerate the
        // ExpressionContext.
        collator = collPtr->getDefaultCollator()->clone();
    }
    auto expCtx =
        make_intrusive<ExpressionContext>(opCtx,
                                          findCommand,
                                          std::move(collator),
                                          CurOp::get(opCtx)->dbProfileLevel() > 0,  // mayDbProfile
                                          verbosity,
                                          allowDiskUseByDefault.load());
    expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";
    expCtx->startExpressionCounters();

    return expCtx;
}

/**
 * Fills out the CurOp for "opCtx" with information about this query.
 */
void beginQueryOp(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& queryObj) {
    auto curOp = CurOp::get(opCtx);
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    curOp->setOpDescription_inlock(queryObj);
    curOp->setNS_inlock(nss);
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
    auto expCtx =
        makeExpressionContext(opCtx, *findCommand, collection, boost::none /* verbosity */);
    auto parsedRequest = uassertStatusOK(parsed_find_command::parse(
        expCtx,
        {.findCommand = std::move(findCommand),
         .extensionsCallback = ExtensionsCallbackReal(opCtx, &nss),
         .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

    // Initialize system variables before constructing CanonicalQuery as the constructor
    // performs constant-folding optimizations which depend on these agg variables being
    // properly initialized.
    expCtx->initializeReferencedSystemVariables();

    // Register query stats collection. Exclude queries against collections with encrypted fields.
    // It is important to do this before canonicalizing and optimizing the query, each of which
    // would alter the query shape.
    if (!(collection && collection.get()->getCollectionOptions().encryptedFieldConfig)) {
        query_stats::registerRequest(opCtx, nss, [&]() {
            return std::make_unique<query_stats::FindKey>(
                expCtx, *parsedRequest, collOrViewAcquisition.getCollectionType());
        });

        if (parsedRequest->findCommandRequest->getIncludeQueryStatsMetrics() &&
            feature_flags::gFeatureFlagQueryStatsDataBearingNodes.isEnabled(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            CurOp::get(opCtx)->debug().queryStatsInfo.metricsRequested = true;
        }
    }

    // Check for server-side javascript usage after parsing is complete and the flags have been set
    // on the expression context.
    if (expCtx->hasServerSideJs.where && _samplerWhereClause.tick()) {
        LOGV2_WARNING(8996500,
                      "$where is deprecated. For more information, see "
                      "https://www.mongodb.com/docs/manual/reference/operator/query/where/");
    }

    if (expCtx->hasServerSideJs.function && _samplerFunctionJs.tick()) {
        LOGV2_WARNING(
            8996501,
            "$function is deprecated. For more information, see "
            "https://www.mongodb.com/docs/manual/reference/operator/aggregation/function/");
    }

    // TODO: SERVER-73632 Remove feature flag for PM-635.
    // Query settings will only be looked up on mongos and therefore should be part of command body
    // on mongod if present.
    expCtx->setQuerySettingsIfNotPresent(
        query_settings::lookupQuerySettingsForFind(expCtx, *parsedRequest, nss));
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = std::move(expCtx),
        .parsedFind = std::move(parsedRequest),
    });
}

/**
 * A command for running .find() queries.
 */
class FindCmd final : public Command {
public:
    FindCmd() : Command("find") {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        auto cmdRequest = _parseCmdObjectToFindCommandRequest(opCtx, opMsgRequest);
        return std::make_unique<Invocation>(this, opMsgRequest, std::move(cmdRequest));
    }

    std::unique_ptr<CommandInvocation> parseForExplain(
        OperationContext* opCtx,
        const OpMsgRequest& request,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity) override {
        auto cmdRequest = _parseCmdObjectToFindCommandRequest(opCtx, request);
        // Providing collection UUID for explain is forbidden, see SERVER-38821 and SERVER-38275
        uassert(ErrorCodes::InvalidNamespace,
                "Providing collection UUID for explain is forbidden",
                !cmdRequest->getNamespaceOrUUID().isUUID());
        return std::make_unique<Invocation>(this, request, std::move(cmdRequest));
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

    class Invocation final : public CommandInvocation {
    public:
        Invocation(const FindCmd* definition,
                   const OpMsgRequest& request,
                   std::unique_ptr<FindCommandRequest> cmdRequest)
            : CommandInvocation(definition),
              _request(request),
              _ns(cmdRequest->getNamespaceOrUUID().isNamespaceString()
                      ? cmdRequest->getNamespaceOrUUID().nss()
                      : NamespaceString(cmdRequest->getNamespaceOrUUID().dbName())),
              _genericArgs(cmdRequest->getGenericArguments()),
              _cmdRequest(std::move(cmdRequest)) {
            invariant(_request.body.isOwned());

            if (_cmdRequest->getMirrored().value_or(false)) {
                markMirrored();
            }
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return !_cmdRequest->getTerm().has_value();
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

        const GenericArguments& getGenericArguments() const override {
            // TODO SERVER-88444: retrieve this directly from _request rather than making a separate
            // copy.
            return _genericArgs;
        }

        const DatabaseName& db() const override {
            return _ns.dbName();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

            const auto hasTerm = _cmdRequest->getTerm().has_value();
            const NamespaceStringOrUUID& nsOrUUID = _cmdRequest->getNamespaceOrUUID();
            if (nsOrUUID.isNamespaceString()) {
                uassert(ErrorCodes::InvalidNamespace,
                        str::stream() << "Namespace " << nsOrUUID.toStringForErrorMsg()
                                      << " is not a valid collection name",
                        nsOrUUID.nss().isValid());
                uassertStatusOK(auth::checkAuthForFind(authSession, nsOrUUID.nss(), hasTerm));
            } else {
                const auto resolvedNss =
                    CollectionCatalog::get(opCtx)->resolveNamespaceStringFromDBNameAndUUID(
                        opCtx, nsOrUUID.dbName(), nsOrUUID.uuid());
                uassertStatusOK(auth::checkAuthForFind(authSession, resolvedNss, hasTerm));
            }
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* replyBuilder) override {
            // We want to start the query planning timer right after parsing. In the explain code
            // path, we have already parsed the FindCommandRequest, so start timing here.
            CurOp::get(opCtx)->beginQueryPlanningTimer();

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

            // Going forward this operation must never ignore interrupt signals while waiting for
            // lock acquisition. This InterruptibleLockGuard will ensure that waiting for lock
            // re-acquisition after yielding will not ignore interrupt signals. This is necessary to
            // avoid deadlocking with replication rollback, which at the storage layer waits for all
            // cursors to be closed under the global MODE_X lock, after having sent interrupt
            // signals to read operations. This operation must never hold open storage cursors while
            // ignoring interrupt.
            InterruptibleLockGuard interruptibleLockAcquisition(opCtx);

            _rewriteFLEPayloads(opCtx);
            auto respSc =
                SerializationContext::stateCommandReply(_cmdRequest->getSerializationContext());

            // The collection may be NULL. If so, getExecutor() should handle it by returning an
            // execution tree with an EOFStage.
            const auto& collectionPtr = collectionOrView->getCollectionPtr();
            if (!collectionOrView->isView()) {
                const bool isClusteredCollection = collectionPtr && collectionPtr->isClustered();
                uassertStatusOK(query_request_helper::validateResumeAfter(
                    opCtx, _cmdRequest->getResumeAfter(), isClusteredCollection));
            }
            auto expCtx = makeExpressionContext(opCtx, *_cmdRequest, collectionPtr, verbosity);
            auto parsedRequest = uassertStatusOK(parsed_find_command::parse(
                expCtx,
                {.findCommand = std::move(_cmdRequest),
                 .extensionsCallback = ExtensionsCallbackReal(opCtx, &_ns),
                 .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

            // Initialize system variables before constructing CanonicalQuery as the constructor
            // performs constant-folding optimizations which depend on these agg variables being
            // properly initialized.
            expCtx->initializeReferencedSystemVariables();

            expCtx->setQuerySettingsIfNotPresent(
                query_settings::lookupQuerySettingsForFind(expCtx, *parsedRequest, _ns));
            auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
                .expCtx = std::move(expCtx), .parsedFind = std::move(parsedRequest)});

            // If we are running a query against a view redirect this query through the aggregation
            // system.
            if (collectionOrView->isView()) {
                // Relinquish locks. The aggregation command will re-acquire them.
                collectionOrView.reset();
                auto curOp = CurOp::get(opCtx);
                curOp->debug().queryStatsInfo.disableForSubqueryExecution = true;
                return runFindOnView(opCtx, *cq, verbosity, replyBuilder);
            }

            // Get the execution plan for the query.
            const auto& collection = collectionOrView->getCollection();
            auto exec = uassertStatusOK(getExecutorFind(opCtx,
                                                        MultipleCollectionAccessor{collection},
                                                        std::move(cq),
                                                        PlanYieldPolicy::YieldPolicy::YIELD_AUTO));

            auto bodyBuilder = replyBuilder->getBodyBuilder();
            // Got the execution tree. Explain it.
            Explain::explainStages(
                exec.get(), collection, verbosity, BSONObj(), respSc, _request.body, &bodyBuilder);
        }

        /**
         * Helper function which handles outputting the first batch of documents generated by 'exec'
         * in 'firstBatch', returning the number of documents in the batch, and recording the number
         * & size of documents in the batch via 'docUnitsReturned'.
         */
        uint64_t batchedExecute(const size_t batchSize,
                                PlanExecutor* exec,
                                CursorResponseBuilder& firstBatch,
                                ResourceConsumption::DocumentUnitCounter& docUnitsReturned) {
            BSONObj pbrt = exec->getPostBatchResumeToken();
            size_t numResults = 0;
            bool failedToAppend = false;

            numResults = exec->getNextBatch(
                batchSize,
                FindCommon::BSONObjCursorAppender{true /* alwaysAcceptFirstDoc */,
                                                  exec,
                                                  &firstBatch,
                                                  &docUnitsReturned,
                                                  pbrt,
                                                  failedToAppend});

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

            const BSONObj& cmdObj = _request.body;

            // Capture diagnostics for tassert and invariant failures that may occur during query
            // parsing, planning or execution. No work is done on the hot-path, all computation of
            // these diagnostics is done lazily during failure handling. This line just creates an
            // RAII object which holds references to objects on this stack frame, which will be used
            // to print diagnostics in the event of a tassert or invariant.
            ScopedDebugInfo findCmdDiagnostics("commandDiagnostics",
                                               command_diagnostics::Printer{opCtx});

            // Parse the command BSON to a FindCommandRequest. Pass in the parsedNss in case cmdObj
            // does not have a UUID.
            const bool isOplogNss = (_ns == NamespaceString::kRsOplogNamespace);

            if (!_cmdRequest) {
                // We're rerunning the same invocation, so we have to parse again
                _cmdRequest = _parseCmdObjectToFindCommandRequest(opCtx, _request);
            }
            // Start the query planning timer right after parsing.
            CurOp::get(opCtx)->beginQueryPlanningTimer();

            _rewriteFLEPayloads(opCtx);
            auto respSc =
                SerializationContext::stateCommandReply(_cmdRequest->getSerializationContext());

            const bool isFindByUUID = _cmdRequest->getNamespaceOrUUID().isUUID();
            uassert(ErrorCodes::InvalidOptions,
                    "When using the find command by UUID, the collectionUUID parameter cannot also "
                    "be specified",
                    !isFindByUUID || !_cmdRequest->getCollectionUUID());

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "It is illegal to open a tailable cursor in a transaction",
                    !(opCtx->inMultiDocumentTransaction() && _cmdRequest->getTailable()));

            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    "The 'readOnce' option is not supported within a transaction.",
                    !txnParticipant || !opCtx->inMultiDocumentTransaction() ||
                        !_cmdRequest->getReadOnce());

            // Validate term before acquiring locks, if provided.
            auto term = _cmdRequest->getTerm();
            if (term) {
                // Note: updateTerm returns ok if term stayed the same.
                uassertStatusOK(replCoord->updateTerm(opCtx, *term));
            }

            const bool includeMetrics = _cmdRequest->getIncludeQueryStatsMetrics();

            // The presence of a term in the request indicates that this is an internal replication
            // oplog read request.
            boost::optional<ScopedAdmissionPriority<ExecutionAdmissionContext>> admissionPriority;
            if (term && isOplogNss) {
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

                auto cmdSort = _cmdRequest->getSort();
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
                                0 /* dbProfilingLevel */
                );
            };
            auto const nssOrUUID = _cmdRequest->getNamespaceOrUUID();
            if (nssOrUUID.isNamespaceString()) {
                CommandHelpers::ensureValidCollectionName(nssOrUUID.nss());
                initializeTracker(nssOrUUID.nss());
            }
            const auto acquisitionRequest = [&] {
                auto req = CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx, nssOrUUID, AcquisitionPrerequisites::kRead);

                req.expectedUUID = _cmdRequest->getCollectionUUID();
                return req;
            }();

            // The acquireCollection can throw before we raise the profile level, and some callers
            // expect to see profiler entries on errors that can throw in the acquisition. To avoid
            // getting an expensive CollectionCatalog snapshot an extra time before the collection
            // acquisition path, only do this if the collection acquisition fails. Note that this
            // still doesn't work correctly if UUID resolution fails.
            auto setProfileLevelOnError = ScopeGuard([&] {
                if (nssOrUUID.isNamespaceString()) {
                    CurOp::get(opCtx)->raiseDbProfileLevel(
                        DatabaseProfileSettings::get(opCtx->getServiceContext())
                            .getDatabaseProfileLevel(nssOrUUID.dbName()));
                }
            });

            boost::optional<CollectionOrViewAcquisition> collectionOrView =
                acquireCollectionOrViewMaybeLockFree(opCtx, acquisitionRequest);
            const NamespaceString nss = collectionOrView->nss();

            // It is cheaper to raise the profiling level here, now that a CollectionCatalog
            // snapshot is stashed on the OpCtx.
            CurOp::get(opCtx)->raiseDbProfileLevel(
                DatabaseProfileSettings::get(opCtx->getServiceContext())
                    .getDatabaseProfileLevel(nss.dbName()));
            setProfileLevelOnError.dismiss();

            if (!tracker) {
                initializeTracker(nss);
            }

            if (!_cmdRequest->getMirrored()) {
                if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                        opCtx,
                        ns(),
                        analyze_shard_key::SampledCommandNameEnum::kFind,
                        *_cmdRequest)) {
                    analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                        ->addFindQuery(*sampleId,
                                       nss,
                                       _cmdRequest->getFilter(),
                                       _cmdRequest->getCollation(),
                                       _cmdRequest->getLet())
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
                    str::stream() << "UUID " << _cmdRequest->getNamespaceOrUUID().uuid()
                                  << " specified in query request not found",
                    collectionOrView->collectionExists() || !isFindByUUID);

            bool isClusteredCollection = false;
            if (collectionOrView->collectionExists()) {
                if (isFindByUUID) {
                    // Replace the UUID in the find command with the fully qualified namespace of
                    // the looked up Collection.
                    _cmdRequest->setNss(nss);
                }

                // Tailing a replicated capped clustered collection requires majority read concern.
                const bool isTailable = _cmdRequest->getTailable();
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

            // Views use the aggregation system and the $_resumeAfter parameter is not allowed. A
            // more descriptive error will be raised later, but we want to validate this parameter
            // before beginning the operation.
            if (!collectionOrView->isView()) {
                uassertStatusOK(query_request_helper::validateResumeAfter(
                    opCtx, _cmdRequest->getResumeAfter(), isClusteredCollection));
            }

            auto cq = parseQueryAndBeginOperation(
                opCtx, *collectionOrView, nss, _request.body, std::move(_cmdRequest));
            const auto& findCommandReq = cq->getFindCommandRequest();

            tassert(7922501,
                    "CanonicalQuery namespace should match catalog namespace",
                    cq->nss() == nss);

            // If we are running a query against a view redirect this query through the aggregation
            // system.
            if (collectionOrView->isView()) {
                // Relinquish locks. The aggregation command will re-acquire them.
                collectionOrView.reset();
                return runFindOnView(opCtx, *cq, boost::none /* verbosity */, replyBuilder);
            }

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

            // If the executor supports it, find operations will maintain the storage engine state
            // across commands.
            if (gMaintainValidCursorsAcrossReadCommands && !opCtx->inMultiDocumentTransaction()) {
                exec->enableSaveRecoveryUnitAcrossCommandsIfSupported();
            }

            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
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
                builder.done(cursorId, nss, metrics, respSc);
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
            ResourceConsumption::DocumentUnitCounter docUnitsReturned;

            // Enforce that the default batch size is used if not specified. Note: A batch
            // size of 0 means we actually want an empty first batch, unlike in get_more. We
            // also don't pre-allocate space for results here.
            const auto batchSize =
                originalFC.getBatchSize().get_value_or(query_request_helper::getDefaultBatchSize());

            try {
                numResults = batchedExecute(batchSize, exec.get(), firstBatch, docUnitsReturned);
            } catch (DBException& exception) {
                firstBatch.abandon();

                auto&& explainer = exec->getPlanExplainer();
                auto&& [stats, _] =
                    explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
                LOGV2_WARNING(23798,
                              "Plan executor error during find command",
                              "error"_attr = exception.toStatus(),
                              "stats"_attr = redact(stats),
                              "cmd"_attr = cmdObj);

                exception.addContext(str::stream() << "Executor error during find command: "
                                                   << nss.toStringForErrorMsg());
                throw;
            }

            // Set up the cursor for getMore.
            CursorId cursorId = 0;
            if (shouldSaveCursor(opCtx, collectionPtr, exec.get())) {
                const bool stashResourcesForGetMore =
                    exec->isSaveRecoveryUnitAcrossCommandsEnabled();
                ClientCursorPin pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                    opCtx,
                    {std::move(exec),
                     nss,
                     AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                     APIParameters::get(opCtx),
                     opCtx->getWriteConcern(),
                     repl::ReadConcernArgs::get(opCtx),
                     ReadPreferenceSetting::get(opCtx),
                     _request.body,
                     {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)}});
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

                if (stashResourcesForGetMore) {
                    // Collect storage stats now before we stash the recovery unit. These stats are
                    // normally collected in the service entry point layer just before a command
                    // ends, but they must be collected before stashing the RecoveryUnit. Otherwise,
                    // the service entry point layer will collect the stats from the new
                    // RecoveryUnit, which wasn't actually used for the query.
                    //
                    // The stats collected here will not get overwritten, as the service entry
                    // point layer will only set these stats when they're not empty.
                    CurOp::get(opCtx)->debug().storageStats =
                        shard_role_details::getRecoveryUnit(opCtx)
                            ->computeOperationStatisticsSinceLastCall();
                }

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
            firstBatch.done(cursorId, nss, metrics, respSc);

            // Increment this metric once we have generated a response and we know it will return
            // documents.
            auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
            metricsCollector.incrementDocUnitsReturned(toStringForLogging(nss), docUnitsReturned);
            query_request_helper::validateCursorResponse(replyBuilder->getBodyBuilder().asTempObj(),
                                                         auth::ValidatedTenancyScope::get(opCtx),
                                                         nss.tenantId(),
                                                         respSc);
        }

        void runFindOnView(OperationContext* opCtx,
                           const CanonicalQuery& cq,
                           boost::optional<ExplainOptions::Verbosity> verbosity,
                           rpc::ReplyBuilderInterface* replyBuilder) {
            auto aggRequest =
                query_request_conversion::asAggregateCommandRequest(cq.getFindCommandRequest());
            aggRequest.setExplain(verbosity);
            aggRequest.setQuerySettings(cq.getExpCtx()->getQuerySettings());

            // An empty PrivilegeVector for explain is acceptable because these privileges are only
            // checked on getMore and explain will not open a cursor.
            const auto privileges = verbosity ? PrivilegeVector()
                                              : uassertStatusOK(auth::getPrivilegesForAggregate(
                                                    AuthorizationSession::get(opCtx->getClient()),
                                                    aggRequest.getNamespace(),
                                                    aggRequest,
                                                    false));
            const auto status = runAggregate(opCtx,
                                             aggRequest,
                                             {aggRequest},
                                             _request.body,
                                             privileges,
                                             replyBuilder,
                                             {} /* usedExternalDataSources  */);
            if (status.code() == ErrorCodes::InvalidPipelineOperator) {
                uasserted(ErrorCodes::InvalidPipelineOperator,
                          str::stream() << "Unsupported in view pipeline: " << status.reason());
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
                return keyBob.obj();
            }();

            _request.body.filterFieldsUndotted(bob, kMirrorableKeys, true);

            // Tell the find to only return a single batch
            bob->append("batchSize", 1);
            bob->append("singleBatch", true);
        }

    private:
        const OpMsgRequest _request;
        const DatabaseName _dbName;
        const NamespaceString _ns;
        const GenericArguments _genericArgs;
        std::unique_ptr<FindCommandRequest> _cmdRequest;

        void _rewriteFLEPayloads(OperationContext* opCtx) {
            // Rewrite any FLE find payloads that exist in the query if this is a FLE 2 query.
            if (shouldDoFLERewrite(_cmdRequest)) {
                invariant(_cmdRequest->getNamespaceOrUUID().isNamespaceString());

                if (!_cmdRequest->getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    processFLEFindD(
                        opCtx, _cmdRequest->getNamespaceOrUUID().nss(), _cmdRequest.get());
                }
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
            }
        }
    };

private:
    // Parses the command object to a FindCommandRequest. If the client request did not specify
    // any runtime constants, make them available to the query here.
    static std::unique_ptr<FindCommandRequest> _parseCmdObjectToFindCommandRequest(
        OperationContext* opCtx, const OpMsgRequest& request) {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

        uassert(ErrorCodes::Unauthorized,
                "Unauthorized",
                authSession->isAuthorizedToParseNamespaceElement(request.body.firstElement()));

        // check validated tenantId and set the flag on the serialization context object
        auto reqSc = request.getSerializationContext();
        const boost::optional<auth::ValidatedTenancyScope>& vts =
            auth::ValidatedTenancyScope::get(opCtx);

        auto findCommand = query_request_helper::makeFromFindCommand(
            request.body,
            vts,
            vts.has_value() ? boost::make_optional(vts->tenantId()) : boost::none,
            reqSc);

        if (auto& nss = findCommand->getNamespaceOrUUID(); nss.isNamespaceString()) {
            CommandHelpers::ensureValidCollectionName(nss.nss());
        }

        // TODO: SERVER-73632 Remove Feature Flag for PM-635.
        // Forbid users from passing 'querySettings' explicitly.
        uassert(7746901,
                "BSON field 'querySettings' is an unknown field",
                query_settings::utils::allowQuerySettingsFromClient(opCtx->getClient()) ||
                    !findCommand->getQuerySettings().has_value());

        return findCommand;
    }
};
MONGO_REGISTER_COMMAND(FindCmd).forShard();

}  // namespace
}  // namespace mongo
