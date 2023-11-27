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

#include "mongo/db/locker_api.h"
#include "mongo/s/grid.h"

namespace mongo {

/**
 * Utility which determines which shard we should merge on. More precisely, if 'nss' resides on a
 * single shard, we should route to the shard which owns 'nss'.  Note that this decision is
 * inherently racy and subject to become stale. This is okay because either choice will work
 * correctly, we are simply applying a heuristic optimization.
 */
inline boost::optional<ShardId> determineSpecificMergeShard(OperationContext* opCtx,
                                                            const NamespaceString& nss) {
    // Do not attempt to refresh the catalog cache when holding a lock.
    // TODO SERVER-79580: Consider improving this function by:
    // - Passing 'allowLocks = true' to getCollectionRoutingInfo' as this would remove the need for
    // the check below.
    // - Moving this to MongoProcessInterface to the Mongos and ShardServer process interfaces.
    if (shard_role_details::getLocker(opCtx)->isLocked()) {
        return boost::none;
    }

    if (auto catalogCache = Grid::get(opCtx)->catalogCache()) {
        auto [cm, _] = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
        if (cm.hasRoutingTable()) {
            if (cm.isUnsplittable()) {
                return cm.getMinKeyShardIdWithSimpleCollation();
            } else {
                return boost::none;
            }
        } else {
            return cm.dbPrimary();
        }
    }
    return boost::none;
}
}  // namespace mongo
