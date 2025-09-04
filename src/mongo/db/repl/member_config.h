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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/member_config_gen.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/db/repl/split_horizon/split_horizon.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace MONGO_MOD_PUB mongo {

class BSONObj;

namespace repl {

/**
 * Representation of the configuration information about a particular member of a replica set.
 */
class MemberConfig : private MemberConfigBase {
public:
    // Expose certain member functions used externally.
    using MemberConfigBase::getId;

    using MemberConfigBase::kArbiterOnlyFieldName;
    using MemberConfigBase::kBuildIndexesFieldName;
    using MemberConfigBase::kHiddenFieldName;
    using MemberConfigBase::kHorizonsFieldName;
    using MemberConfigBase::kHostFieldName;
    using MemberConfigBase::kIdFieldName;
    using MemberConfigBase::kNewlyAddedFieldName;
    using MemberConfigBase::kPriorityFieldName;
    using MemberConfigBase::kSecondaryDelaySecsFieldName;
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
     * Creates a MemberConfig from a BSON object.  Call "addTagInfo", below, afterwards to
     * finish initializing.
     */
    static MemberConfig parseFromBSON(const BSONObj& mcfg);

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
     * Gets the horizon name for which the parameters (captured during the first `hello`)
     * correspond.
     */
    std::string determineHorizon(const SplitHorizon::Parameters& params) const {
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
     * Gets this member's base priority, without considering the 'newlyAdded' field.
     */
    double getBasePriority() const {
        return MemberConfigBase::getPriority();
    }

    /**
     * Gets the amount of time behind the primary that this member will atempt to
     * remain.  Zero seconds means stay as caught up as possible.
     */
    Seconds getSecondaryDelay() const {
        return getSecondaryDelaySecs() ? Seconds(getSecondaryDelaySecs().get()) : Seconds(0);
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
     * Returns the number of votes the member has, without considering the 'newlyAdded' field.
     */
    int getBaseNumVotes() const {
        return (getVotes() == 0) ? 0 : 1;
    }

    /**
     * Returns true if this member is an arbiter (is not data-bearing).
     */
    bool isArbiter() const {
        return getArbiterOnly();
    }

    /**
     * Returns true if this member has the field 'secondaryDelaySecs'.
     */
    bool hasSecondaryDelaySecs() const {
        return getSecondaryDelaySecs().has_value();
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
     * Returns true if this member is hidden (not reported by "hello", not electable).
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
        // All valid MemberConfig objects should have at least one tag, kInternalAllTagName if
        // nothing else.  So if we're accessing an empty _tags array we're using a MemberConfig
        // from a MutableReplSetConfig, which is invalid.
        invariant(!_tags.empty());
        return _tags.size();
    }

    /**
     * Returns true if this MemberConfig has any non-internal tags.
     */
    bool hasTags() const;

    /**
     * Gets a begin iterator over the tags for this member.
     */
    TagIterator tagsBegin() const {
        invariant(!_tags.empty());
        return _tags.begin();
    }

    /**
     * Gets an end iterator over the tags for this member.
     */
    TagIterator tagsEnd() const {
        invariant(!_tags.empty());
        return _tags.end();
    }

    /**
     * Returns true if this represents the configuration of an electable member.
     */
    bool isElectable() const {
        return !isArbiter() && getPriority() > 0;
    }

    /**
     * Returns the member config as a BSONObj.
     */
    BSONObj toBSON(bool omitNewlyAddedField = false) const;

    /*
     * Adds the tag info for this member to the tagConfig; to be used after an IDL parse.
     */
    void addTagInfo(ReplSetTagConfig* tagConfig);

private:
    // Allow MutableReplSetConfig to modify the newlyAdded field.
    friend class MutableReplSetConfig;

    friend void setNewlyAdded_forTest(MemberConfig*, boost::optional<bool>);

    /**
     * Constructor used by IDL; does not set up tags because we cannot pass TagConfig through IDL.
     */
    MemberConfig(const BSONObj& mcfg);

    const HostAndPort& _host() const {
        return getHostAndPort(SplitHorizon::kDefaultHorizon);
    }

    /**
     * Modifiers which potentially affect tags.  Calling them clears the tags array, which
     * will be rebuilt when addTagInfo is called.  Accessing a cleared tags array is not allowed
     * and is enforced by invariant.
     */
    void setNewlyAdded(boost::optional<bool> newlyAdded);
    void setArbiterOnly(bool arbiterOnly);
    void setVotes(int64_t votes);
    void setPriority(double priority);

    std::vector<ReplSetTag> _tags;  // tagging for data center, rack, etc.
    SplitHorizon _splitHorizon;
};

}  // namespace repl
}  // namespace MONGO_MOD_PUB mongo
