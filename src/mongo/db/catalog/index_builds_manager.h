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

#pragma once

#include <map>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class Collection;
class OperationContext;
class ServiceContext;

enum IndexBuildRecoveryState { Building, Verifying, Committing };

/**
 * This is the interface through which to act on index builders. Index builder life times are
 * managed here, and all actions taken on index builders pass through this interface. Index builder
 * state is set up and then cleaned up by this class.
 */
class IndexBuildsManager {
    MONGO_DISALLOW_COPYING(IndexBuildsManager);

public:
    IndexBuildsManager() = default;
    ~IndexBuildsManager();

    /**
     * Sets up the index build state and registers it in the manager.
     */
    using OnInitFn = MultiIndexBlock::OnInitFn;
    Status setUpIndexBuild(OperationContext* opCtx,
                           Collection* collection,
                           const std::vector<BSONObj>& specs,
                           const UUID& buildUUID,
                           OnInitFn onInit);

    /**
     * Recovers the index build from its persisted state and sets it up to run again.
     *
     * Returns an enum reflecting the point up to which the build was recovered, so the caller knows
     * where to recommence.
     *
     * TODO: Not yet implemented.
     */
    StatusWith<IndexBuildRecoveryState> recoverIndexBuild(const NamespaceString& nss,
                                                          const UUID& buildUUID,
                                                          std::vector<std::string> indexNames);

    /**
     * Runs the scanning/insertion phase of the index build..
     *
     * TODO: Not yet implemented.
     */
    Status startBuildingIndex(const UUID& buildUUID);

    /**
     * Document inserts observed during the scanning/insertion phase of an index build are not
     * added but are instead stored in a temporary buffer until this function is invoked.
     */
    Status drainBackgroundWrites(const UUID& buildUUID);

    /**
     * Persists information in the index catalog entry to reflect the successful completion of the
     * scanning/insertion phase.
     *
     * TODO: Not yet implemented.
     */
    Status finishBuildingPhase(const UUID& buildUUID);

    /**
     * Runs the index constraint violation checking phase of the index build..
     *
     * TODO: Not yet implemented.
     */
    Status checkIndexConstraintViolations(const UUID& buildUUID);

    /**
     * Persists information in the index catalog entry that the index is ready for use, as well as
     * updating the in-memory index catalog entry for this index to ready.
     */
    using OnCreateEachFn = MultiIndexBlock::OnCreateEachFn;
    using OnCommitFn = MultiIndexBlock::OnCommitFn;
    Status commitIndexBuild(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const UUID& buildUUID,
                            OnCreateEachFn onCreateEachFn,
                            OnCommitFn onCommitFn);

    /**
     * Signals the index build to be aborted and returns without waiting for completion.
     *
     * Returns true if a build existed to be signaled, as opposed to having already finished and
     * been cleared away, or not having yet started..
     *
     * TODO: Not yet fully implemented. The MultiIndexBlock::abort function that is called is
     * not yet implemented.
     */
    bool abortIndexBuild(const UUID& buildUUID, const std::string& reason);

    /**
     * Signals the index build to be interrupted and returns without waiting for it to stop. Does
     * nothing if the index build has already been cleared away.
     *
     * Returns true if a build existed to be signaled, as opposed to having already finished and
     * been cleared away, or not having yet started..
     *
     * TODO: Not yet implemented.
     */
    bool interruptIndexBuild(const UUID& buildUUID, const std::string& reason);

    /**
     * Cleans up the index build state and unregisters it from the manager.
     */
    void tearDownIndexBuild(const UUID& buildUUID);

    /**
     * Returns true if the index build supports background writes while building an index. This is
     * true for the kHybrid and kBackground methods.
     */
    bool isBackgroundBuilding(const UUID& buildUUID);

    /**
     * Checks via invariant that the manager has no index builds presently.
     */
    void verifyNoIndexBuilds_forTestOnly();

private:
    /**
     * Creates and registers a new builder in the _builders map, mapped by the provided buildUUID.
     */
    void _registerIndexBuild(OperationContext* opCtx, Collection* collection, UUID buildUUID);

    /**
     * Unregisters the builder associcated with the given buildUUID from the _builders map.
     */
    void _unregisterIndexBuild(const UUID& buildUUID);

    /**
     * Returns a shared pointer to the builder. Invariants if the builder does not exist.
     */
    std::shared_ptr<MultiIndexBlock> _getBuilder(const UUID& buildUUID);

    // Protects the map data structures below.
    mutable stdx::mutex _mutex;

    // Map of index builders by build UUID. Allows access to the builders so that actions can be
    // taken on and information passed to and from index builds.
    std::map<UUID, std::shared_ptr<MultiIndexBlock>> _builders;
};

}  // namespace mongo
