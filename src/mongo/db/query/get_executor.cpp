// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/get_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/classic/batched_delete_stage.h"
#include "mongo/db/exec/classic/count.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/eof.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/projection.h"
#include "mongo/db/exec/classic/record_store_fast_count.h"
#include "mongo/db/exec/classic/sort_key_generator.h"
#include "mongo/db/exec/classic/spool.h"
#include "mongo/db/exec/classic/timeseries_modify.h"
#include "mongo/db/exec/classic/timeseries_upsert.h"
#include "mongo/db/exec/classic/update_stage.h"
#include "mongo/db/exec/classic/upsert_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/express/plan_executor_express.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/physical_model/query_solution/eof_node_type.h"
#include "mongo/db/query/distinct_access.h"
#include "mongo/db/query/engine_selection.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/get_executor_deferred_engine_choice.h"
#include "mongo/db/query/get_executor_fast_paths.h"
#include "mongo/db/query/get_executor_helpers.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/replanning_required_info.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <variant>

#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<ExpressionContext> makeExpressionContextForGetExecutor(
    OperationContext* opCtx,
    const BSONObj& requestCollation,
    const NamespaceString& nss,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    tassert(11321300, "opCtx must not be null", opCtx);
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .ns(nss)
                      .mayDbProfile(true)
                      .explain(verbosity)
                      .build();
    if (!requestCollation.isEmpty()) {
        auto statusWithCollator =
            CollatorFactoryInterface::get(expCtx->getOperationContext()->getServiceContext())
                ->makeFromBSON(requestCollation);
        expCtx->setCollator(uassertStatusOK(std::move(statusWithCollator)));
    }
    return expCtx;
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    std::size_t plannerOptions,
    Pipeline* pipeline,
    bool needsMerge,
    boost::optional<TraversalPreference> traversalPreference) {
    tassert(11321303, "canonicalQuery must not be null", canonicalQuery);

    // Ensure that the shard filter option is set if this is a shard.
    if (collections.getMainCollectionAcquisition().getShardingDescription().isSharded()) {
        plannerOptions |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    }

    // Helper function for creating query planner parameters, with and without query settings. This
    // will be later used to ensure that queries can safely retry the planning process if the
    // application of the settings lead to a failure in generating the plan.
    auto makeQueryPlannerParams =
        [&opCtx, &collections, traversalPreference = std::move(traversalPreference)](
            const CanonicalQuery& cq,
            size_t options,
            boost::optional<QueryPlannerParams::ReplanningData> replanningData)
        -> std::unique_ptr<QueryPlannerParams> {
        auto result =
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForSingleCollectionQuery{
                .opCtx = opCtx,
                .canonicalQuery = cq,
                .collections = collections,
                .plannerOptions = options,
                .traversalPreference = traversalPreference,
                .planRanker = cq.getExpCtx()->getQueryKnobConfiguration().getPlanRanker(),
            });
        result->replanningData = std::move(replanningData);
        return result;
    };

    ON_BLOCK_EXIT([&] {
        // Stop the query planning timer once we have an execution plan.
        CurOp::get(opCtx)->stopQueryPlanningTimer();
    });

    auto expressResult =
        tryExpress(opCtx, collections, canonicalQuery, plannerOptions, makeQueryPlannerParams);
    if (expressResult.executor) {
        return std::move(expressResult.executor);
    }
    // If no express executor was returned, we can reuse the planner params created by `tryExpress`
    // for other planning logic.
    tassert(11974306, "Expected planner params to be initialized.", expressResult.plannerParams);

    markShedEligibleIfFindCommand(opCtx);

    // Initialize path arrayness in ExpressionContext from the CollectionQueryInfo.
    // Do not invoke if it has been already initialized.
    // TODO SERVER-123955 Consider moving it to a different location
    const auto& collection = collections.getMainCollection();
    const auto& nss = canonicalQuery->getExpCtx()->getNamespaceString();
    if (feature_flags::gFeatureFlagPathArrayness.isEnabled() &&
        !canonicalQuery->getExpCtx()->hasPathArraynessForNss(nss) && collection) {
        canonicalQuery->getExpCtx()->setPathArraynessForNss(
            nss, CollectionQueryInfo::get(collection).getPathArrayness());
    }

    if (feature_flags::gFeatureFlagQueryTypeInference.checkEnabled() &&
        !canonicalQuery->getExpCtx()->hasSchemaTypeInfoForNss(nss) && collection) {
        canonicalQuery->getExpCtx()->setSchemaTypeInfoForNss(
            nss, CollectionQueryInfo::get(collection).getSchemaTypeInfo());
    }

    // TODO SERVER-137028 Simplify this function after legacy executor is fully deleted.
    return exec_deferred_engine_choice::getExecutorFindDeferredEngineChoice(
        opCtx,
        collections,
        std::move(canonicalQuery),
        std::move(yieldPolicy),
        std::move(expressResult.plannerParams),
        makeQueryPlannerParams,
        plannerOptions,
        pipeline);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getSearchMetadataExecutorSBE(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ExpressionContext& expCtx,
    std::unique_ptr<executor::TaskExecutorCursor> metadataCursor) {
    // For metadata executor, we always have only one remote cursor, any id will work.
    const size_t metadataCursorId = 0;
    auto remoteCursors = std::make_unique<RemoteCursorMap>();
    remoteCursors->insert({metadataCursorId, std::move(metadataCursor)});

    MultipleCollectionAccessor emptyMca;
    auto sbeYieldPolicy =
        PlanYieldPolicySBE::make(opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, emptyMca, nss);
    auto root = stage_builder::buildSearchMetadataExecutorSBE(
        opCtx, expCtx, metadataCursorId, remoteCursors.get(), sbeYieldPolicy.get());
    return plan_executor_factory::make(opCtx,
                                       /*cq=*/nullptr,
                                       /*solution=*/nullptr,
                                       std::move(root),
                                       emptyMca,
                                       /*plannerOptions=*/{},
                                       nss,
                                       std::move(sbeYieldPolicy),
                                       /*isFromPlanCache=*/false,
                                       /*cachedPlanHash=*/boost::none,
                                       /*usedJoinOpt=*/false,
                                       /*rejectedJoinPlans=*/{},
                                       std::move(remoteCursors));
}

namespace {

/**
 * Attempts to construct and return the projection AST corresponding to 'projObj'. Illegal to call
 * if 'projObj' is empty.
 *
 * If 'allowPositional' is false, and the projection AST involves positional projection, returns a
 * non-OK status.
 *
 * Marks any metadata dependencies required by the projection on the given CanonicalQuery.
 */
StatusWith<std::unique_ptr<projection_ast::Projection>> makeProjection(const BSONObj& projObj,
                                                                       bool allowPositional,
                                                                       CanonicalQuery* cq) {
    tassert(11321304, "projObj must not be empty", !projObj.isEmpty());

    projection_ast::Projection proj =
        projection_ast::parseAndAnalyze(cq->getExpCtx(),
                                        projObj.getOwned(),
                                        cq->getPrimaryMatchExpression(),
                                        cq->getQueryObj(),
                                        ProjectionPolicies::findProjectionPolicies());

    // ProjectionExec requires the MatchDetails from the query expression when the projection
    // uses the positional operator. Since the query may no longer match the newly-updated
    // document, we forbid this case.
    if (!allowPositional && proj.requiresMatchDetails()) {
        return {ErrorCodes::BadValue,
                "cannot use a positional projection and return the new document"};
    }

    cq->requestAdditionalMetadata(proj.metadataDeps());

    // $meta sortKey is not allowed to be projected in findAndModify commands.
    if (cq->metadataDeps()[DocumentMetadataFields::kSortKey]) {
        return {ErrorCodes::BadValue,
                "Cannot use a $meta sortKey projection in findAndModify commands."};
    }

    return std::make_unique<projection_ast::Projection>(proj);
}

}  // namespace


namespace {
/**
 * Returns a MakePlannerParamsFn for a query on the single collection in 'collections'.
 */
auto makeQueryPlannerParamsFactory(OperationContext* opCtx,
                                   MultipleCollectionAccessor& collections) {
    return [opCtx, &collections](const CanonicalQuery& cq,
                                 size_t options,
                                 boost::optional<QueryPlannerParams::ReplanningData> replanningData)
               -> std::unique_ptr<QueryPlannerParams> {
        auto result =
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForSingleCollectionQuery{
                .opCtx = opCtx,
                .canonicalQuery = cq,
                .collections = collections,
                .plannerOptions = options,
                .planRanker = cq.getExpCtx()->getQueryKnobConfiguration().getPlanRanker(),
            });
        result->replanningData = std::move(replanningData);
        return result;
    };
}

/**
 * Builds the stages that perform the delete on top of 'root', the root of the winning plan.
 */
std::unique_ptr<PlanStage> addDeleteStage(std::unique_ptr<PlanStage> root,
                                          WorkingSet* ws,
                                          const CollectionAcquisition& coll,
                                          CanonicalDelete& canonicalDelete,
                                          const projection_ast::Projection* projection,
                                          DeleteStageParams deleteStageParams) {
    const auto& expCtx = deleteStageParams.canonicalQuery->getExpCtx();
    OperationContext* opCtx = expCtx->getOperationContext();
    const auto& collectionPtr = coll.getCollectionPtr();

    // TODO (SERVER-64506): support change streams' pre- and post-images.
    // TODO (SERVER-66079): allow batched deletions in the config.* namespace.
    const bool batchDelete = gBatchUserMultiDeletes.load() &&
        (shard_role_details::getRecoveryUnit(opCtx)->getState() == RecoveryUnit::State::kInactive ||
         shard_role_details::getRecoveryUnit(opCtx)->getState() ==
             RecoveryUnit::State::kActiveNotInUnitOfWork) &&
        !opCtx->inMultiDocumentTransaction() && !opCtx->isRetryableWrite() &&
        !collectionPtr->isChangeStreamPreAndPostImagesEnabled() &&
        !collectionPtr->ns().isConfigDB() && deleteStageParams.isMulti &&
        !deleteStageParams.fromMigrate && !deleteStageParams.returnDeleted &&
        deleteStageParams.sort.isEmpty() && !deleteStageParams.numStatsForDoc;

    if (canonicalDelete.isEligibleForArbitraryTimeseriesDelete()) {
        // Checks if the delete is on a time-series collection and cannot run on bucket
        // documents directly.
        root = std::make_unique<TimeseriesModifyStage>(
            expCtx.get(),
            TimeseriesModifyParams(&deleteStageParams),
            ws,
            std::move(root),
            coll,
            timeseries::BucketUnpacker(*collectionPtr->getTimeseriesOptions()),
            canonicalDelete.releaseResidualExpr());
    } else if (batchDelete) {
        root = std::make_unique<BatchedDeleteStage>(expCtx.get(),
                                                    std::move(deleteStageParams),
                                                    std::make_unique<BatchedDeleteStageParams>(),
                                                    ws,
                                                    coll,
                                                    root.release());
    } else {
        root = std::make_unique<DeleteStage>(
            expCtx.get(), std::move(deleteStageParams), ws, coll, root.release());
    }

    if (projection) {
        root = std::make_unique<ProjectionStageDefault>(
            expCtx, canonicalDelete.getRequest()->getProj(), projection, ws, std::move(root));
    }
    return root;
}

/**
 * Builds the stages that perform the update on top of 'root', the root of the winning plan.
 */
std::unique_ptr<PlanStage> addUpdateStage(std::unique_ptr<PlanStage> root,
                                          WorkingSet* ws,
                                          const CollectionAcquisition& coll,
                                          CanonicalUpdate& canonicalUpdate,
                                          const projection_ast::Projection* projection,
                                          UpdateStageParams updateStageParams) {
    const auto& expCtx = updateStageParams.canonicalQuery->getExpCtx();
    const auto& request = canonicalUpdate.getRequest();
    const bool isUpsert = updateStageParams.request->isUpsert();
    if (canonicalUpdate.isEligibleForArbitraryTimeseriesUpdate()) {
        const auto timeseriesOptions = coll.getCollectionPtr()->getTimeseriesOptions();
        if (request->isMulti()) {
            // If this is a multi-update, we need to spool the data before beginning to apply
            // updates, in order to avoid the Halloween problem.
            root = std::make_unique<SpoolStage>(expCtx.get(), ws, std::move(root));
        }
        if (isUpsert) {
            root = std::make_unique<TimeseriesUpsertStage>(
                expCtx.get(),
                TimeseriesModifyParams(&updateStageParams),
                ws,
                std::move(root),
                coll,
                timeseries::BucketUnpacker(*timeseriesOptions),
                canonicalUpdate.releaseResidualExpr(),
                canonicalUpdate.releaseOriginalExpr(),
                *request);
        } else {
            root = std::make_unique<TimeseriesModifyStage>(
                expCtx.get(),
                TimeseriesModifyParams(&updateStageParams),
                ws,
                std::move(root),
                coll,
                timeseries::BucketUnpacker(*timeseriesOptions),
                canonicalUpdate.releaseResidualExpr(),
                canonicalUpdate.releaseOriginalExpr());
        }
    } else if (isUpsert) {
        root = std::make_unique<UpsertStage>(
            expCtx.get(), updateStageParams, ws, coll, root.release());
    } else {
        root = std::make_unique<UpdateStage>(
            expCtx.get(), updateStageParams, ws, coll, root.release());
    }

    if (projection) {
        root = std::make_unique<ProjectionStageDefault>(
            expCtx, request->getProj(), projection, ws, std::move(root));
    }
    return root;
}

}  // namespace

//
// Delete
//

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDelete(
    OpDebug* opDebug,
    CollectionAcquisition coll,
    CanonicalDelete& canonicalDelete,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    const auto& collectionPtr = coll.getCollectionPtr();

    auto expCtx = canonicalDelete.expCtx();
    OperationContext* opCtx = expCtx->getOperationContext();
    const DeleteRequest* request = canonicalDelete.getRequest();

    const NamespaceString& nss(request->getNsString());

    if (collectionPtr && collectionPtr->isCapped()) {
        expCtx->setIsCappedDelete();
    }

    if (collectionPtr && collectionPtr->isCapped() && opCtx->inMultiDocumentTransaction()) {
        // This check is duplicated from collection_internal::deleteDocument() for two reasons:
        // - Performing a remove on an empty capped collection would not call
        //   collection_internal::deleteDocument().
        // - We can avoid doing lookups on documents and erroring later when trying to delete them.
        return Status(
            ErrorCodes::IllegalOperation,
            str::stream()
                << "Cannot remove from a capped collection in a multi-document transaction: "
                << nss.toStringForErrorMsg());
    }

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream()
                          << "Not primary while removing from " << nss.toStringForErrorMsg());
    }

    ON_BLOCK_EXIT([&] {
        // Stop the query planning timer once we have an execution plan.
        CurOp::get(opCtx)->stopQueryPlanningTimer();
    });

    if (!collectionPtr) {
        // Treat collections that do not exist as empty collections. Return a PlanExecutor which
        // contains an EOF stage.
        LOGV2_DEBUG(20927,
                    2,
                    "Collection does not exist. Using EOF stage",
                    logAttrs(nss),
                    "query"_attr = redact(request->getQuery()));

        return plan_executor_factory::make(
            expCtx,
            std::make_unique<WorkingSet>(),
            std::make_unique<EOFStage>(expCtx.get(), eof_node::EOFType::NonExistentNamespace),
            coll,
            canonicalDelete.yieldPolicy(),
            false, /* whether we must return owned data */
            nss);
    }

    if (!canonicalDelete.hasParsedQuery()) {

        // Only consider using the idhack if no hint was provided and this is not a timeseries
        // request.
        // TODO SERVER-87213 Allow hinted queries to go through idhack.
        if (!canonicalDelete.isRequestToTimeseries() && request->getHint().isEmpty()) {
            // This is the idhack fast-path for getting a PlanExecutor without doing the work to
            // create a CanonicalQuery.
            const BSONObj& unparsedQuery = request->getQuery();

            bool hasIdIndex = collectionPtr->getIndexCatalog()->findIdIndex(opCtx) ||
                clustered_util::isClusteredOnId(collectionPtr->getClusteredInfo());

            // Construct delete request collator.
            std::unique_ptr<CollatorInterface> collator;
            if (!request->getCollation().isEmpty()) {
                auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(request->getCollation());
                if (!statusWithCollator.isOK()) {
                    return statusWithCollator.getStatus();
                }
                collator = std::move(statusWithCollator.getValue());
            }
            const bool hasCollectionDefaultCollation = request->getCollation().isEmpty() ||
                CollatorInterface::collatorsMatch(collator.get(),
                                                  collectionPtr->getDefaultCollator());

            if (hasIdIndex && isSimpleIdQuery(unparsedQuery) && request->getProj().isEmpty() &&
                hasCollectionDefaultCollation) {

                LOGV2_DEBUG(8376000, 2, "Using express", "query"_attr = redact(unparsedQuery));

                return makeExpressExecutorForDelete(opCtx, coll, canonicalDelete);
            }
        }

        // If we're here then we don't have a parsed query, but we're also not eligible for
        // the idhack fast path. We need to force canonicalization now.
        Status cqStatus = canonicalDelete.parseQueryToCQ();
        if (!cqStatus.isOK()) {
            return cqStatus;
        }
    }

    // This is the regular path for when we have a CanonicalQuery.
    std::unique_ptr<CanonicalQuery> cq(canonicalDelete.releaseParsedQuery());

    const auto policy = canonicalDelete.yieldPolicy();

    DeleteStageParams deleteStageParams;
    deleteStageParams.isMulti = request->getMulti();
    deleteStageParams.fromMigrate = request->getFromMigrate();
    deleteStageParams.isExplain = request->getIsExplain();
    deleteStageParams.returnDeleted = request->getReturnDeleted();
    deleteStageParams.sort = request->getSort();
    deleteStageParams.opDebug = opDebug;
    deleteStageParams.stmtId = request->getStmtId();

    if (canonicalDelete.isRequestToTimeseries() &&
        !canonicalDelete.isEligibleForArbitraryTimeseriesDelete()) {
        deleteStageParams.numStatsForDoc = timeseries::numMeasurementsForBucketCounter(
            collectionPtr->getTimeseriesOptions()->getTimeField());
    }

    // Transfer the explain verbosity level into the expression context.
    cq->getExpCtx()->setExplain(verbosity);

    std::unique_ptr<projection_ast::Projection> projection;
    if (!request->getProj().isEmpty()) {
        tassert(11321305,
                "returnDeleted must be true if the projection is not empty",
                request->getReturnDeleted());

        const bool allowPositional = true;
        auto projectionWithStatus = makeProjection(request->getProj(), allowPositional, cq.get());
        if (!projectionWithStatus.isOK()) {
            return projectionWithStatus.getStatus();
        }
        projection = std::move(projectionWithStatus.getValue());
    }

    // The underlying query plan must preserve the record id, since it will be needed in order to
    // identify the record to update.
    cq->setForceGenerateRecordId(true);
    deleteStageParams.canonicalQuery = cq.get();

    MultipleCollectionAccessor collections{coll};
    bool rootStageAdded = false;
    return exec_deferred_engine_choice::getExecutorWithClassicRootStage(
        opCtx,
        collections,
        std::move(cq),
        policy,
        makeQueryPlannerParamsFactory(opCtx, collections),
        QueryPlannerParams::DEFAULT,
        [&](std::unique_ptr<PlanStage> root, WorkingSet* ws) {
            tassert(13847302, "Expected the delete stage to be added only once", !rootStageAdded);
            rootStageAdded = true;
            return addDeleteStage(std::move(root),
                                  ws,
                                  coll,
                                  canonicalDelete,
                                  projection.get(),
                                  std::move(deleteStageParams));
        });
}

//
// Update
//

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorUpdate(
    OpDebug* opDebug,
    CollectionAcquisition coll,
    CanonicalUpdate& canonicalUpdate,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    const auto& collectionPtr = coll.getCollectionPtr();

    auto expCtx = canonicalUpdate.expCtx();
    OperationContext* opCtx = expCtx->getOperationContext();

    const UpdateRequest* request = canonicalUpdate.getRequest();
    UpdateDriver* driver = canonicalUpdate.getDriver();

    const NamespaceString& nss = request->getNamespaceString();

    // If there is no collection and this is an upsert, callers are supposed to create
    // the collection prior to calling this method. Explain, however, will never do
    // collection or database creation.
    tassert(11321306,
            "Expected 'request' to be an explain if the underlying op is an upsert over a "
            "non-existent collection",
            !(!coll.exists() && request->isUpsert()) || request->explain().has_value());

    // If this is a user-issued update, then we want to return an error: you cannot perform
    // writes on a secondary. If this is an update to a secondary from the replication system,
    // however, then we make an exception and let the write proceed.
    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream() << "Not primary while performing update on "
                                    << nss.toStringForErrorMsg());
    }

    const auto policy = canonicalUpdate.yieldPolicy();

    auto documentCounter = [&] {
        if (canonicalUpdate.isRequestToTimeseries() &&
            !canonicalUpdate.isEligibleForArbitraryTimeseriesUpdate()) {
            return timeseries::numMeasurementsForBucketCounter(
                collectionPtr->getTimeseriesOptions()->getTimeField());
        }
        return UpdateStageParams::DocumentCounter{};
    }();

    ON_BLOCK_EXIT([&] {
        // Stop the query planning timer once we have an execution plan.
        CurOp::get(opCtx)->stopQueryPlanningTimer();
    });

    // If the collection doesn't exist, then return a PlanExecutor for a no-op EOF plan. We have
    // should have already enforced upstream that in this case either the upsert flag is false, or
    // we are an explain. If the collection doesn't exist, we're not an explain, and the upsert flag
    // is true, we expect the caller to have created the collection already.
    if (!coll.exists()) {
        LOGV2_DEBUG(20929,
                    2,
                    "Collection does not exist. Using EOF stage",
                    logAttrs(nss),
                    "query"_attr = redact(request->getQuery()));

        return plan_executor_factory::make(
            expCtx,
            std::make_unique<WorkingSet>(),
            std::make_unique<EOFStage>(expCtx.get(), eof_node::EOFType::NonExistentNamespace),
            boost::none,
            policy,
            false, /* whether owned BSON must be returned */
            nss);
    }

    if (!canonicalUpdate.hasParsedQuery()) {
        // Only consider using the idhack if no hint was provided.
        if (!canonicalUpdate.isEligibleForArbitraryTimeseriesUpdate() &&
            request->getHint().isEmpty()) {
            // This is the idhack fast-path for getting a PlanExecutor without doing the work
            // to create a CanonicalQuery.
            const BSONObj& unparsedQuery = request->getQuery();

            const bool hasCollectionDefaultCollation = CollatorInterface::collatorsMatch(
                expCtx->getCollator(), collectionPtr->getDefaultCollator());

            if (isSimpleIdQuery(unparsedQuery) && request->getProj().isEmpty() &&
                hasCollectionDefaultCollation) {

                const auto idIndexDesc = collectionPtr->getIndexCatalog()->findIdIndex(opCtx);
                if (!request->isUpsert() &&
                    (idIndexDesc ||
                     clustered_util::isClusteredOnId(collectionPtr->getClusteredInfo()))) {
                    // Upserts not supported in express for now.
                    LOGV2_DEBUG(83759, 2, "Using Express", "query"_attr = redact(unparsedQuery));

                    return makeExpressExecutorForUpdate(
                        opCtx, coll, canonicalUpdate, false /* return owned BSON */);

                } else if (idIndexDesc) {
                    LOGV2_DEBUG(20930, 2, "Using idhack", "query"_attr = redact(unparsedQuery));
                    UpdateStageParams updateStageParams(
                        request, driver, opDebug, std::move(documentCounter));
                    BSONObj queryFilter = request->isUpsert()
                        ? getQueryFilterMaybeUnwrapEq(unparsedQuery)
                        : unparsedQuery;
                    fastPathQueryCounters.incrementIdHackQueryCounter();
                    return InternalPlanner::updateWithIdHack(
                        opCtx, coll, updateStageParams, idIndexDesc, queryFilter, policy);
                }
            }
        }

        // If we're here then we don't have a parsed query, but we're also not eligible for
        // the idhack fast path. We need to force canonicalization now.
        Status cqStatus = canonicalUpdate.parseQueryToCQ();
        if (!cqStatus.isOK()) {
            return cqStatus;
        }
    }

    // This is the regular path for when we have a CanonicalQuery.
    UpdateStageParams updateStageParams(request, driver, opDebug, std::move(documentCounter));
    std::unique_ptr<CanonicalQuery> cq(canonicalUpdate.releaseParsedQuery());

    std::unique_ptr<projection_ast::Projection> projection;
    if (!request->getProj().isEmpty()) {
        tassert(11321307,
                "Expected 'shouldReturnAnyDocs' to be true if a projection is present",
                request->shouldReturnAnyDocs());

        // If the plan stage is to return the newly-updated version of the documents, then it
        // is invalid to use a positional projection because the query expression need not
        // match the array element after the update has been applied.
        const bool allowPositional = request->shouldReturnOldDocs();
        auto projectionWithStatus = makeProjection(request->getProj(), allowPositional, cq.get());
        if (!projectionWithStatus.isOK()) {
            return projectionWithStatus.getStatus();
        }
        projection = std::move(projectionWithStatus.getValue());
    }

    // The underlying query plan must preserve the record id, since it will be needed in order to
    // identify the record to update.
    cq->setForceGenerateRecordId(true);
    updateStageParams.canonicalQuery = cq.get();

    MultipleCollectionAccessor collections{coll};
    bool rootStageAdded = false;
    return exec_deferred_engine_choice::getExecutorWithClassicRootStage(
        opCtx,
        collections,
        std::move(cq),
        policy,
        makeQueryPlannerParamsFactory(opCtx, collections),
        QueryPlannerParams::DEFAULT,
        [&](std::unique_ptr<PlanStage> root, WorkingSet* ws) {
            tassert(13847303, "Expected the update stage to be added only once", !rootStageAdded);
            rootStageAdded = true;
            return addUpdateStage(std::move(root),
                                  ws,
                                  coll,
                                  canonicalUpdate,
                                  projection.get(),
                                  std::move(updateStageParams));
        });
}

//
// Count hack
//

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorCount(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const CollectionAcquisition& coll,
    std::unique_ptr<ParsedFindCommand> parsedFind,
    const CountCommandRequest& count) {
    OperationContext* opCtx = expCtx->getOperationContext();
    auto statusWithCQ = CanonicalQuery::make(
        {.expCtx = expCtx, .parsedFind = std::move(parsedFind), .isCountLike = true});
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }
    auto cq = std::move(statusWithCQ.getValue());

    const auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;

    const auto skip = count.getSkip().value_or(0);
    const auto limit = count.getLimit().value_or(0);

    ON_BLOCK_EXIT([&] {
        // Stop the query planning timer once we have an execution plan.
        CurOp::get(opCtx)->stopQueryPlanningTimer();
    });

    if (!coll.exists()) {
        std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
        // Treat collections that do not exist as empty collections. Note that the explain reporting
        // machinery always assumes that the root stage for a count operation is a CountStage, so in
        // this case we put a CountStage on top of an EOFStage.
        std::unique_ptr<PlanStage> root = std::make_unique<CountStage>(
            expCtx.get(),
            limit,
            skip,
            ws.get(),
            new EOFStage(expCtx.get(), eof_node::EOFType::NonExistentNamespace));

        return plan_executor_factory::make(expCtx,
                                           std::move(ws),
                                           std::move(root),
                                           boost::none,
                                           yieldPolicy,
                                           false, /* whether we must return owned BSON */
                                           cq->getFindCommandRequest().getNamespaceOrUUID().nss());
    }

    // Can't encode plan cache key for non-existent collections. Add plan cache key information to
    // curOp here so both FastCountStage and multi-planner codepaths properly populate it.
    auto planCache = plan_cache_key_factory::make<PlanCacheKey>(*cq, coll);
    setOpDebugPlanCacheInfo(opCtx,
                            PlanCacheInfo{.planCacheKey = planCache.planCacheKeyHash(),
                                          .planCacheShapeHash = planCache.planCacheShapeHash()});

    // If the query is empty, then we can determine the count by just asking the collection
    // for its number of records. This is implemented by the CountStage, and we don't need
    // to create a child for the count stage in this case.
    //
    // If there is a hint, then we can't use a trival count plan as described above.
    const bool isEmptyQueryPredicate =
        cq->getPrimaryMatchExpression()->matchType() == MatchExpression::AND &&
        cq->getPrimaryMatchExpression()->numChildren() == 0;
    const bool useRecordStoreCount =
        isEmptyQueryPredicate && cq->getFindCommandRequest().getHint().isEmpty();

    if (useRecordStoreCount) {
        std::unique_ptr<PlanStage> root =
            std::make_unique<RecordStoreFastCountStage>(expCtx.get(), coll, skip, limit);

        return plan_executor_factory::make(expCtx,
                                           std::make_unique<WorkingSet>(),
                                           std::move(root),
                                           boost::none,
                                           yieldPolicy,
                                           false, /* whether we must returned owned BSON */
                                           cq->getFindCommandRequest().getNamespaceOrUUID().nss());
    }

    size_t plannerOptions = QueryPlannerParams::DEFAULT;
    if (coll.getShardingDescription().isSharded()) {
        plannerOptions |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    }

    MultipleCollectionAccessor collections{coll};
    bool rootStageAdded = false;
    return exec_deferred_engine_choice::getExecutorWithClassicRootStage(
        opCtx,
        collections,
        std::move(cq),
        yieldPolicy,
        makeQueryPlannerParamsFactory(opCtx, collections),
        plannerOptions,
        [&](std::unique_ptr<PlanStage> root, WorkingSet* ws) -> std::unique_ptr<PlanStage> {
            tassert(13847304, "Expected the count stage to be added only once", !rootStageAdded);
            rootStageAdded = true;
            return std::make_unique<CountStage>(expCtx.get(), limit, skip, ws, root.release());
        });
}

StatusWith<std::unique_ptr<QuerySolution>> tryGetQuerySolutionForDistinct(
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    const CanonicalQuery& canonicalQuery,
    bool flipDistinctScanDirection) {
    tassert(9245500, "Expected distinct property on CanonicalQuery", canonicalQuery.getDistinct());

    const auto& collectionPtr = collections.getMainCollection();
    if (!collectionPtr) {
        // The caller should create EOF plan for the appropriate engine.
        return {ErrorCodes::NoQueryExecutionPlans, "No viable DISTINCT_SCAN plan"};
    }

    auto* opCtx = canonicalQuery.getExpCtx()->getOperationContext();

    auto getQuerySolution = [&](size_t options) -> std::unique_ptr<QuerySolution> {
        auto plannerParams =
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForDistinct{
                opCtx,
                canonicalQuery,
                collections,
                options,
                flipDistinctScanDirection,
            });

        // Can't create a DISTINCT_SCAN stage if no suitable indexes are present.
        if (plannerParams->mainCollectionInfo.indexes.empty()) {
            return nullptr;
        }
        return createDistinctScanSolution(
            canonicalQuery, *plannerParams, flipDistinctScanDirection);
    };
    auto soln = getQuerySolution(plannerOptions);
    if (!soln) {
        // Try again this time without query settings applied.
        soln = getQuerySolution(plannerOptions | QueryPlannerParams::IGNORE_QUERY_SETTINGS);
    }
    if (!soln) {
        return {ErrorCodes::NoQueryExecutionPlans, "No viable DISTINCT_SCAN plan"};
    }

    return {std::move(soln)};
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDistinct(
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::unique_ptr<QuerySolution> soln) {
    tassert(9245501, "Expected distinct property on CanonicalQuery", canonicalQuery->getDistinct());

    const auto& collectionPtr = collections.getMainCollection();
    auto* opCtx = canonicalQuery->getExpCtx()->getOperationContext();
    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    auto collPtrOrAcq = collections.getMainCollectionPtrOrAcquisition();
    auto&& root = stage_builder::buildClassicExecutableTree(
        opCtx, collPtrOrAcq, *canonicalQuery, *soln, ws.get());

    LOGV2_DEBUG(
        20932, 2, "Using fast distinct", "query"_attr = redact(canonicalQuery->toStringShort()));

    // Stop the query planning timer once we have an execution plan.
    CurOp::get(opCtx)->stopQueryPlanningTimer();

    return plan_executor_factory::make(std::move(canonicalQuery),
                                       std::move(ws),
                                       std::move(root),
                                       collPtrOrAcq,
                                       PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                       plannerOptions,
                                       collectionPtr->ns(),
                                       std::move(soln));
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getCollectionScanExecutor(
    OperationContext* opCtx,
    const CollectionAcquisition& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    CollectionScanDirection scanDirection,
    const boost::optional<RecordId>& resumeAfterRecordId,
    bool tolerateDeletedScanPosition) {
    auto isForward = scanDirection == CollectionScanDirection::kForward;
    auto direction = isForward ? InternalPlanner::FORWARD : InternalPlanner::BACKWARD;
    return InternalPlanner::collectionScan(
        {.opCtx = opCtx,
         .collection = collection,
         .yieldPolicy = yieldPolicy,
         .direction = direction,
         .resumeAfterRecordId = resumeAfterRecordId,
         .tolerateDeletedScanPosition = tolerateDeletedScanPosition});
}

}  // namespace mongo
