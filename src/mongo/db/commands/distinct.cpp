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


#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/serialization_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace {

CanonicalDistinct parseDistinctCmd(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const BSONObj& cmdObj,
                                   const ExtensionsCallback& extensionsCallback,
                                   const CollatorInterface* defaultCollator,
                                   boost::optional<ExplainOptions::Verbosity> verbosity) {
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    const auto serializationContext = vts != boost::none
        ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
        : SerializationContext::stateCommandRequest();

    auto distinctCommand = std::make_unique<DistinctCommandRequest>(
        DistinctCommandRequest::parse(IDLParserContext("distinctCommandRequest",
                                                       false /* apiStrict */,
                                                       vts,
                                                       nss.tenantId(),
                                                       serializationContext),
                                      cmdObj));

    // Forbid users from passing 'querySettings' explicitly.
    uassert(7923000,
            "BSON field 'querySettings' is an unknown field",
            query_settings::utils::allowQuerySettingsFromClient(opCtx->getClient()) ||
                !distinctCommand->getQuerySettings().has_value());

    auto expCtx = CanonicalDistinct::makeExpressionContext(
        opCtx, nss, *distinctCommand, defaultCollator, verbosity);

    auto parsedDistinct =
        parsed_distinct_command::parse(expCtx,
                                       cmdObj,
                                       std::move(distinctCommand),
                                       extensionsCallback,
                                       MatchExpressionParser::kAllowAllSpecialFeatures);

    // TODO: SERVER-73632 Remove feature flag for PM-635.
    // Query settings will only be looked up on mongos and therefore should be part of command body
    // on mongod if present.
    expCtx->setQuerySettings(
        query_settings::lookupQuerySettingsForDistinct(expCtx, *parsedDistinct, nss));
    return CanonicalDistinct::parse(std::move(expCtx), std::move(parsedDistinct));
}

namespace dps = dotted_path_support;

namespace {
// This function might create a classic or SBE plan executor. It relies on some assumptions that are
// specific to the distinct() command and shouldn't be blindly reused in other "distinct" contexts.
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> createExecutorForDistinctCommand(
    OperationContext* opCtx,
    CanonicalDistinct canonicalDistinct,
    const CollectionAcquisition& coll) {
    const auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;
    const auto& collectionPtr = coll.getCollectionPtr();
    const MultipleCollectionAccessor collections{coll};

    // If the collection doesn't exist 'getExecutor()' should create an EOF plan for it no matter
    // the query.
    if (!collectionPtr) {
        return uassertStatusOK(
            getExecutorFind(opCtx, collections, canonicalDistinct.releaseQuery(), yieldPolicy));
    }

    // Try creating a plan that does DISTINCT_SCAN.
    auto executor =
        tryGetExecutorDistinct(collections, QueryPlannerParams::DEFAULT, canonicalDistinct);
    if (executor.isOK()) {
        return std::move(executor.getValue());
    }

    // If there is no DISTINCT_SCAN plan, create whatever non-distinct plan is appropriate, because
    // 'distinct()' command is capable of de-duplicating and unwinding its inputs. Note: In order to
    // allow a covered DISTINCT_SCAN we've inserted a projection -- there is no point of keeping it
    // if a DISTINCT_SCAN didn't bake out.
    auto cq = canonicalDistinct.getQuery();
    auto findCommand = std::make_unique<FindCommandRequest>(cq->getFindCommandRequest());
    findCommand->setProjection(BSONObj());

    auto cqWithoutProjection = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = cq->getExpCtx(),
        .parsedFind =
            ParsedFindCommandParams{
                .findCommand = std::move(findCommand),
                .extensionsCallback = ExtensionsCallbackReal(opCtx, &collectionPtr->ns()),
                .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures},
    });

    return uassertStatusOK(
        getExecutorFind(opCtx, collections, std::move(cqWithoutProjection), yieldPolicy));
}
}  // namespace

class DistinctCommand : public BasicCommand {
public:
    DistinctCommand() : BasicCommand("distinct") {}

    std::string help() const override {
        return "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
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

        const auto resolvedNss =
            CollectionCatalog::get(opCtx)->resolveNamespaceStringFromDBNameAndUUID(
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
        const DatabaseName dbName = request.getDbName();
        const BSONObj& cmdObj = request.body;
        // Acquire locks. The RAII object is optional, because in the case of a view, the locks
        // need to be released.
        const auto nss = CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);

        AutoStatsTracker tracker(
            opCtx,
            nss,
            Top::LockType::ReadLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nss.dbName()));

        const auto acquisitionRequest = CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx, nss, AcquisitionPrerequisites::kRead);
        boost::optional<CollectionOrViewAcquisition> collectionOrView =
            acquireCollectionOrViewMaybeLockFree(opCtx, acquisitionRequest);
        const CollatorInterface* defaultCollator = collectionOrView->getCollectionPtr()
            ? collectionOrView->getCollectionPtr()->getDefaultCollator()
            : nullptr;

        auto canonicalDistinct = parseDistinctCmd(
            opCtx, nss, cmdObj, ExtensionsCallbackReal(opCtx, &nss), defaultCollator, verbosity);

        if (collectionOrView->isView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            collectionOrView.reset();
            runDistinctOnView(opCtx, canonicalDistinct, verbosity, replyBuilder);
            return Status::OK();
        }

        auto executor = createExecutorForDistinctCommand(
            opCtx, std::move(canonicalDistinct), collectionOrView->getCollection());
        SerializationContext serializationCtx = request.getSerializationContext();
        auto bodyBuilder = replyBuilder->getBodyBuilder();
        Explain::explainStages(executor.get(),
                               collectionOrView->getCollectionPtr(),
                               verbosity,
                               BSONObj(),
                               SerializationContext::stateCommandReply(serializationCtx),
                               cmdObj,
                               &bodyBuilder);
        return Status::OK();
    }

    bool runWithReplyBuilder(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmdObj,
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
                            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nss.dbName()));
        };
        auto const nssOrUUID = CommandHelpers::parseNsOrUUID(dbName, cmdObj);
        if (nssOrUUID.isNamespaceString()) {
            initializeTracker(nssOrUUID.nss());
        }
        const auto acquisitionRequest = CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx, nssOrUUID, AcquisitionPrerequisites::kRead);

        boost::optional<CollectionOrViewAcquisition> collectionOrView =
            acquireCollectionOrViewMaybeLockFree(opCtx, acquisitionRequest);
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

        auto canonicalDistinct = parseDistinctCmd(
            opCtx, nss, cmdObj, ExtensionsCallbackReal(opCtx, &nss), defaultCollation, {});

        if (canonicalDistinct.isMirrored()) {
            const auto& invocation = CommandInvocation::get(opCtx);
            invocation->markMirrored();
        } else if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                       opCtx,
                       nss,
                       analyze_shard_key::SampledCommandNameEnum::kDistinct,
                       canonicalDistinct)) {
            auto cq = canonicalDistinct.getQuery();
            analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                ->addDistinctQuery(
                    *sampleId, nss, cq->getQueryObj(), cq->getFindCommandRequest().getCollation())
                .getAsync([](auto) {});
        }

        if (collectionOrView->isView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            collectionOrView.reset();
            runDistinctOnView(opCtx, canonicalDistinct, boost::none /* verbosity */, replyBuilder);
            return true;
        }

        // Check whether we are allowed to read from this node after acquiring our locks.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassertStatusOK(replCoord->checkCanServeReadsFor(
            opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

        auto executor = createExecutorForDistinctCommand(
            opCtx, std::move(canonicalDistinct), collectionOrView->getCollection());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setPlanSummary_inlock(executor->getPlanExplainer().getPlanSummary());
        }

        const auto key = cmdObj.getStringField(CanonicalDistinct::kKeyField);

        std::vector<BSONObj> distinctValueHolder;
        BSONElementSet values(executor->getCanonicalQuery()->getCollator());

        const int kMaxResponseSize = BSONObjMaxUserSize - 4096;

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
                dps::extractAllElementsAlongPath(obj, key, elts);

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
                          "cmd"_attr = cmdObj);

            exception.addContext("Executor error during distinct command");
            throw;
        }

        auto curOp = CurOp::get(opCtx);
        const auto& collection = collectionOrView->getCollectionPtr();

        // Get summary information about the plan.
        PlanSummaryStats stats;
        auto&& explainer = executor->getPlanExplainer();
        explainer.getSummaryStats(&stats);
        if (collection) {
            CollectionQueryInfo::get(collection).notifyOfQuery(opCtx, collection, stats);
        }
        curOp->debug().setPlanSummaryMetrics(stats);

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
             BSONObjBuilder& result) {
        tasserted(8687400, "distinct command should have not invoked this method");
        return true;
    }

    void runDistinctOnView(OperationContext* opCtx,
                           const CanonicalDistinct& canonicalDistinct,
                           boost::optional<ExplainOptions::Verbosity> verbosity,
                           rpc::ReplyBuilderInterface* replyBuilder) const {
        const auto& cq = *canonicalDistinct.getQuery();
        const auto& nss = cq.nss();
        const auto& dbName = nss.dbName();
        const auto& vts = auth::ValidatedTenancyScope::get(opCtx);
        const auto viewAggCmd =
            OpMsgRequestBuilder::create(
                vts, dbName, uassertStatusOK(canonicalDistinct.asAggregationCommand()))
                .body;
        const auto serializationContext = vts != boost::none
            ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
            : SerializationContext::stateCommandRequest();
        auto viewAggRequest = aggregation_request_helper::parseFromBSON(
            opCtx,
            nss,
            viewAggCmd,
            verbosity,
            APIParameters::get(opCtx).getAPIStrict().value_or(false),
            serializationContext);
        viewAggRequest.setQuerySettings(cq.getExpCtx()->getQuerySettings());

        // If running explain distinct on view, then aggregate is executed without privilege checks
        // and without response formatting.
        if (verbosity) {
            uassertStatusOK(runAggregate(opCtx,
                                         viewAggRequest,
                                         {viewAggRequest},
                                         viewAggCmd,
                                         PrivilegeVector(),
                                         replyBuilder,
                                         {} /* usedExternalDataSources */));
            return;
        }

        const auto privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(AuthorizationSession::get(opCtx->getClient()),
                                            viewAggRequest.getNamespace(),
                                            viewAggRequest,
                                            false /* isMongos */));
        uassertStatusOK(runAggregate(opCtx,
                                     viewAggRequest,
                                     {viewAggRequest},
                                     viewAggCmd,
                                     privileges,
                                     replyBuilder,
                                     {} /* usedExternalDataSources */));

        // Copy the result from the aggregate command.
        auto resultBuilder = replyBuilder->getBodyBuilder();
        CommandHelpers::extractOrAppendOk(resultBuilder);
        ViewResponseFormatter responseFormatter(resultBuilder.asTempObj().copy());

        // Reset the builder state, as the response will be written to the same builder.
        resultBuilder.resetToEmpty();
        uassertStatusOK(
            responseFormatter.appendAsDistinctResponse(&resultBuilder, dbName.tenantId()));
    }

    void appendMirrorableRequest(BSONObjBuilder* bob, const BSONObj& cmdObj) const override {
        static const auto kMirrorableKeys = [] {
            BSONObjBuilder keyBob;
            keyBob.append("distinct", 1);
            keyBob.append("key", 1);
            keyBob.append("query", 1);
            keyBob.append("hint", 1);
            keyBob.append("collation", 1);
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
