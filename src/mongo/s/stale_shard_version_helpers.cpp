/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/s/stale_shard_version_helpers.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/database_name.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace shard_version_retry {

void checkErrorStatusAndMaxRetries(const Status& status,
                                   const NamespaceString& nss,
                                   CatalogCache* catalogCache,
                                   StringData taskDescription,
                                   size_t numAttempts) {
    auto logAndTestMaxRetries = [numAttempts, taskDescription](const Status& status) {
        if (numAttempts > kMaxNumStaleVersionRetries) {
            uassertStatusOKWithContext(status,
                                       str::stream() << "Exceeded maximum number of "
                                                     << kMaxNumStaleVersionRetries
                                                     << " retries attempting " << taskDescription);
        }

        LOGV2_DEBUG(4553800,
                    3,
                    "Retrying {task_description}. Got error: {exception}",
                    "task_description"_attr = taskDescription,
                    "exception"_attr = status);
    };

    if (status == ErrorCodes::StaleDbVersion) {
        auto staleInfo = status.extraInfo<
            error_details::ErrorExtraInfoForImpl<ErrorCodes::StaleDbVersion>::type>();
        // If the database version is stale, refresh its entry in the catalog cache.
        catalogCache->onStaleDatabaseVersion(staleInfo->getDb(), staleInfo->getVersionWanted());

        logAndTestMaxRetries(status);
        return;
    }

    if (status.isA<ErrorCategory::StaleShardVersionError>()) {
        // If the exception provides a shardId, add it to the set of shards requiring a refresh.
        // If the cache currently considers the collection to be unsharded, this will trigger an
        // epoch refresh. If no shard is provided, then the epoch is stale and we must refresh.
        if (auto staleInfo = status.extraInfo<StaleConfigInfo>()) {
            catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
                staleInfo->getNss(), staleInfo->getVersionWanted(), staleInfo->getShardId());
        } else {
            catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
        }

        logAndTestMaxRetries(status);
        return;
    }

    uassertStatusOK(status);
}

}  // namespace shard_version_retry
}  // namespace mongo
