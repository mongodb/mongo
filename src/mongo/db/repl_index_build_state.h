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

#include <algorithm>
#include <list>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Tracks the cross replica set progress of a particular index build identified by a build UUID.
 *
 * This is intended to only be used by the IndexBuildsCoordinator class.
 *
 * TODO: pass in commit quorum setting and FCV to decide the twoPhaseIndexBuild setting.
 */
struct ReplIndexBuildState {
    ReplIndexBuildState(const UUID& indexBuildUUID,
                        const UUID& collUUID,
                        const std::vector<std::string> names,
                        const std::vector<BSONObj>& specs,
                        Promise<void> promise)
        : buildUUID(indexBuildUUID),
          collectionUUID(collUUID),
          indexNames(names),
          indexSpecs(specs) {
        promises.emplace_back(std::move(promise));

        // Verify that the given index names and index specs match.
        invariant(names.size() == specs.size());
        for (auto& spec : specs) {
            std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
            invariant(std::find(names.begin(), names.end(), name) != names.end());
        }
    }

    // Uniquely identifies this index build across replica set members.
    const UUID buildUUID;

    // Identifies the collection for which the index is being built. Collections can be renamed, so
    // the collection UUID is used to maintain correct association.
    const UUID collectionUUID;

    // The names of the indexes being built.
    const std::vector<std::string> indexNames;

    // The specs of the index(es) being built. Facilitates new callers joining an active index
    // build.
    const std::vector<BSONObj> indexSpecs;

    // Whether to do a two phase index build or a single phase index build like in v4.0. The FCV
    // at the start of the index build will determine this setting.
    bool twoPhaseIndexBuild = false;

    // Protects the state below.
    mutable stdx::mutex mutex;

    // The quorum required of commit ready replica set members before the index build will be
    // allowed to commit.
    WriteConcernOptions commitQuorum;

    // Whether or not the primary replica set member has signaled that it is okay to go ahead and
    // verify index constraint violations have gone away.
    bool prepareIndexBuild = false;

    // Tracks the members of the replica set that have finished building the index(es) and are ready
    // to commit the index(es).
    std::vector<HostAndPort> commitReadyMembers;

    // Communicates the final outcome of the index build to any callers waiting upon the associated
    // Future(s).
    std::vector<Promise<void>> promises;

    // There is a period of time where the index build is registered on the coordinator, but an
    // index builder does not yet exist. Since a signal cannot be set on the index builder at that
    // time, it must be saved here.
    bool aborted = false;
    std::string abortReason = "";

    // The coordinator for the index build will wait upon this when awaiting an external signal,
    // such as commit or commit readiness signals.
    stdx::condition_variable condVar;
};

}  // namespace mongo
