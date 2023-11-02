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
#include "mongo/db/commands/bulk_write_common.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/commands/bulk_write_parser.h"
#include "mongo/db/curop.h"
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
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor.h"
#include "mongo/s/query/cluster_client_cursor_guard.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_result.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/s/query/router_stage_queued_data.h"
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

    const std::set<std::string>& apiVersions() const {
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
            uassert(
                ErrorCodes::CommandNotSupported,
                "BulkWrite may not be run without featureFlagBulkWriteCommand enabled",
                gFeatureFlagBulkWriteCommand.isEnabled(serverGlobalParams.featureCompatibility));

            bulk_write_common::validateRequest(_request, /*isRouter=*/true);
        }

        const BulkWriteCommandRequest& getBulkRequest() const {
            return _request;
        }

        bool getBypass() const {
            return _request.getBypassDocumentValidation();
        }

    private:
        void preRunImplHook(OperationContext* opCtx) const {
            Impl::checkCanRunHere(opCtx);
        }

        void doCheckAuthorizationHook(AuthorizationSession* authzSession) const {
            Impl::doCheckAuthorization(authzSession, getBypass(), getBulkRequest());
        }

        NamespaceString ns() const final {
            return NamespaceString(_request.getDbName());
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

            const NamespaceString cursorNss =
                NamespaceString::makeBulkWriteNSS(req.getDollarTenant());
            ClusterClientCursorParams params(cursorNss,
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
                                                     opCtx->getTxnNumber());
                                             }());

            long long batchSize = std::numeric_limits<long long>::max();
            if (req.getCursor() && req.getCursor()->getBatchSize()) {
                params.batchSize = req.getCursor()->getBatchSize();
                batchSize = *req.getCursor()->getBatchSize();
            }
            params.originatingCommandObj = reqObj.getOwned();
            params.originatingPrivileges = bulk_write_common::getPrivileges(req);

            auto queuedDataStage = std::make_unique<RouterStageQueuedData>(opCtx);
            auto& [replyItems, numErrors, wcErrors, retriedStmtIds] = replyInfo;
            BulkWriteCommandReply reply;
            reply.setNumErrors(numErrors);
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

            auto bulkWriteReply = cluster::bulkWrite(opCtx, bulkRequest);
            response = _populateCursorReply(opCtx, bulkRequest, request, std::move(bulkWriteReply));
            result.appendElements(response.toBSON());
            return true;
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) {
            preRunImplHook(opCtx);

            BSONObjBuilder bob = result->getBodyBuilder();
            bool ok = runImpl(opCtx, *_opMsgRequest, _request, bob);
            if (!ok) {
                CommandHelpers::appendSimpleCommandStatus(bob, ok);
            }
        }

        const OpMsgRequest* _opMsgRequest;
        BulkWriteCommandRequest _request;
    };
};

}  // namespace mongo
