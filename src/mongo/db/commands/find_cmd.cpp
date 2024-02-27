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
#include "mongo/db/commands/run_aggregate.h"
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
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
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
 * Check if the operation comes from the internal client. Returns 'true' if the client is
 * internal ('mongos'), and false otherwise.
 */
bool isInternalClient(const OperationContext* opCtx) {
    return opCtx->getClient()->session() && opCtx->getClient()->isInternalClient();
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

    // After parsing to detect if $$USER_ROLES is referenced in the query, set the value of
    // $$USER_ROLES for the find command.
    expCtx->setUserRoles();
    // Register query stats collection. Exclude queries against collections with encrypted fields.
    // It is important to do this before canonicalizing and optimizing the query, each of which
    // would alter the query shape.
    if (!(collection && collection.get()->getCollectionOptions().encryptedFieldConfig)) {
        query_stats::registerRequest(opCtx, nss, [&]() {
            return std::make_unique<query_stats::FindKey>(
                expCtx, *parsedRequest, collOrViewAcquisition.getCollectionType());
        });
    }

    // TODO: SERVER-73632 Remove feature flag for PM-635.
    // Query settings will only be looked up on mongos and therefore should be part of command body
    // on mongod if present.
    expCtx->setQuerySettings(
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

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        // TODO: Parse into a QueryRequest here.
        return std::make_unique<Invocation>(this, opMsgRequest);
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
        Invocation(const FindCmd* definition, const OpMsgRequest& request)
            : CommandInvocation(definition),
              _request(request),
              _dbName(request.getDbName()),
              _ns(CommandHelpers::parseNsFromCommand(_dbName, _request.body)) {
            invariant(_request.body.isOwned());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
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
                    authSession->isAuthorizedToParseNamespaceElement(_request.body.firstElement()));

            const auto hasTerm = _request.body.hasField(kTermField);
            const auto nsOrUUID = CommandHelpers::parseNsOrUUID(_dbName, _request.body);
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
            // Acquire locks. The RAII object is optional, because in the case of a view, the locks
            // need to be released.
            // TODO SERVER-79175: Make nicer. We need to instantiate the AutoStatsTracker before the
            // acquisition in case it would throw so we can ensure data is written to the profile
            // collection that some test may rely on.
            auto const nss = CommandHelpers::parseNsCollectionRequired(_dbName, _request.body);
            AutoStatsTracker tracker{
                opCtx,
                nss,
                Top::LockType::ReadLocked,
                AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nss.dbName())};

            const auto acquisitionRequest = CollectionOrViewAcquisitionRequest::fromOpCtx(
                opCtx, nss, AcquisitionPrerequisites::kRead);

            boost::optional<CollectionOrViewAcquisition> collectionOrView =
                acquireCollectionOrViewMaybeLockFree(opCtx, acquisitionRequest);

            // Going forward this operation must never ignore interrupt signals while waiting for
            // lock acquisition. This InterruptibleLockGuard will ensure that waiting for lock
            // re-acquisition after yielding will not ignore interrupt signals. This is necessary to
            // avoid deadlocking with replication rollback, which at the storage layer waits for all
            // cursors to be closed under the global MODE_X lock, after having sent interrupt
            // signals to read operations. This operation must never hold open storage cursors while
            // ignoring interrupt.
            InterruptibleLockGuard interruptibleLockAcquisition(
                shard_role_details::getLocker(opCtx));

            // Parse the command BSON to a FindCommandRequest.
            auto findCommand = _parseCmdObjectToFindCommandRequest(opCtx, nss, _request);
            auto respSc =
                SerializationContext::stateCommandReply(findCommand->getSerializationContext());

            // The collection may be NULL. If so, getExecutor() should handle it by returning an
            // execution tree with an EOFStage.
            const auto& collectionPtr = collectionOrView->getCollectionPtr();
            if (!collectionOrView->isView()) {
                const bool isClusteredCollection = collectionPtr && collectionPtr->isClustered();
                uassertStatusOK(query_request_helper::validateResumeAfter(
                    opCtx, findCommand->getResumeAfter(), isClusteredCollection));
            }
            auto expCtx = makeExpressionContext(opCtx, *findCommand, collectionPtr, verbosity);
            auto parsedRequest = uassertStatusOK(parsed_find_command::parse(
                expCtx,
                {.findCommand = std::move(findCommand),
                 .extensionsCallback = ExtensionsCallbackReal(opCtx, &nss),
                 .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

            expCtx->setQuerySettings(
                query_settings::lookupQuerySettingsForFind(expCtx, *parsedRequest, nss));
            auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
                .expCtx = std::move(expCtx), .parsedFind = std::move(parsedRequest)});

            // After parsing to detect if $$USER_ROLES is referenced in the query, set the value of
            // $$USER_ROLES for the find command.
            cq->getExpCtx()->setUserRoles();

            // If we are running a query against a view redirect this query through the aggregation
            // system.
            if (collectionOrView->isView()) {
                // Relinquish locks. The aggregation command will re-acquire them.
                collectionOrView.reset();
                return runFindOnView(opCtx, *cq, verbosity, replyBuilder);
            }

            cq->setUseCqfIfEligible(true);

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
         * Runs a query using the following steps:
         *   --Parsing.
         *   --Acquire locks.
         *   --Plan query, obtaining an executor that can run it.
         *   --Generate the first batch.
         *   --Save state for getMore, transferring ownership of the executor to a ClientCursor.
         *   --Generate response to send to the client.
         */
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* replyBuilder) {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            const BSONObj& cmdObj = _request.body;

            // Parse the command BSON to a FindCommandRequest. Pass in the parsedNss in case cmdObj
            // does not have a UUID.
            const bool isOplogNss = (_ns == NamespaceString::kRsOplogNamespace);

            auto findCommand = _parseCmdObjectToFindCommandRequest(opCtx, _ns, _request);
            auto respSc =
                SerializationContext::stateCommandReply(findCommand->getSerializationContext());

            CurOp::get(opCtx)->beginQueryPlanningTimer();

            // Only allow speculative majority for internal commands that specify the correct flag.
            uassert(ErrorCodes::ReadConcernMajorityNotEnabled,
                    "Majority read concern is not enabled.",
                    !(repl::ReadConcernArgs::get(opCtx).isSpeculativeMajority() &&
                      !findCommand->getAllowSpeculativeMajorityRead()));

            const bool isFindByUUID = findCommand->getNamespaceOrUUID().isUUID();
            uassert(ErrorCodes::InvalidOptions,
                    "When using the find command by UUID, the collectionUUID parameter cannot also "
                    "be specified",
                    !isFindByUUID || !findCommand->getCollectionUUID());

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "It is illegal to open a tailable cursor in a transaction",
                    !(opCtx->inMultiDocumentTransaction() && findCommand->getTailable()));

            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    "The 'readOnce' option is not supported within a transaction.",
                    !txnParticipant || !opCtx->inMultiDocumentTransaction() ||
                        !findCommand->getReadOnce());

            // Validate term before acquiring locks, if provided.
            auto term = findCommand->getTerm();
            if (term) {
                // Note: updateTerm returns ok if term stayed the same.
                uassertStatusOK(replCoord->updateTerm(opCtx, *term));
            }

            const bool includeMetrics = findCommand->getIncludeQueryStatsMetrics();

            // The presence of a term in the request indicates that this is an internal replication
            // oplog read request.
            if (term && isOplogNss) {
                // We do not want to wait to take tickets for internal (replication) oplog reads.
                // Stalling on ticket acquisition can cause complicated deadlocks. Primaries may
                // depend on data reaching secondaries in order to proceed; and secondaries may get
                // stalled replicating because of an inability to acquire a read ticket.
                shard_role_details::getLocker(opCtx)->setAdmissionPriority(
                    AdmissionContext::Priority::kImmediate);
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

                auto cmdSort = findCommand->getSort();
                if (!cmdSort.isEmpty()) {
                    BSONElement natural = cmdSort[query_request_helper::kNaturalSortField];
                    if (natural) {
                        reverseScan = natural.safeNumberInt() < 0;
                    }
                }

                auto isInternal = isInternalClient(opCtx);
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
                tracker.emplace(
                    opCtx,
                    nss,
                    Top::LockType::ReadLocked,
                    AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                    CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nss.dbName()));
            };
            auto const nssOrUUID = findCommand->getNamespaceOrUUID();
            if (nssOrUUID.isNamespaceString()) {
                CommandHelpers::ensureValidCollectionName(nssOrUUID.nss());
                initializeTracker(nssOrUUID.nss());
            }
            const auto acquisitionRequest = [&] {
                auto req = CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx, nssOrUUID, AcquisitionPrerequisites::kRead);

                req.expectedUUID = findCommand->getCollectionUUID();
                return req;
            }();

            boost::optional<CollectionOrViewAcquisition> collectionOrView =
                acquireCollectionOrViewMaybeLockFree(opCtx, acquisitionRequest);
            const NamespaceString nss = collectionOrView->nss();

            if (!tracker) {
                initializeTracker(nss);
            }

            if (!findCommand->getMirrored()) {
                if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                        opCtx,
                        ns(),
                        analyze_shard_key::SampledCommandNameEnum::kFind,
                        *findCommand)) {
                    analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                        ->addFindQuery(*sampleId,
                                       nss,
                                       findCommand->getFilter(),
                                       findCommand->getCollation(),
                                       findCommand->getLet())
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
            InterruptibleLockGuard interruptibleLockAcquisition(
                shard_role_details::getLocker(opCtx));

            const auto& collectionPtr = collectionOrView->getCollectionPtr();

            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "UUID " << findCommand->getNamespaceOrUUID().uuid()
                                  << " specified in query request not found",
                    collectionOrView->collectionExists() || !isFindByUUID);

            bool isClusteredCollection = false;
            if (collectionOrView->collectionExists()) {
                if (isFindByUUID) {
                    // Replace the UUID in the find command with the fully qualified namespace of
                    // the looked up Collection.
                    findCommand->setNss(nss);
                }

                // Tailing a replicated capped clustered collection requires majority read concern.
                const bool isTailable = findCommand->getTailable();
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
                    opCtx, findCommand->getResumeAfter(), isClusteredCollection));
            }

            auto cq = parseQueryAndBeginOperation(
                opCtx, *collectionOrView, nss, _request.body, std::move(findCommand));
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

            cq->setUseCqfIfEligible(true);

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

            FindCommon::waitInFindBeforeMakingBatch(opCtx, *exec->getCanonicalQuery());

            const FindCommandRequest& originalFC =
                exec->getCanonicalQuery()->getFindCommandRequest();

            // Stream query results, adding them to a BSONArray as we go.
            CursorResponseBuilder::Options options;
            options.isInitialResponse = true;
            if (!opCtx->inMultiDocumentTransaction()) {
                options.atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
            }
            CursorResponseBuilder firstBatch(replyBuilder, options);
            BSONObj obj;
            PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
            std::uint64_t numResults = 0;
            bool stashedResult = false;
            ResourceConsumption::DocumentUnitCounter docUnitsReturned;

            try {
                while (!FindCommon::enoughForFirstBatch(originalFC, numResults) &&
                       PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr))) {
                    // If we can't fit this result inside the current batch, then we stash it for
                    // later.
                    if (!FindCommon::haveSpaceForNext(obj, numResults, firstBatch.bytesUsed())) {
                        exec->stashResult(obj);
                        stashedResult = true;
                        break;
                    }

                    // If this executor produces a postBatchResumeToken, add it to the response.
                    firstBatch.setPostBatchResumeToken(exec->getPostBatchResumeToken());

                    // Add result to output buffer.
                    firstBatch.append(obj);
                    numResults++;
                    docUnitsReturned.observeOne(obj.objsize());
                }
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

            // For empty batches, or in the case where the final result was added to the batch
            // rather than being stashed, we update the PBRT to ensure that it is the most recent
            // available.
            if (!stashedResult) {
                firstBatch.setPostBatchResumeToken(exec->getPostBatchResumeToken());
            }

            // Set up the cursor for getMore.
            CursorId cursorId = 0;
            if (shouldSaveCursor(opCtx, collectionPtr, state, exec.get())) {
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
                // During destruction of the pinned cursor we may discover that the operation has
                // been interrupted. This would cause the transaction resources we just stashed to
                // be released and destroyed. As we hold a reference here to them in the form of the
                // acquisition which modifies the resources, we must release it now before
                // destroying the pinned cursor.
                collectionOrView.reset();
            } else {
                endQueryOp(opCtx, collectionPtr, *exec, numResults, boost::none, cmdObj);
                // We want to destroy executor as soon as possible to release any resources locks it
                // may hold.
                exec.reset();
                collectionOrView.reset();
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

            // An empty PrivilegeVector for explain is acceptable because these privileges are only
            // checked on getMore and explain will not open a cursor.
            const auto privileges = verbosity ? PrivilegeVector()
                                              : uassertStatusOK(auth::getPrivilegesForAggregate(
                                                    AuthorizationSession::get(opCtx->getClient()),
                                                    aggRequest.getNamespace(),
                                                    aggRequest,
                                                    false));
            const auto status =
                runAggregate(opCtx, aggRequest, _request.body, privileges, replyBuilder);
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

        // Parses the command object to a FindCommandRequest. If the client request did not specify
        // any runtime constants, make them available to the query here.
        std::unique_ptr<FindCommandRequest> _parseCmdObjectToFindCommandRequest(
            OperationContext* opCtx, NamespaceString nss, const OpMsgRequest& request) {
            // check validated tenantId and set the flag on the serialization context object
            auto reqSc = request.getSerializationContext();

            auto findCommand = query_request_helper::makeFromFindCommand(
                request.body,
                auth::ValidatedTenancyScope::get(opCtx),
                nss.tenantId(),
                reqSc,
                APIParameters::get(opCtx).getAPIStrict().value_or(false));

            // TODO: SERVER-73632 Remove Feature Flag for PM-635.
            // Forbid users from passing 'querySettings' explicitly.
            uassert(7746901,
                    "BSON field 'querySettings' is an unknown field",
                    isInternalClient(opCtx) || !findCommand->getQuerySettings().has_value());

            // Rewrite any FLE find payloads that exist in the query if this is a FLE 2 query.
            if (shouldDoFLERewrite(findCommand)) {
                invariant(findCommand->getNamespaceOrUUID().isNamespaceString());
                LOGV2_DEBUG(7964101,
                            2,
                            "Processing Queryable Encryption command",
                            "cmd"_attr = request.body);

                if (!findCommand->getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    processFLEFindD(
                        opCtx, findCommand->getNamespaceOrUUID().nss(), findCommand.get());
                }
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
            }

            if (findCommand->getMirrored().value_or(false)) {
                const auto& invocation = CommandInvocation::get(opCtx);
                invocation->markMirrored();
            }

            return findCommand;
        }
    };
};
MONGO_REGISTER_COMMAND(FindCmd).forShard();

}  // namespace
}  // namespace mongo
