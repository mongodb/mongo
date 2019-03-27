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

#include "mongo/db/repl_index_build_state.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/uuid.h"

namespace mongo {

class IndexBuildsManager;

/**
 * Tracks index builds for a particular collection. Provides access to index build information by
 * collection and index name. Can be used to act on all index builds for a collection, wait upon the
 * completion of all index builds, and provide collection level index build information.
 *
 * The owner of a CollectionIndexBuildsTracker instance must instantiate a mutex to use along with
 * the data structure to ensure it remains consistent across single or multiple function accesses.
 *
 * This is intended to only be used by the IndexBuildsCoordinator class.
 */
class CollectionIndexBuildsTracker {
    CollectionIndexBuildsTracker(const CollectionIndexBuildsTracker&) = delete;
    CollectionIndexBuildsTracker& operator=(const CollectionIndexBuildsTracker&) = delete;

public:
    CollectionIndexBuildsTracker() = default;
    ~CollectionIndexBuildsTracker();

    /**
     * Starts tracking the specified index build on the collection.
     */
    void addIndexBuild(WithLock, std::shared_ptr<ReplIndexBuildState> buildInfo);

    /**
     * Stops tracking the specified index build on the collection.
     */
    void removeIndexBuild(WithLock, std::shared_ptr<ReplIndexBuildState> buildInfo);

    std::shared_ptr<ReplIndexBuildState> getIndexBuildState(WithLock, StringData indexName) const;

    bool hasIndexBuildState(WithLock, StringData indexName) const;

    /**
     * Runs the provided function operation on all this collection's index builds
     *
     * 'func' returns void and must not throw. It should not have failure state to return.
     */
    void runOperationOnAllBuilds(
        WithLock,
        IndexBuildsManager* indexBuildsManager,
        std::function<void(WithLock,
                           IndexBuildsManager* indexBuildsManager,
                           std::shared_ptr<ReplIndexBuildState> replIndexBuildState,
                           const std::string& reason)> func,
        const std::string& reason) noexcept;

    /**
     * Note that this is the number of index builders, and that each index builder can be building
     * several indexes.
     */
    int getNumberOfIndexBuilds(WithLock) const;

    /**
     * Returns when no index builds remain on this collection.
     */
    void waitUntilNoIndexBuildsRemain(stdx::unique_lock<stdx::mutex>& lk);

private:
    // Maps of index build states on the collection, by build UUID and index name.
    stdx::unordered_map<UUID, std::shared_ptr<ReplIndexBuildState>, UUID::Hash>
        _buildStateByBuildUUID;
    stdx::unordered_map<std::string, std::shared_ptr<ReplIndexBuildState>> _buildStateByIndexName;

    // Condition variable that is signaled when there are no active index builds remaining on the
    // collection.
    stdx::condition_variable _noIndexBuildsRemainCondVar;
};

}  // namespace mongo
