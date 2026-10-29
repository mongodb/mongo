// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/collection_uuid_mismatch.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/list_collections_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;
Status populateCollectionUUIDMismatch(OperationContext* opCtx,
                                      const Status& collectionUUIDMismatch) {
    tassert(6487200,
            str::stream() << "Expected CollectionUUIDMismatch but got " << collectionUUIDMismatch,
            collectionUUIDMismatch == ErrorCodes::CollectionUUIDMismatch);

    auto info = collectionUUIDMismatch.extraInfo<CollectionUUIDMismatchInfo>();
    if (info->actualCollection()) {
        return collectionUUIDMismatch;
    }

    // The listCollections command cannot be run in multi-document transactions, so run it using an
    // alternative client.
    auto client =
        opCtx->getService()->makeClient("populateCollectionUUIDMismatch", Client::noSession());
    auto alternativeOpCtx = client->makeOperationContext();
    opCtx = alternativeOpCtx.get();
    AlternativeClientRegion acr{client};

    ListCollections listCollections;
    // Empty tenant id is acceptable here as command's tenant id will not be serialized to BSON.
    listCollections.setDbName(info->dbName());
    listCollections.setFilter(BSON("info.uuid" << info->collectionUUID()));

    sharding::router::DBPrimaryRouter router(opCtx, info->dbName());
    return router.route(
        "populateCollectionUUIDMismatch"sv,
        [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) -> Status {
            auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                opCtx,
                info->dbName(),
                dbInfo,
                listCollections.toBSON(),
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                Shard::RetryPolicy::kIdempotent);
            if (!response.swResponse.isOK()) {
                return response.swResponse.getStatus();
            }

            if (auto status = getStatusFromCommandResult(response.swResponse.getValue().data);
                !status.isOK()) {
                return status;
            }

            if (auto actualCollectionElem = bson::extractElementAtDottedPath(
                    response.swResponse.getValue().data, "cursor.firstBatch.0.name")) {
                return {CollectionUUIDMismatchInfo{info->dbName(),
                                                   info->collectionUUID(),
                                                   info->expectedCollection(),
                                                   actualCollectionElem.str()},
                        collectionUUIDMismatch.reason()};
            }
            return collectionUUIDMismatch;
        });
}
}  // namespace mongo
