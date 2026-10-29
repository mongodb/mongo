// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/logical_session_cache_factory_mongod.h"

#include "mongo/db/global_catalog/ddl/sessions_collection_config_server.h"
#include "mongo/db/global_catalog/ddl/sessions_collection_sharded.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_cache_impl.h"
#include "mongo/db/session/service_liaison_impl.h"
#include "mongo/db/session/service_liaison_shard.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/db/session/sessions_collection_rs.h"
#include "mongo/db/session/sessions_collection_standalone.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

std::unique_ptr<LogicalSessionCache> makeLogicalSessionCacheD(LogicalSessionCacheServer state) {
    auto liaison = std::make_unique<ServiceLiaisonImpl>(
        service_liaison_shard_callbacks::getOpenCursorSessions,
        service_liaison_shard_callbacks::killCursorsWithMatchingSessions);

    auto sessionsColl = [&]() -> std::shared_ptr<SessionsCollection> {
        switch (state) {
            case LogicalSessionCacheServer::kSharded:
                return std::make_shared<SessionsCollectionSharded>();
            case LogicalSessionCacheServer::kConfigServer:
                return std::make_shared<SessionsCollectionConfigServer>();
            case LogicalSessionCacheServer::kReplicaSet:
                return std::make_shared<SessionsCollectionRS>();
            case LogicalSessionCacheServer::kStandalone:
                return std::make_shared<SessionsCollectionStandalone>();
        }

        MONGO_UNREACHABLE;
    }();

    auto reapSessionsOlderThanFn = [](OperationContext* opCtx,
                                      SessionsCollection& sessionsCollection,
                                      Date_t possiblyExpired) {
        return MongoDSessionCatalog::get(opCtx)->reapSessionsOlderThan(
            opCtx, sessionsCollection, possiblyExpired);
    };

    return std::make_unique<LogicalSessionCacheImpl>(
        std::move(liaison), std::move(sessionsColl), std::move(reapSessionsOlderThanFn));
}

}  // namespace mongo
