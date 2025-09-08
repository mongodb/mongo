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
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/bson/multikey_dotted_path_support.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/client_cursor/collect_query_stats_mongod.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_diagnostic_printer.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_stats/distinct_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/serialization_context.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace {

std::unique_ptr<CanonicalQuery> parseDistinctCmd(
    OperationContext* opCtx,
    const CollectionOrViewAcquisition& collOrViewAcquisition,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ExtensionsCallback& extensionsCallback,
    const CollatorInterface* defaultCollator,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    const auto serializationContext = vts != boost::none
        ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
        : SerializationContext::stateCommandRequest();

    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        cmdObj,
        IDLParserContext("distinctCommandRequest", vts, nss.tenantId(), serializationContext)));

    // Start the query planning timer right after parsing.
    CurOp::get(opCtx)->beginQueryPlanningTimer();

    // Forbid users from passing 'querySettings' explicitly.
    uassert(7923000,
            "BSON field 'querySettings' is an unknown field",
            query_settings::allowQuerySettingsFromClient(opCtx->getClient()) ||
                !distinctCommand->getQuerySettings().has_value());

    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, *distinctCommand, defaultCollator)
                      .ns(nss)
                      .explain(verbosity)
                      .build();
    auto parsedDistinct =
        parsed_distinct_command::parse(expCtx,
                                       std::move(distinctCommand),
                                       extensionsCallback,
                                       MatchExpressionParser::kAllowAllSpecialFeatures);

    // Compute QueryShapeHash and record it in CurOp.
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::DistinctCmdShape>(*parsedDistinct, expCtx);
    }};
    auto queryShapeHash = shape_helpers::computeQueryShapeHash(expCtx, deferredShape, nss);
    CurOp::get(opCtx)->debug().setQueryShapeHashIfNotPresent(opCtx, queryShapeHash);

    // Perform the query settings lookup and attach it to 'expCtx'.
    auto& querySettingsService = query_settings::QuerySettingsService::get(opCtx);
    auto querySettings = querySettingsService.lookupQuerySettingsWithRejectionCheck(
        expCtx, queryShapeHash, nss, parsedDistinct->distinctCommandRequest->getQuerySettings());
    expCtx->setQuerySettingsIfNotPresent(std::move(querySettings));

    // We do not collect queryStats on explain for distinct.
    if (feature_flags::gFeatureFlagQueryStatsCountDistinct.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        !verbosity.has_value()) {
        query_stats::registerRequest(opCtx, nss, [&]() {
            uassertStatusOKWithContext(deferredShape->getStatus(), "Failed to compute query shape");
            return std::make_unique<query_stats::DistinctKey>(
                expCtx,
                *parsedDistinct->distinctCommandRequest,
                std::move(deferredShape->getValue()),
                collOrViewAcquisition.getCollectionType());
        });

        if (parsedDistinct->distinctCommandRequest->getIncludeQueryStatsMetrics()) {
            CurOp::get(opCtx)->debug().queryStatsInfo.metricsRequested = true;
        }
    }

    return parsed_distinct_command::parseCanonicalQuery(
        std::move(expCtx), std::move(parsedDistinct), nullptr);
}

namespace mdps = multikey_dotted_path_support;

namespace {
// This function might create a classic or SBE plan executor. It relies on some assumptions that are
// specific to the distinct() command and shouldn't be blindly reused in other "distinct" contexts.
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> createExecutorForDistinctCommand(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    const CollectionAcquisition& coll) {
    const auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;
    const auto& collectionPtr = coll.getCollectionPtr();
    const MultipleCollectionAccessor collections{coll};

    const bool isFeatureFlagShardFilteringDistinctScanEnabled =
        canonicalQuery->getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled();

    // If there's a $natural hint via query settings, we need to go through the query planner to
    // ensure it gets enforced.
    const auto hasNaturalHintViaQuerySettings = [&] {
        const auto& indexHintSpecs =
            canonicalQuery->getExpCtx()->getQuerySettings().getIndexHints();
        if (!indexHintSpecs) {
            return false;
        }
        for (const auto& hintSpec : *indexHintSpecs) {
            for (const auto& hint : hintSpec.getAllowedIndexes()) {
                if (hint.getNaturalHint()) {
                    return true;
                }
            }
        }
        return false;
    };

    // If the query has no filter or sort, there's no need to do multiplanning and we will
    // short-cut the planner by immediately picking the index with the smallest suitable key.
    // Ultimately the query planner would do the same, but the additional planning work done
    // before that can add up to a noticeable regression for fast queries.
    const bool shouldMultiplan = isFeatureFlagShardFilteringDistinctScanEnabled &&
        (!canonicalQuery->getFindCommandRequest().getFilter().isEmpty() ||
         canonicalQuery->getSortPattern() || hasNaturalHintViaQuerySettings());

    if (shouldMultiplan) {
        return uassertStatusOK(
            getExecutorFind(opCtx,
                            collections,
                            std::move(canonicalQuery),
                            yieldPolicy,
                            // TODO SERVER-93018: Investigate why we prefer a collection scan
                            // against a 'GENERATE_COVERED_IXSCANS' when no filter is present.
                            QueryPlannerParams::DEFAULT));
    }

    size_t plannerOptions = QueryPlannerParams::DEFAULT;
    if (isFeatureFlagShardFilteringDistinctScanEnabled &&
        OperationShardingState::isComingFromRouter(opCtx)) {
        plannerOptions |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    }

    // If the collection doesn't exist 'getExecutor()' should create an EOF plan for it no
    // matter the query.
    if (!collectionPtr) {
        return uassertStatusOK(
            getExecutorFind(opCtx, collections, std::move(canonicalQuery), yieldPolicy));
    }

    // Try creating a plan that does DISTINCT_SCAN.
    auto swQuerySolution =
        tryGetQuerySolutionForDistinct(collections, plannerOptions, *canonicalQuery);
    if (swQuerySolution.isOK()) {
        return uassertStatusOK(getExecutorDistinct(collections,
                                                   plannerOptions,
                                                   std::move(canonicalQuery),
                                                   std::move(swQuerySolution.getValue())));
    }

    // If there is no DISTINCT_SCAN plan, create whatever non-distinct plan is appropriate, because
    // 'distinct()' command is capable of de-duplicating and unwinding its inputs. Note: In order to
    // allow a covered DISTINCT_SCAN we've inserted a projection -- there is no point of keeping it
    // if a DISTINCT_SCAN didn't bake out.
    auto findCommand =
        std::make_unique<FindCommandRequest>(canonicalQuery->getFindCommandRequest());
    findCommand->setProjection(BSONObj());

    auto cqWithoutProjection = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = canonicalQuery->getExpCtx(),
        .parsedFind =
            ParsedFindCommandParams{
                .findCommand = std::move(findCommand),
                .extensionsCallback = ExtensionsCallbackReal(opCtx, &collectionPtr->ns()),
                .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures},
    });

    return uassertStatusOK(
        getExecutorFind(opCtx, collections, std::move(cqWithoutProjection), yieldPolicy));
}

template <class NamespaceType>
BSONObj translateCmdObjForRawData(OperationContext* opCtx,
                                  const BSONObj& cmdObj,
                                  NamespaceType& ns,
                                  boost::optional<CollectionOrViewAcquisition>& collectionOrView,
                                  const std::function<CollectionOrViewAcquisition()>& acquire) {
    if (OptionalBool::parseFromBSON(cmdObj[DistinctCommandRequest::kRawDataFieldName])) {
        const auto vts = auth::ValidatedTenancyScope::get(opCtx);
        const auto serializationContext = vts != boost::none
            ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
            : SerializationContext::stateCommandRequest();

        auto [isTimeseriesViewRequest, translatedNs] = timeseries::isTimeseriesViewRequest(
            opCtx,
            DistinctCommandRequest::parse(
                cmdObj,
                IDLParserContext{"rawData", vts, ns.dbName().tenantId(), serializationContext}));
        if (isTimeseriesViewRequest) {
            ns = translatedNs;
            collectionOrView = acquire();
            return rewriteCommandForRawDataOperation<DistinctCommandRequest>(cmdObj,
                                                                             translatedNs.coll());
        }
    }

    return cmdObj;
}
}  // namespace

class DistinctCommand : public BasicCommand {
public:
    DistinctCommand() : BasicCommand("distinct") {}

    std::string help() const override {
        return "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    bool canIgnorePrepareConflicts() const override {
        return true;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
    }

    bool supportsRawData() const override {
        return true;
    }

    bool isSubjectToIngressAdmissionControl() const override {
        return true;
    }

    bool shouldAffectReadOptionCounters() const override {
        return true;
    }

    bool supportsReadMirroring(const BSONObj&) const override {
        return true;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbname,
                                 const BSONObj& cmdObj) const override {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

        if (!authSession->isAuthorizedToParseNamespaceElement(cmdObj.firstElement())) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        const auto hasTerm = false;
        const auto nsOrUUID = CommandHelpers::parseNsOrUUID(dbname, cmdObj);
        if (nsOrUUID.isNamespaceString()) {
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Namespace " << nsOrUUID.toStringForErrorMsg()
                                  << " is not a valid collection name",
                    nsOrUUID.nss().isValid());
            return auth::checkAuthForFind(authSession, nsOrUUID.nss(), hasTerm);
        }

        const auto resolvedNss = shard_role_nocheck::resolveNssWithoutAcquisition(
            opCtx, nsOrUUID.dbName(), nsOrUUID.uuid());
        return auth::checkAuthForFind(authSession, resolvedNss, hasTerm);
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* replyBuilder) const override {
        const DatabaseName dbName = request.parseDbName();
        const BSONObj& originalCmdObj = request.body;
        // Acquire locks. The RAII object is optional, because in the case of a view, the locks
        // need to be released.
        auto nss = CommandHelpers::parseNsCollectionRequired(dbName, originalCmdObj);

        AutoStatsTracker tracker(opCtx,
                                 nss,
                                 Top::LockType::ReadLocked,
                                 AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                 DatabaseProfileSettings::get(opCtx->getServiceContext())
                                     .getDatabaseProfileLevel(nss.dbName()));

        auto acquire = [&] {
            return acquireCollectionOrViewMaybeLockFree(
                opCtx,
                CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx, nss, AcquisitionPrerequisites::kRead));
        };
        boost::optional<CollectionOrViewAcquisition> collectionOrView = acquire();

        auto cmdObj =
            translateCmdObjForRawData(opCtx, originalCmdObj, nss, collectionOrView, acquire);

        const CollatorInterface* defaultCollator = collectionOrView->getCollectionPtr()
            ? collectionOrView->getCollectionPtr()->getDefaultCollator()
            : nullptr;

        auto canonicalQuery = parseDistinctCmd(opCtx,
                                               *collectionOrView,
                                               nss,
                                               cmdObj,
                                               ExtensionsCallbackReal(opCtx, &nss),
                                               defaultCollator,
                                               verbosity);

        // Create an RAII object that prints useful information about the ExpressionContext in the
        // case of a tassert or crash.
        ScopedDebugInfo expCtxDiagnostics(
            "ExpCtxDiagnostics",
            diagnostic_printers::ExpressionContextPrinter{canonicalQuery->getExpCtx()});

        if (collectionOrView->isView() ||
            timeseries::requiresViewlessTimeseriesTranslation(opCtx, *collectionOrView)) {
            // Relinquish locks. The aggregation command will re-acquire them.
            collectionOrView.reset();
            runDistinctAsAgg(opCtx, std::move(canonicalQuery), verbosity, replyBuilder);
            return Status::OK();
        }

        // Create an RAII object that prints the collection's shard key in the case of a tassert or
        // crash.
        auto collShardingDescription = collectionOrView->getCollection().getShardingDescription();
        ScopedDebugInfo shardKeyDiagnostics("ShardKeyDiagnostics",
                                            diagnostic_printers::ShardKeyDiagnosticPrinter{
                                                collShardingDescription.isSharded()
                                                    ? collShardingDescription.getKeyPattern()
                                                    : BSONObj()});

        auto executor = createExecutorForDistinctCommand(
            opCtx, std::move(canonicalQuery), collectionOrView->getCollection());
        SerializationContext serializationCtx = request.getSerializationContext();
        auto bodyBuilder = replyBuilder->getBodyBuilder();

        ScopedDebugInfo explainDiagnostics(
            "explainDiagnostics", diagnostic_printers::ExplainDiagnosticPrinter{executor.get()});
        Explain::explainStages(executor.get(),
                               collectionOrView->getCollection(),
                               verbosity,
                               BSONObj(),
                               SerializationContext::stateCommandReply(serializationCtx),
                               originalCmdObj,
                               &bodyBuilder);
        return Status::OK();
    }

    bool runWithReplyBuilder(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& originalCmdObj,
                             rpc::ReplyBuilderInterface* replyBuilder) override {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

        // Acquire locks and resolve possible UUID. The RAII object is optional, because in the case
        // of a view, the locks need to be released.

        // TODO SERVER-79175: Make nicer. We need to instantiate the AutoStatsTracker before the
        // acquisition in case it would throw so we can ensure data is written to the profile
        // collection that some test may rely on. However, we might not know the namespace at this
        // point so it is wrapped in a boost::optional. If the request is with a UUID we instantiate
        // it after, but this is fine as the request should not be for sharded collections.
        boost::optional<AutoStatsTracker> tracker;
        auto const initializeTracker = [&](const NamespaceString& nss) {
            tracker.emplace(opCtx,
                            nss,
                            Top::LockType::ReadLocked,
                            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                            DatabaseProfileSettings::get(opCtx->getServiceContext())
                                .getDatabaseProfileLevel(nss.dbName()));
        };
        auto nssOrUUID = CommandHelpers::parseNsOrUUID(dbName, originalCmdObj);

        if (nssOrUUID.isNamespaceString()) {
            initializeTracker(nssOrUUID.nss());
        }
        auto acquire = [&] {
            return acquireCollectionOrViewMaybeLockFree(
                opCtx,
                CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx, nssOrUUID, AcquisitionPrerequisites::kRead));
        };
        boost::optional<CollectionOrViewAcquisition> collectionOrView = acquire();

        auto cmdObj =
            translateCmdObjForRawData(opCtx, originalCmdObj, nssOrUUID, collectionOrView, acquire);
        const auto nss = collectionOrView->nss();

        if (!tracker) {
            initializeTracker(nss);
        }

        if (collectionOrView->isCollection()) {
            const auto& coll = collectionOrView->getCollection();
            // Distinct doesn't filter orphan documents so it is not allowed to run on sharded
            // collections in multi-document transactions.
            uassert(
                ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot run 'distinct' on a sharded collection in a multi-document transaction. "
                "Please see http://dochub.mongodb.org/core/transaction-distinct for a recommended "
                "alternative.",
                !opCtx->inMultiDocumentTransaction() || !coll.getShardingDescription().isSharded());

            // Similarly, we ban readConcern level snapshot for sharded collections.
            uassert(
                ErrorCodes::InvalidOptions,
                "Cannot run 'distinct' on a sharded collection with readConcern level 'snapshot'",
                repl::ReadConcernArgs::get(opCtx).getLevel() !=
                        repl::ReadConcernLevel::kSnapshotReadConcern ||
                    !coll.getShardingDescription().isSharded());
        }
        const CollatorInterface* defaultCollation = collectionOrView->getCollectionPtr()
            ? collectionOrView->getCollectionPtr()->getDefaultCollator()
            : nullptr;

        auto canonicalQuery = parseDistinctCmd(opCtx,
                                               *collectionOrView,
                                               nss,
                                               cmdObj,
                                               ExtensionsCallbackReal(opCtx, &nss),
                                               defaultCollation,
                                               {});
        const CanonicalDistinct& canonicalDistinct = *canonicalQuery->getDistinct();

        // Create an RAII object that prints useful information about the ExpressionContext in the
        // case of a tassert or crash.
        ScopedDebugInfo expCtxDiagnostics(
            "ExpCtxDiagnostics",
            diagnostic_printers::ExpressionContextPrinter{canonicalQuery->getExpCtx()});

        if (canonicalDistinct.isMirrored()) {
            const auto& invocation = CommandInvocation::get(opCtx);
            invocation->markMirrored();
        } else if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                       opCtx,
                       nss,
                       analyze_shard_key::SampledCommandNameEnum::kDistinct,
                       canonicalDistinct)) {
            analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                ->addDistinctQuery(*sampleId,
                                   nss,
                                   canonicalQuery->getQueryObj(),
                                   canonicalQuery->getFindCommandRequest().getCollation())
                .getAsync([](auto) {});
        }

        if (collectionOrView->isView() ||
            timeseries::requiresViewlessTimeseriesTranslation(opCtx, *collectionOrView)) {
            // Relinquish locks. The aggregation command will re-acquire them.
            collectionOrView.reset();
            runDistinctAsAgg(
                opCtx, std::move(canonicalQuery), boost::none /* verbosity */, replyBuilder);
            return true;
        }

        // Create an RAII object that prints the collection's shard key in the case of a tassert or
        // crash.
        auto collShardingDescription = collectionOrView->getCollection().getShardingDescription();
        ScopedDebugInfo shardKeyDiagnostics("ShardKeyDiagnostics",
                                            diagnostic_printers::ShardKeyDiagnosticPrinter{
                                                collShardingDescription.isSharded()
                                                    ? collShardingDescription.getKeyPattern()
                                                    : BSONObj()});

        // Check whether we are allowed to read from this node after acquiring our locks.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassertStatusOK(replCoord->checkCanServeReadsFor(
            opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

        auto executor = createExecutorForDistinctCommand(
            opCtx, std::move(canonicalQuery), collectionOrView->getCollection());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setPlanSummary(lk, executor->getPlanExplainer().getPlanSummary());
        }

        const auto key = cmdObj.getStringField(CanonicalDistinct::kKeyField);

        std::vector<BSONObj> distinctValueHolder;
        BSONElementSet values(executor->getCanonicalQuery()->getCollator());

        const int kMaxResponseSize = BSONObjMaxUserSize - 4096;

        // Capture diagnostics to be logged in the case of a failure.
        ScopedDebugInfo explainDiagnostics(
            "explainDiagnostics", diagnostic_printers::ExplainDiagnosticPrinter{executor.get()});
        try {
            size_t listApproxBytes = 0;
            BSONObj obj;
            while (PlanExecutor::ADVANCED == executor->getNext(&obj, nullptr)) {
                // Distinct expands arrays.
                //
                // If our query is covered, each value of the key should be in the index key and
                // available to us without this.  If a collection scan is providing the data, we may
                // have to expand an array.
                BSONElementSet elts;
                mdps::extractAllElementsAlongPath(obj, key, elts);

                for (BSONElementSet::iterator it = elts.begin(); it != elts.end(); ++it) {
                    BSONElement elt = *it;
                    if (values.count(elt)) {
                        continue;
                    }

                    // This is an approximate size check which safeguards against use of unbounded
                    // memory by the distinct command. We perform a more precise check at the end of
                    // this method to confirm that the response size is less than 16MB.
                    listApproxBytes += elt.size();
                    uassert(
                        17217, "distinct too big, 16mb cap", listApproxBytes < kMaxResponseSize);

                    auto distinctObj = elt.wrap();
                    values.insert(distinctObj.firstElement());
                    distinctValueHolder.push_back(std::move(distinctObj));
                }
            }
        } catch (DBException& exception) {
            auto&& explainer = executor->getPlanExplainer();
            auto&& [stats, _] =
                explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
            LOGV2_WARNING(23797,
                          "Plan executor error during distinct command",
                          "error"_attr = exception.toStatus(),
                          "stats"_attr = redact(stats),
                          "cmd"_attr = redact(cmdObj));

            exception.addContext(str::stream()
                                 << "Executor error during distinct command on namespace: "
                                 << nss.toStringForErrorMsg());
            throw;
        }

        auto curOp = CurOp::get(opCtx);
        const auto& collection = collectionOrView->getCollectionPtr();

        // Get summary information about the plan.
        PlanSummaryStats stats;
        auto&& explainer = executor->getPlanExplainer();
        explainer.getSummaryStats(&stats);
        if (collection) {
            CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
                collection.get(),
                stats.collectionScans,
                stats.collectionScansNonTailable,
                stats.indexesUsed);
        }

        curOp->debug().setPlanSummaryMetrics(std::move(stats));
        curOp->setEndOfOpMetrics(values.size());

        if (curOp->shouldDBProfile()) {
            auto&& [stats, _] =
                explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
            curOp->debug().execStats = std::move(stats);
        }

        BSONObjBuilder result = replyBuilder->getBodyBuilder();
        BSONArrayBuilder valueListBuilder(result.subarrayStart("values"));
        for (const auto& value : values) {
            valueListBuilder.append(value);
        }
        valueListBuilder.doneFast();

        if (!opCtx->inMultiDocumentTransaction() &&
            repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
            result.append("atClusterTime"_sd,
                          repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()->asTimestamp());
        }

        uassert(31299, "distinct too big, 16mb cap", result.len() < kMaxResponseSize);

        auto* cq = executor->getCanonicalQuery();
        collectQueryStatsMongod(
            opCtx, cq->getExpCtx(), std::move(curOp->debug().queryStatsInfo.key));

        // Include queryStats metrics in the result to be sent to mongos.
        const bool includeMetrics = CurOp::get(opCtx)->debug().queryStatsInfo.metricsRequested;

        if (includeMetrics) {
            // It is safe to unconditionally add the metrics because we are assured that the user
            // data will not exceed the user size limit, and the limit enforced for sending the
            // entire message is actually several KB larger, intentionally leaving buffer for
            // metadata like this.
            auto metrics = CurOp::get(opCtx)->debug().getCursorMetrics().toBSON();
            result.append("metrics", metrics);
        }

        return true;
    }

    /**
     * This method is defined by the parent class and is supposed to be directly invoked by
     * runWithReplyBuilder(). However, since runWithReplyBuilder is overriden here, run() method
     * will never be called.
     */
    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        tasserted(8687400, "distinct command should have not invoked this method");
        return true;
    }

    void runDistinctAsAgg(OperationContext* opCtx,
                          std::unique_ptr<CanonicalQuery> canonicalQuery,
                          boost::optional<ExplainOptions::Verbosity> verbosity,
                          rpc::ReplyBuilderInterface* replyBuilder) const {
        const auto& nss = canonicalQuery->nss();
        const auto& dbName = nss.dbName();
        const auto& vts = auth::ValidatedTenancyScope::get(opCtx);
        const auto serializationContext = vts != boost::none
            ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
            : SerializationContext::stateCommandRequest();

        auto distinctAggRequest = parsed_distinct_command::asAggregation(
            *canonicalQuery, verbosity, serializationContext);

        distinctAggRequest.setQuerySettings(canonicalQuery->getExpCtx()->getQuerySettings());

        auto curOp = CurOp::get(opCtx);

        // We must store the key in distinct to prevent collecting query stats when the aggregation
        // runs.
        auto ownedQueryStatsKey = std::move(curOp->debug().queryStatsInfo.key);
        curOp->debug().queryStatsInfo.disableForSubqueryExecution = true;

        // If running explain distinct as agg, then aggregate is executed without privilege checks
        // and without response formatting.
        if (verbosity) {
            uassertStatusOK(runAggregate(opCtx,
                                         distinctAggRequest,
                                         {distinctAggRequest},
                                         distinctAggRequest.toBSON(),
                                         PrivilegeVector(),
                                         verbosity,
                                         replyBuilder));
            return;
        }

        const auto privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(AuthorizationSession::get(opCtx->getClient()),
                                            distinctAggRequest.getNamespace(),
                                            distinctAggRequest,
                                            false /* isMongos */));
        uassertStatusOK(runAggregate(opCtx,
                                     distinctAggRequest,
                                     {distinctAggRequest},
                                     distinctAggRequest.toBSON(),
                                     privileges,
                                     verbosity,
                                     replyBuilder));

        // Copy the result from the aggregate command.
        auto resultBuilder = replyBuilder->getBodyBuilder();
        CommandHelpers::extractOrAppendOk(resultBuilder);
        ViewResponseFormatter responseFormatter(resultBuilder.asTempObj().copy());

        // Reset the builder state, as the response will be written to the same builder.
        resultBuilder.resetToEmpty();

        // Include queryStats metrics in the result to be sent to mongos. While most views for
        // distinct on mongos will run through an aggregate pipeline on mongos, views on collections
        // that can be read completely locally, such as non-existent database collections or
        // unsplittable collections, will run through this distinct path on mongod and return
        // metrics back to mongos.
        const bool includeMetrics = curOp->debug().queryStatsInfo.metricsRequested;
        boost::optional<BSONObj> metrics = includeMetrics
            ? boost::make_optional(curOp->debug().getCursorMetrics().toBSON())
            : boost::none;
        uassertStatusOK(
            responseFormatter.appendAsDistinctResponse(&resultBuilder, dbName.tenantId(), metrics));

        curOp->setEndOfOpMetrics(resultBuilder.asTempObj().getObjectField("values").nFields());
        collectQueryStatsMongod(opCtx, canonicalQuery->getExpCtx(), std::move(ownedQueryStatsKey));
    }

    void appendMirrorableRequest(BSONObjBuilder* bob, const BSONObj& cmdObj) const override {
        static const auto kMirrorableKeys = [] {
            BSONObjBuilder keyBob;
            keyBob.append("distinct", 1);
            keyBob.append("key", 1);
            keyBob.append("query", 1);
            keyBob.append("hint", 1);
            keyBob.append("collation", 1);
            keyBob.append("rawData", 1);
            keyBob.append("shardVersion", 1);
            keyBob.append("databaseVersion", 1);
            return keyBob.obj();
        }();

        // Filter the keys that can be mirrored
        cmdObj.filterFieldsUndotted(bob, kMirrorableKeys, true);
    }
};
MONGO_REGISTER_COMMAND(DistinctCommand).forShard();

}  // namespace
}  // namespace mongo
