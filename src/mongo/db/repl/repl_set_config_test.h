// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <algorithm>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace mongo {
namespace repl {

inline bool operator==(const MemberConfig& a, const MemberConfig& b) {
    // do tag comparisons
    for (MemberConfig::TagIterator itrA = a.tagsBegin(); itrA != a.tagsEnd(); ++itrA) {
        if (std::find(b.tagsBegin(), b.tagsEnd(), *itrA) == b.tagsEnd()) {
            return false;
        }
    }
    return a.getId() == b.getId() && a.getHostAndPort() == b.getHostAndPort() &&
        a.getPriorityPort() == b.getPriorityPort() && a.getPriority() == b.getPriority() &&
        a.getSecondaryDelay() == b.getSecondaryDelay() && a.isVoter() == b.isVoter() &&
        a.isArbiter() == b.isArbiter() && a.isNewlyAdded() == b.isNewlyAdded() &&
        a.isHidden() == b.isHidden() && a.shouldBuildIndexes() == b.shouldBuildIndexes() &&
        a.getNumTags() == b.getNumTags() && a.getHorizonMappings() == b.getHorizonMappings() &&
        a.getHorizonReverseHostMappings() == b.getHorizonReverseHostMappings();
}

inline bool operator==(const ReplSetConfig& a, const ReplSetConfig& b) {
    // compare WriteConcernModes
    std::vector<std::string> modeNames = a.getWriteConcernNames();
    for (std::vector<std::string>::iterator it = modeNames.begin(); it != modeNames.end(); it++) {
        ReplSetTagPattern patternA = a.findCustomWriteMode(*it).getValue();
        ReplSetTagPattern patternB = b.findCustomWriteMode(*it).getValue();
        for (ReplSetTagPattern::ConstraintIterator itrA = patternA.constraintsBegin();
             itrA != patternA.constraintsEnd();
             itrA++) {
            bool same = false;
            for (ReplSetTagPattern::ConstraintIterator itrB = patternB.constraintsBegin();
                 itrB != patternB.constraintsEnd();
                 itrB++) {
                if (itrA->getKeyIndex() == itrB->getKeyIndex() &&
                    itrA->getMinCount() == itrB->getMinCount()) {
                    same = true;
                    break;
                }
            }
            if (!same) {
                return false;
            }
        }
    }

    // compare the members
    for (ReplSetConfig::MemberIterator memA = a.membersBegin(); memA != a.membersEnd(); memA++) {
        bool same = false;
        for (ReplSetConfig::MemberIterator memB = b.membersBegin(); memB != b.membersEnd();
             memB++) {
            if (*memA == *memB) {
                same = true;
                break;
            }
        }
        if (!same) {
            return false;
        }
    }

    // simple comparisons
    return a.getReplSetName() == b.getReplSetName() &&
        a.getConfigVersion() == b.getConfigVersion() && a.getNumMembers() == b.getNumMembers() &&
        a.getHeartbeatInterval() == b.getHeartbeatInterval() &&
        a.getHeartbeatTimeoutPeriod() == b.getHeartbeatTimeoutPeriod() &&
        a.getElectionTimeoutPeriod() == b.getElectionTimeoutPeriod() &&
        a.isChainingAllowed() == b.isChainingAllowed() &&
        a.getConfigServer_deprecated() == b.getConfigServer_deprecated() &&
        a.getDefaultWriteConcern().w == b.getDefaultWriteConcern().w &&
        a.getProtocolVersion() == b.getProtocolVersion() &&
        a.getReplicaSetId() == b.getReplicaSetId();
}

}  // namespace repl
}  // namespace mongo
