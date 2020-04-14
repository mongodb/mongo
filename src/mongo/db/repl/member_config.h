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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/repl_set_config_gen.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/db/repl/split_horizon.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;

namespace repl {

/**
 * Representation of the configuration information about a particular member of a replica set.
 */
class MemberConfig : private MemberConfigBase {
public:
    // Expose certain member functions used externally.
    using MemberConfigBase::getId;
    using MemberConfigBase::setNewlyAdded;

    using MemberConfigBase::kArbiterOnlyFieldName;
    using MemberConfigBase::kBuildIndexesFieldName;
    using MemberConfigBase::kHiddenFieldName;
    using MemberConfigBase::kHorizonsFieldName;
    using MemberConfigBase::kHostFieldName;
    using MemberConfigBase::kIdFieldName;
    using MemberConfigBase::kNewlyAddedFieldName;
    using MemberConfigBase::kPriorityFieldName;
    using MemberConfigBase::kSlaveDelaySecsFieldName;
    using MemberConfigBase::kTagsFieldName;
    using MemberConfigBase::kVotesFieldName;

    typedef std::vector<ReplSetTag>::const_iterator TagIterator;

    static const std::string kInternalVoterTagName;
    static const std::string kInternalElectableTagName;
    static const std::string kInternalAllTagName;
    static const std::string kConfigAllTagName;
    static const std::string kConfigVoterTagName;

    /**
     * Construct a MemberConfig from the contents of "mcfg".
     *
     * If "mcfg" describes any tags, builds ReplSetTags for this
     * configuration using "tagConfig" as the tag's namespace. This may
     * have the effect of altering "tagConfig" when "mcfg" describes a
     * tag not previously added to "tagConfig".
     */
    MemberConfig(const BSONObj& mcfg, ReplSetTagConfig* tagConfig);

    /**
     * Gets the canonical name of this member, by which other members and clients
     * will contact it.
     */
    const HostAndPort& getHostAndPort(StringData horizon = SplitHorizon::kDefaultHorizon) const {
        return _splitHorizon.getHostAndPort(horizon);
    }

    /**
     * Gets the mapping of horizon names to `HostAndPort` for this replica set member.
     */
    const auto& getHorizonMappings() const {
        return _splitHorizon.getForwardMappings();
    }

    /**
     * Gets the mapping of host names (not `HostAndPort`) to horizon names for this replica set
     * member.
     */
    const auto& getHorizonReverseHostMappings() const {
        return _splitHorizon.getReverseHostMappings();
    }

    /**
     * Gets the horizon name for which the parameters (captured during the first `isMaster`)
     * correspond.
     */
    StringData determineHorizon(const SplitHorizon::Parameters& params) const {
        return _splitHorizon.determineHorizon(params);
    }

    /**
     * Gets this member's effective priority. Higher means more likely to be elected
     * primary. If the node is newly added, it has an effective priority of 0.0.
     */
    double getPriority() const {
        return isNewlyAdded() ? 0.0 : MemberConfigBase::getPriority();
    }

    /**
     * Gets the amount of time behind the primary that this member will atempt to
     * remain.  Zero seconds means stay as caught up as possible.
     */
    Seconds getSlaveDelay() const {
        return Seconds(getSlaveDelaySecs());
    }

    /**
     * Returns true if this member may vote in elections. If the node is newly added, it should be
     * treated as a non-voting node.
     */
    bool isVoter() const {
        return (getVotes() != 0 && !isNewlyAdded());
    }

    /**
     * Returns the number of votes the member has.
     */
    int getNumVotes() const {
        return isVoter() ? 1 : 0;
    }

    /**
     * Returns true if this member is an arbiter (is not data-bearing).
     */
    bool isArbiter() const {
        return getArbiterOnly();
    }

    /**
     * Returns true if this member is newly added from reconfig. This indicates that this node
     * should be treated as non-voting.
     */
    bool isNewlyAdded() const {
        if (getNewlyAdded()) {
            invariant(getNewlyAdded().get());
            return true;
        }
        return false;
    }

    /**
     * Returns true if this member is hidden (not reported by isMaster, not electable).
     */
    bool isHidden() const {
        return getHidden();
    }

    /**
     * Returns true if this member should build secondary indexes.
     */
    bool shouldBuildIndexes() const {
        return getBuildIndexes();
    }

    /**
     * Gets the number of replica set tags, including internal '$' tags, for this member.
     */
    size_t getNumTags() const {
        return _tags.size();
    }

    /**
     * Returns true if this MemberConfig has any non-internal tags, using "tagConfig" to
     * determine the internal property of the tags.
     */
    bool hasTags(const ReplSetTagConfig& tagConfig) const;

    /**
     * Gets a begin iterator over the tags for this member.
     */
    TagIterator tagsBegin() const {
        return _tags.begin();
    }

    /**
     * Gets an end iterator over the tags for this member.
     */
    TagIterator tagsEnd() const {
        return _tags.end();
    }

    /**
     * Returns true if this represents the configuration of an electable member.
     */
    bool isElectable() const {
        return !isArbiter() && getPriority() > 0;
    }

    /**
     * Returns the member config as a BSONObj, using "tagConfig" to generate the tag subdoc.
     */
    BSONObj toBSON(const ReplSetTagConfig& tagConfig, bool omitNewlyAddedField) const;

private:
    const HostAndPort& _host() const {
        return getHostAndPort(SplitHorizon::kDefaultHorizon);
    }

    std::vector<ReplSetTag> _tags;  // tagging for data center, rack, etc.
    SplitHorizon _splitHorizon;
};

}  // namespace repl
}  // namespace mongo
