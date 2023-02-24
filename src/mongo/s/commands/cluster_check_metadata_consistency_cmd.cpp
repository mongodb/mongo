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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/metadata_consistency_types_gen.h"
#include "mongo/db/query/find_common.h"
#include "mongo/s/check_metadata_consistency_gen.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"


namespace mongo {
namespace {

// We must allow some amount of overhead per result document, since when we make a cursor response
// the documents are elements of a BSONArray. The overhead is 1 byte/doc for the type + 1 byte/doc
// for the field name's null terminator + 1 byte per digit in the array index. The index can be no
// more than 8 decimal digits since the response is at most 16MB, and 16 * 1024 * 1024 < 1 * 10^8.
static const int kPerDocumentOverheadBytesUpperBound = 10;

MetadataConsistencyCommandLevelEnum getCommandLevel(const NamespaceString& nss) {
    if (nss.isAdminDB()) {
        return MetadataConsistencyCommandLevelEnum::kClusterLevel;
    } else if (nss.isCollectionlessCursorNamespace()) {
        return MetadataConsistencyCommandLevelEnum::kDatabaseLevel;
    } else {
        return MetadataConsistencyCommandLevelEnum::kCollectionLevel;
    }
}

class CheckMetadataConsistencyCmd final : public TypedCommand<CheckMetadataConsistencyCmd> {
public:
    using Request = CheckMetadataConsistency;
    using Response = CursorInitialReply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            const auto nss = ns();
            switch (getCommandLevel(nss)) {
                case MetadataConsistencyCommandLevelEnum::kClusterLevel:
                    return _runClusterLevel(opCtx, nss);
                case MetadataConsistencyCommandLevelEnum::kDatabaseLevel:
                    return _runDatabaseLevel(opCtx, nss);
                case MetadataConsistencyCommandLevelEnum::kCollectionLevel:
                    return _runCollectionLevel(nss);
                default:
                    MONGO_UNREACHABLE;
            }
        }

    private:
        Response _runClusterLevel(OperationContext* opCtx, const NamespaceString& nss) {
            uassert(
                ErrorCodes::InvalidNamespace,
                "cluster level mode must be run against the 'admin' database with {aggregate: 1}",
                nss.isCollectionlessCursorNamespace());

            const auto catalogClient = Grid::get(opCtx)->catalogClient();
            // TODO: SERVER-73978: Retrieve directly from configsvr a list of shards with its
            // corresponding primary databases.
            const auto databases =
                catalogClient->getAllDBs(opCtx, repl::ReadConcernLevel::kMajorityReadConcern);
            return _sendCommandToDbPrimaries(opCtx, nss, databases);
        }

        Response _runDatabaseLevel(OperationContext* opCtx, const NamespaceString& nss) {
            const auto catalogClient = Grid::get(opCtx)->catalogClient();
            const auto dbInfo = catalogClient->getDatabase(
                opCtx, nss.db(), repl::ReadConcernLevel::kMajorityReadConcern);
            return _sendCommandToDbPrimaries(opCtx, nss, {dbInfo});
        }

        Response _runCollectionLevel(const NamespaceString& nss) {
            uasserted(ErrorCodes::NotImplemented,
                      "collection level mode command is not implemented");
        }

        Response _sendCommandToDbPrimaries(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const std::vector<DatabaseType>& dbs) {
            const auto& cursorOpts = request().getCursor();

            std::vector<std::pair<ShardId, BSONObj>> requests;
            ShardsvrCheckMetadataConsistency shardsvrRequest{nss};
            shardsvrRequest.setDbName(nss.db());
            shardsvrRequest.setCursor(cursorOpts);

            // Send a unique request per shard that is a primary shard for, at least, one database.
            std::set<ShardId> shardIds;
            for (const auto& db : dbs) {
                const auto insertionRes = shardIds.insert(db.getPrimary());
                if (insertionRes.second) {
                    // The shard was not in the set, so we need to send a request to it.
                    requests.emplace_back(db.getPrimary(), shardsvrRequest.toBSON({}));
                }
            }

            // Send a request to the configsvr to check cluster metadata consistency.
            if (getCommandLevel(nss) == MetadataConsistencyCommandLevelEnum::kClusterLevel) {
                ConfigsvrCheckClusterMetadataConsistency configsvrRequest(nss);
                configsvrRequest.setDbName(nss.db());
                configsvrRequest.setCursor(cursorOpts);
                requests.emplace_back(ShardId::kConfigServerId, configsvrRequest.toBSON({}));
            }

            ClusterClientCursorParams params(
                nss,
                APIParameters::get(opCtx),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

            // Establish the cursors with a consistent shardVersion across shards.
            params.remotes = establishCursors(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                nss,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()),
                requests,
                false /*allowPartialResults*/);

            // Determine whether the cursor we may eventually register will be single- or
            // multi-target.
            const auto cursorType = params.remotes.size() > 1
                ? ClusterCursorManager::CursorType::MultiTarget
                : ClusterCursorManager::CursorType::SingleTarget;

            // Transfer the established cursors to a ClusterClientCursor.
            auto ccc = ClusterClientCursorImpl::make(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                std::move(params));

            auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
            size_t bytesBuffered = 0;
            std::vector<BSONObj> firstBatch;
            const auto batchSize = [&] {
                if (cursorOpts && cursorOpts->getBatchSize()) {
                    return (long long)*cursorOpts->getBatchSize();
                } else {
                    return query_request_helper::kDefaultBatchSize;
                }
            }();

            for (long long objCount = 0; objCount < batchSize; objCount++) {
                auto next = uassertStatusOK(ccc->next());
                if (next.isEOF()) {
                    // We reached end-of-stream, if all the remote cursors are exhausted, there is
                    // no hope of returning data and thus we need to close the mongos cursor as
                    // well.
                    cursorState = ClusterCursorManager::CursorState::Exhausted;
                    break;
                }

                auto nextObj = *next.getResult();

                // If adding this object will cause us to exceed the message size limit, then we
                // stash it for later.
                if (!FindCommon::haveSpaceForNext(nextObj, objCount, bytesBuffered)) {
                    ccc->queueResult(nextObj);
                    break;
                }

                bytesBuffered += nextObj.objsize() + kPerDocumentOverheadBytesUpperBound;
                firstBatch.push_back(std::move(nextObj));
            }

            ccc->detachFromOperationContext();

            if (cursorState == ClusterCursorManager::CursorState::Exhausted) {
                CursorInitialReply resp;
                InitialResponseCursor initRespCursor{std::move(firstBatch)};
                initRespCursor.setResponseCursorBase({0LL /* cursorId */, nss});
                resp.setCursor(std::move(initRespCursor));
                return resp;
            }

            ccc->incNBatches();

            auto authUser =
                AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName();

            // Register the cursor with the cursor manager for subsequent getMore's.
            auto clusterCursorId =
                uassertStatusOK(Grid::get(opCtx)->getCursorManager()->registerCursor(
                    opCtx,
                    ccc.releaseCursor(),
                    nss,
                    cursorType,
                    ClusterCursorManager::CursorLifetime::Mortal,
                    authUser));

            CursorInitialReply resp;
            InitialResponseCursor initRespCursor{std::move(firstBatch)};
            initRespCursor.setResponseCursorBase({clusterCursorId, nss});
            resp.setCursor(std::move(initRespCursor));
            return resp;
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            // TODO: SERVER-72667: Add authorization checks for cluster command
        }
    };
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(CheckMetadataConsistencyCmd,
                                       feature_flags::gCheckMetadataConsistency);

}  // namespace
}  // namespace mongo
