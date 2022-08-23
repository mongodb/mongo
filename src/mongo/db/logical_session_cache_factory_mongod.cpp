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


#include "mongo/platform/basic.h"

#include "mongo/db/logical_session_cache_factory_mongod.h"

#include <memory>

#include "mongo/db/s/sessions_collection_config_server.h"
#include "mongo/db/service_liaison_mongod.h"
#include "mongo/db/session/logical_session_cache_impl.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/sessions_collection_rs.h"
#include "mongo/db/session/sessions_collection_standalone.h"
#include "mongo/s/sessions_collection_sharded.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

std::unique_ptr<LogicalSessionCache> makeLogicalSessionCacheD(LogicalSessionCacheServer state) {
    auto liaison = std::make_unique<ServiceLiaisonMongod>();

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
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        return mongoDSessionCatalog->reapSessionsOlderThan(
            opCtx, sessionsCollection, possiblyExpired);
    };
    return std::make_unique<LogicalSessionCacheImpl>(
        std::move(liaison), std::move(sessionsColl), std::move(reapSessionsOlderThanFn));
}

}  // namespace mongo
