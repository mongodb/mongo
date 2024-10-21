/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/bulk_write_common.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_parser.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/s/commands/query_cmd/cluster_explain.h"
#include "mongo/s/commands/query_cmd/cluster_write_cmd.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/s/query/exec/cluster_client_cursor_guard.h"
#include "mongo/s/query/exec/cluster_client_cursor_impl.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/s/query/exec/router_stage_queued_data.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

namespace mongo {

template <typename Impl>
class ClusterBulkWriteCmd : public Command {
public:
    ClusterBulkWriteCmd() : Command(Impl::kName) {}

    bool adminOnly() const final {
        return true;
    }

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final {
        auto parsedRequest =
            BulkWriteCommandRequest::parse(IDLParserContext{"clusterBulkWriteParse"}, request);
        bulk_write_exec::addIdsForInserts(parsedRequest);
        return std::make_unique<Invocation>(this, request, std::move(parsedRequest));
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
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

    LogicalOp getLogicalOp() const final {
        return LogicalOp::opBulkWrite;
    }

    std::string help() const override {
        return "command to apply inserts, updates and deletes in bulk";
    }

    class Invocation : public CommandInvocation {
    public:
        Invocation(const ClusterBulkWriteCmd* command,
                   const OpMsgRequest& request,
                   BulkWriteCommandRequest bulkRequest)
            : CommandInvocation(command),
              _opMsgRequest{&request},
              _request{std::move(bulkRequest)} {
            uassert(ErrorCodes::CommandNotSupported,
                    "BulkWrite may not be run without featureFlagBulkWriteCommand enabled",
                    gFeatureFlagBulkWriteCommand.isEnabled(
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

            bulk_write_common::validateRequest(_request, /*isRouter=*/true);
        }

        const BulkWriteCommandRequest& getBulkRequest() const {
            return _request;
        }

        bool getBypass() const {
            return _request.getBypassDocumentValidation();
        }

        const GenericArguments& getGenericArguments() const override {
            return _request.getGenericArguments();
        }

    private:
        void preRunImplHook(OperationContext* opCtx) const {
            Impl::checkCanRunHere(opCtx);
        }

        void preExplainImplHook(OperationContext* opCtx) const {
            Impl::checkCanExplainHere(opCtx);
        }

        void doCheckAuthorizationHook(AuthorizationSession* authzSession) const {
            Impl::doCheckAuthorization(authzSession, getBypass(), getBulkRequest());
        }

        NamespaceString ns() const final {
            return NamespaceString(_request.getDbName());
        }

        std::vector<NamespaceString> allNamespaces() const final {
            const auto& nsInfos = _request.getNsInfo();
            std::vector<NamespaceString> result(nsInfos.size());

            for (auto& nsInfo : nsInfos) {
                result.emplace_back(nsInfo.getNs());
            }

            return result;
        }

        const DatabaseName& db() const final {
            return _request.getDbName();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            try {
                doCheckAuthorizationHook(AuthorizationSession::get(opCtx->getClient()));
            } catch (const DBException& e) {
                NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(e.code());
                throw;
            }
        }

        const ClusterBulkWriteCmd* command() const {
            return static_cast<const ClusterBulkWriteCmd*>(definition());
        }

        BulkWriteCommandReply _populateCursorReply(
            OperationContext* opCtx,
            BulkWriteCommandRequest& bulkRequest,
            const OpMsgRequest& unparsedRequest,
            bulk_write_exec::BulkWriteReplyInfo replyInfo) const {
            const auto& req = bulkRequest;
            auto reqObj = unparsedRequest.body;
            auto& [replyItems, summaryFields, wcErrors, retriedStmtIds, _] = replyInfo;
            const NamespaceString cursorNss =
                NamespaceString::makeBulkWriteNSS(req.getDbName().tenantId());

            if (bulk_write_common::isUnacknowledgedBulkWrite(opCtx)) {
                // Skip cursor creation and return the simplest reply.
                return BulkWriteCommandReply(BulkWriteCommandResponseCursor(
                                                 0 /* cursorId */, {} /* firstBatch */, cursorNss),
                                             summaryFields.nErrors,
                                             summaryFields.nInserted,
                                             summaryFields.nMatched,
                                             summaryFields.nModified,
                                             summaryFields.nUpserted,
                                             summaryFields.nDeleted);
            }

            ClusterClientCursorParams params(
                cursorNss,
                APIParameters::get(opCtx),
                ReadPreferenceSetting::get(opCtx),
                repl::ReadConcernArgs::get(opCtx),
                [&] {
                    if (!opCtx->getLogicalSessionId())
                        return OperationSessionInfoFromClient();
                    // TODO (SERVER-80525): This code path does not
                    // clear the setAutocommit field on the presence of
                    // TransactionRouter::get
                    return OperationSessionInfoFromClient(
                        *opCtx->getLogicalSessionId(),
                        // Retryable writes will have a txnNumber we do not want to associate with
                        // the cursor. We only want to set this field for transactions.
                        opCtx->inMultiDocumentTransaction() ? opCtx->getTxnNumber() : boost::none);
                }());

            long long batchSize = std::numeric_limits<long long>::max();
            if (req.getCursor() && req.getCursor()->getBatchSize()) {
                params.batchSize = req.getCursor()->getBatchSize();
                batchSize = *req.getCursor()->getBatchSize();
            }
            params.originatingCommandObj = reqObj.getOwned();
            params.originatingPrivileges = bulk_write_common::getPrivileges(req);

            auto queuedDataStage = std::make_unique<RouterStageQueuedData>(opCtx);
            BulkWriteCommandReply reply;
            reply.setNErrors(summaryFields.nErrors);
            reply.setNInserted(summaryFields.nInserted);
            reply.setNDeleted(summaryFields.nDeleted);
            reply.setNMatched(summaryFields.nMatched);
            reply.setNModified(summaryFields.nModified);
            reply.setNUpserted(summaryFields.nUpserted);
            reply.setWriteConcernError(wcErrors);
            reply.setRetriedStmtIds(retriedStmtIds);

            for (auto& replyItem : replyItems) {
                queuedDataStage->queueResult(replyItem.toBSON());
            }

            auto ccc =
                ClusterClientCursorImpl::make(opCtx, std::move(queuedDataStage), std::move(params));

            size_t numRepliesInFirstBatch = 0;
            FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                auto next = uassertStatusOK(ccc->next());

                if (next.isEOF()) {
                    break;
                }

                auto nextObj = *next.getResult();
                if (!responseSizeTracker.haveSpaceForNext(nextObj)) {
                    ccc->queueResult(nextObj);
                    break;
                }

                numRepliesInFirstBatch++;
                responseSizeTracker.add(nextObj);
            }
            if (numRepliesInFirstBatch == replyItems.size()) {
                replyItems.resize(numRepliesInFirstBatch);
                reply.setCursor(BulkWriteCommandResponseCursor(
                    0, std::vector<BulkWriteReplyItem>(std::move(replyItems)), cursorNss));
                return reply;
            }

            ccc->detachFromOperationContext();
            ccc->incNBatches();

            auto authUser =
                AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName();
            auto cursorId = uassertStatusOK(Grid::get(opCtx)->getCursorManager()->registerCursor(
                opCtx,
                ccc.releaseCursor(),
                cursorNss,
                ClusterCursorManager::CursorType::QueuedData,
                ClusterCursorManager::CursorLifetime::Mortal,
                authUser));

            // Record the cursorID in CurOp.
            CurOp::get(opCtx)->debug().cursorid = cursorId;

            replyItems.resize(numRepliesInFirstBatch);
            reply.setCursor(BulkWriteCommandResponseCursor(
                cursorId, std::vector<BulkWriteReplyItem>(std::move(replyItems)), cursorNss));
            return reply;
        }

        bool runImpl(OperationContext* opCtx,
                     const OpMsgRequest& request,
                     BulkWriteCommandRequest& bulkRequest,
                     BSONObjBuilder& result) const {
            BulkWriteCommandReply response;
            // We pre-create the targeters to pass in, as having access to the targeters is
            // necessary for handling WouldChangeOwningShard errors, as for TS views we need to be
            // able to obtain the bucket namespace to write to which we get via targeter.
            std::vector<std::unique_ptr<NSTargeter>> targeters;
            targeters.reserve(bulkRequest.getNsInfo().size());
            for (const auto& nsInfo : bulkRequest.getNsInfo()) {
                targeters.push_back(
                    std::make_unique<CollectionRoutingInfoTargeter>(opCtx, nsInfo.getNs()));
            }

            if (auto let = bulkRequest.getLet()) {
                // Evaluate the let parameters.
                auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).letParameters(*let).build();
                expCtx->variables.seedVariablesWithLetParameters(expCtx.get(), *let);
                bulkRequest.setLet(expCtx->variables.toBSON(expCtx->variablesParseState, *let));
            }

            // Dispatch the bulk write through the cluster.
            // - To ensure that possible writeErrors are properly managed, a "fire and forget"
            //   request needs to be temporarily upgraded to 'w:1'(unless the request belongs to a
            //   transaction, where per-operation WC settings are not supported);
            // - Once done, The original WC is re-established to allow _populateCursorReply
            //   evaluating whether a reply needs to be returned to the external client.
            auto bulkWriteReply = [&] {
                WriteConcernOptions originalWC = opCtx->getWriteConcern();
                ScopeGuard resetWriteConcernGuard(
                    [opCtx, &originalWC] { opCtx->setWriteConcern(originalWC); });
                if (auto wc = opCtx->getWriteConcern();
                    !wc.requiresWriteAcknowledgement() && !opCtx->inMultiDocumentTransaction()) {
                    wc.w = 1;
                    opCtx->setWriteConcern(wc);
                }
                return cluster::bulkWrite(opCtx, bulkRequest, targeters);
            }();

            bool updatedShardKey =
                handleWouldChangeOwningShardError(opCtx, bulkRequest, bulkWriteReply, targeters);
            bulk_write_exec::BulkWriteExecStats execStats = std::move(bulkWriteReply.execStats);

            response = _populateCursorReply(opCtx, bulkRequest, request, std::move(bulkWriteReply));
            result.appendElements(response.toBSON());

            // TODO SERVER-83869 handle BulkWriteExecStats for batches of size > 1 containing
            // updates that modify a document’s owning shard.
            execStats.updateMetrics(opCtx, targeters, updatedShardKey);

            return true;
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            preRunImplHook(opCtx);

            BSONObjBuilder bob = result->getBodyBuilder();
            bool ok = runImpl(opCtx, *_opMsgRequest, _request, bob);
            if (!ok) {
                CommandHelpers::appendSimpleCommandStatus(bob, ok);
            }
        }

        /**
         * Inspects the provided response to determine if it contains any 'WouldChangeOwningShard'
         * errors.
         * - If none are found, returns boost::none.
         * - If exactly 1 is found and the batchSize is 1, returns the contained information.
         * - If 1+ are found and the batchSize is > 1, it means the user sent a write that changes
         *   a document's owning shard but did not send it in its own batch, which is currently
         *   unsupported behavior. Accordingly, if we see this behavior:
         *     - In a txn, we raise a top-level error.
         *     - Otherwise, we set the reply status for the corresponding write(s) to a new error.
         */
        boost::optional<WouldChangeOwningShardInfo> getWouldChangeOwningShardErrorInfo(
            bulk_write_exec::BulkWriteReplyInfo& response, bool inTransaction) const {
            if (response.summaryFields.nErrors == 0) {
                return boost::none;
            }

            auto batchSize = _request.getOps().size();
            for (auto& replyItem : response.replyItems) {
                if (replyItem.getStatus() == ErrorCodes::WouldChangeOwningShard) {
                    if (batchSize != 1) {
                        if (inTransaction)
                            uasserted(ErrorCodes::InvalidOptions,
                                      "Document shard key value updates that cause the doc to move "
                                      "shards must be sent with write batch of size 1");

                        replyItem.setStatus(
                            {ErrorCodes::InvalidOptions,
                             "Document shard key value updates that cause the doc to move shards "
                             "must be sent with write batch of size 1"});
                    } else {
                        BSONObjBuilder extraInfoBuilder;
                        replyItem.getStatus().extraInfo()->serialize(&extraInfoBuilder);
                        auto extraInfo = extraInfoBuilder.obj();
                        return WouldChangeOwningShardInfo::parseFromCommandError(extraInfo);
                    }
                }
            }

            return boost::none;
        }

        /**
         * If the provided response contains a WouldChangeOwningShardError, handles executing the
         * transactional delete from old shard and insert to new shard, and updates the response
         * accordingly. If it does not contain such an error, does nothing.
         *
         * Returns true if a document shard key update was actually performed.
         */
        bool handleWouldChangeOwningShardError(
            OperationContext* opCtx,
            const BulkWriteCommandRequest& request,
            bulk_write_exec::BulkWriteReplyInfo& response,
            const std::vector<std::unique_ptr<NSTargeter>>& targeters) const {
            auto wcosInfo =
                getWouldChangeOwningShardErrorInfo(response, opCtx->inMultiDocumentTransaction());
            if (!wcosInfo)
                return false;

            // A shard should only give us back this error if one of these conditions are true. If
            // neither are, we would get back an IllegalOperation error instead.
            tassert(7279300,
                    "Unexpectedly got a WouldChangeOwningShard error back from a shard outside of "
                    "a retryable write or transaction",
                    opCtx->isRetryableWrite() || opCtx->inMultiDocumentTransaction());

            // Obtain the targeted namespace that we got the WCOS error for.This is always the
            // targeted namespace for the first op, as a write that change's a document's owning
            // shard must be the only write in the incoming request.
            auto firstWriteNSIndex = BulkWriteCRUDOp(request.getOps()[0]).getNsInfoIdx();
            auto nss = targeters[firstWriteNSIndex]->getNS();

            bool updatedShardKey = false;
            boost::optional<BSONObj> upsertedId;

            opCtx->setQuerySamplingOptions(OperationContext::QuerySamplingOptions::kOptOut);

            if (opCtx->inMultiDocumentTransaction()) {
                std::tie(updatedShardKey, upsertedId) =
                    documentShardKeyUpdateUtil::handleWouldChangeOwningShardErrorTransactionLegacy(
                        opCtx, nss, *wcosInfo);
            } else {
                // We must be in a retryable write.
                std::tie(updatedShardKey, upsertedId) = documentShardKeyUpdateUtil::
                    handleWouldChangeOwningShardErrorRetryableWriteLegacy(
                        opCtx,
                        nss,
                        // RerunOriginalWriteFn:
                        [&]() {
                            response = cluster::bulkWrite(opCtx, request, targeters);
                            return getWouldChangeOwningShardErrorInfo(
                                response, opCtx->inMultiDocumentTransaction());
                        },
                        // ProcessWCEFn:
                        [&](std::unique_ptr<WriteConcernErrorDetail> wce) {
                            auto bwWce = BulkWriteWriteConcernError::parseOwned(
                                IDLParserContext("BulkWriteWriteConcernError"), wce->toBSON());
                            response.wcErrors = bwWce;
                        },
                        // ProcessWriteErrorFn:
                        [&](DBException& e) { response.replyItems[0].setStatus(e.toStatus()); });
            }

            // See BulkWriteOp::generateReplyInfo, it is easier to handle this metric for
            // WouldChangeOwningShardError here.
            globalOpCounters.gotUpdate();

            if (updatedShardKey) {
                // Remove the WCOS error from the count. Since this write must have been sent in its
                // own batch it is not possible there are statistics for any other writes in
                // summaryFields.
                response.summaryFields.nErrors = 0;

                auto successReply = BulkWriteReplyItem(0);
                // 'n' is always 1 for an update, regardless of it was an upsert, and indicates the
                // number matched *or* inserted.
                successReply.setN(1);

                if (upsertedId) {
                    successReply.setNModified(0);
                    successReply.setUpserted(IDLAnyTypeOwned(upsertedId->getField("_id")));
                    response.summaryFields.nUpserted = 1;
                } else {
                    successReply.setNModified(1);
                    response.summaryFields.nMatched = 1;
                    response.summaryFields.nModified = 1;
                }

                response.replyItems.clear();
                if (!request.getErrorsOnly()) {
                    response.replyItems.push_back(successReply);
                }
            }

            return updatedShardKey;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            preExplainImplHook(opCtx);

            uassert(ErrorCodes::InvalidLength,
                    "explained bulkWrite must be of size 1",
                    _request.getOps().size() == 1U);

            auto op = BulkWriteCRUDOp(_request.getOps()[0]);
            BatchedCommandRequest batchedRequest = [&]() {
                auto type = op.getType();
                if (type == BulkWriteCRUDOp::kInsert) {
                    return BatchedCommandRequest::buildInsertOp(
                        _request.getNsInfo()[op.getNsInfoIdx()].getNs(),
                        {op.getInsert()->getDocument()});
                } else if (type == BulkWriteCRUDOp::kUpdate) {
                    return BatchedCommandRequest(
                        bulk_write_common::makeUpdateCommandRequestFromUpdateOp(
                            op.getUpdate(), _request, 0));
                } else if (type == BulkWriteCRUDOp::kDelete) {
                    return BatchedCommandRequest(bulk_write_common::makeDeleteCommandRequestForFLE(
                        opCtx, op.getDelete(), _request, _request.getNsInfo()[op.getNsInfoIdx()]));
                } else {
                    MONGO_UNREACHABLE;
                }
            }();

            ClusterWriteCmd::executeWriteOpExplain(
                opCtx, batchedRequest, batchedRequest.toBSON(), verbosity, result);
        }

        const OpMsgRequest* _opMsgRequest;
        BulkWriteCommandRequest _request;
    };
};

}  // namespace mongo
