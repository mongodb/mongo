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

#include "mongo/platform/basic.h"

#include "mongo/db/repl_index_build_state.h"

namespace mongo {

namespace {

/**
 * Parses index specs to generate list of index names for ReplIndexBuildState initialization.
 */
std::vector<std::string> extractIndexNames(const std::vector<BSONObj>& specs) {
    std::vector<std::string> indexNames;
    for (const auto& spec : specs) {
        std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
        invariant(!name.empty(),
                  str::stream() << "Bad spec passed into ReplIndexBuildState constructor, missing '"
                                << IndexDescriptor::kIndexNameFieldName << "' field: " << spec);
        indexNames.push_back(name);
    }
    return indexNames;
}

/**
 * Returns true if the requested IndexBuildState transition is allowed.
 */
bool checkIfValidTransition(IndexBuildState::StateFlag currentState,
                            IndexBuildState::StateFlag newState) {
    if ((currentState == IndexBuildState::StateFlag::kSetup &&
         newState == IndexBuildState::StateFlag::kInProgress) ||
        (currentState == IndexBuildState::StateFlag::kInProgress &&
         newState != IndexBuildState::StateFlag::kSetup) ||
        (currentState == IndexBuildState::StateFlag::kPrepareCommit &&
         newState == IndexBuildState::StateFlag::kCommitted)) {
        return true;
    }
    return false;
}

}  // namespace

void IndexBuildState::setState(StateFlag state,
                               bool skipCheck,
                               boost::optional<Timestamp> timestamp,
                               boost::optional<std::string> abortReason) {
    if (!skipCheck) {
        invariant(checkIfValidTransition(_state, state),
                  str::stream() << "current state :" << toString(_state)
                                << ", new state: " << toString(state));
    }
    _state = state;
    if (timestamp)
        _timestamp = timestamp;
    if (abortReason) {
        invariant(_state == kAborted);
        _abortReason = abortReason;
    }
}

ReplIndexBuildState::ReplIndexBuildState(const UUID& indexBuildUUID,
                                         const UUID& collUUID,
                                         const std::string& dbName,
                                         const std::vector<BSONObj>& specs,
                                         IndexBuildProtocol protocol)
    : buildUUID(indexBuildUUID),
      collectionUUID(collUUID),
      dbName(dbName),
      indexNames(extractIndexNames(specs)),
      indexSpecs(specs),
      protocol(protocol) {
    waitForNextAction = std::make_unique<SharedPromise<IndexBuildAction>>();
    if (protocol == IndexBuildProtocol::kTwoPhase)
        commitQuorumLock.emplace(indexBuildUUID.toString());
}

bool ReplIndexBuildState::isResumable() const {
    stdx::unique_lock<Latch> lk(mutex);
    return !_lastOpTimeBeforeInterceptors.isNull();
}

repl::OpTime ReplIndexBuildState::getLastOpTimeBeforeInterceptors() const {
    stdx::unique_lock<Latch> lk(mutex);
    return _lastOpTimeBeforeInterceptors;
}

void ReplIndexBuildState::setLastOpTimeBeforeInterceptors(repl::OpTime opTime) {
    stdx::unique_lock<Latch> lk(mutex);
    _lastOpTimeBeforeInterceptors = std::move(opTime);
}

void ReplIndexBuildState::clearLastOpTimeBeforeInterceptors() {
    stdx::unique_lock<Latch> lk(mutex);
    _lastOpTimeBeforeInterceptors = {};
}

}  // namespace mongo
