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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
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
        a.getPriority() == b.getPriority() && a.getSecondaryDelay() == b.getSecondaryDelay() &&
        a.isVoter() == b.isVoter() && a.isArbiter() == b.isArbiter() &&
        a.isNewlyAdded() == b.isNewlyAdded() && a.isHidden() == b.isHidden() &&
        a.shouldBuildIndexes() == b.shouldBuildIndexes() && a.getNumTags() == b.getNumTags() &&
        a.getHorizonMappings() == b.getHorizonMappings() &&
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
