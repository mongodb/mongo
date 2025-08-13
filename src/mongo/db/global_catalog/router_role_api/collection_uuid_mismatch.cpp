/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/router_role_api/collection_uuid_mismatch.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
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
    //
    // TODO(SERVER-74658): Please revisit if this thread could be made killable.
    auto client = opCtx->getService()->makeClient("populateCollectionUUIDMismatch",
                                                  Client::noSession(),
                                                  ClientOperationKillableByStepdown{false});
    auto alternativeOpCtx = client->makeOperationContext();
    opCtx = alternativeOpCtx.get();
    AlternativeClientRegion acr{client};

    ListCollections listCollections;
    // Empty tenant id is acceptable here as command's tenant id will not be serialized to BSON.
    listCollections.setDbName(info->dbName());
    listCollections.setFilter(BSON("info.uuid" << info->collectionUUID()));

    sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), info->dbName());
    return router.route(
        opCtx,
        "populateCollectionUUIDMismatch"_sd,
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
