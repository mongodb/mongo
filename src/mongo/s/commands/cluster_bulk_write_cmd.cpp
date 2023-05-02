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

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/bulk_write_common.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/router_stage_queued_data.h"

namespace mongo {
namespace {

class ClusterBulkWriteCmd : public BulkWriteCmdVersion1Gen<ClusterBulkWriteCmd> {
public:
    bool adminOnly() const final {
        return true;
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

    std::string help() const override {
        return "command to apply inserts, updates and deletes in bulk";
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) final {
            uassert(
                ErrorCodes::CommandNotSupported,
                "BulkWrite may not be run without featureFlagBulkWriteCommand enabled",
                gFeatureFlagBulkWriteCommand.isEnabled(serverGlobalParams.featureCompatibility));

            bulk_write_common::validateRequest(request(), opCtx->isRetryableWrite());

            auto replyItems = cluster::bulkWrite(opCtx, request());
            return _populateCursorReply(opCtx, std::move(replyItems));
        }

        void doCheckAuthorization(OperationContext* opCtx) const final try {
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivileges(bulk_write_common::getPrivileges(request())));
        } catch (const DBException& e) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(e.code());
            throw;
        }

    private:
        Reply _populateCursorReply(OperationContext* opCtx,
                                   std::vector<BulkWriteReplyItem> replyItems) {
            const auto& req = request();
            auto reqObj = unparsedRequest().body;

            const NamespaceString cursorNss =
                NamespaceString::makeBulkWriteNSS(req.getDollarTenant());
            ClusterClientCursorParams params(cursorNss,
                                             APIParameters::get(opCtx),
                                             ReadPreferenceSetting::get(opCtx),
                                             repl::ReadConcernArgs::get(opCtx));

            long long batchSize = std::numeric_limits<long long>::max();
            if (req.getCursor() && req.getCursor()->getBatchSize()) {
                params.batchSize = request().getCursor()->getBatchSize();
                batchSize = *req.getCursor()->getBatchSize();
            }
            params.originatingCommandObj = reqObj.getOwned();
            params.originatingPrivileges = bulk_write_common::getPrivileges(req);
            params.lsid = opCtx->getLogicalSessionId();
            params.txnNumber = opCtx->getTxnNumber();

            auto queuedDataStage = std::make_unique<RouterStageQueuedData>(opCtx);
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
                collectTelemetryMongos(opCtx, reqObj, numRepliesInFirstBatch);
                return BulkWriteCommandReply(
                    BulkWriteCommandResponseCursor(
                        0, std::vector<BulkWriteReplyItem>(std::move(replyItems))),
                    0 /* TODO SERVER-76267: correctly populate numErrors */);
            }

            ccc->detachFromOperationContext();
            ccc->incNBatches();
            collectTelemetryMongos(opCtx, ccc, numRepliesInFirstBatch);

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
            return BulkWriteCommandReply(
                BulkWriteCommandResponseCursor(
                    cursorId, std::vector<BulkWriteReplyItem>(std::move(replyItems))),
                0 /* TODO SERVER-76267: correctly populate numErrors */);
        }
    };

} clusterBulkWriteCmd;

}  // namespace
}  // namespace mongo
