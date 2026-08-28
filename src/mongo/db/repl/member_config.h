// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/member_config_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/db/repl/split_horizon/split_horizon.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class BSONObj;

namespace repl {

/**
 * Representation of the configuration information about a particular member of a replica set.
 */
class MemberConfig : private MemberConfigBase {
public:
    // Expose certain member functions used externally.
    using MemberConfigBase::getId;
    using MemberConfigBase::getPriorityPort;

    using MemberConfigBase::kArbiterOnlyFieldName;
    using MemberConfigBase::kBuildIndexesFieldName;
    using MemberConfigBase::kHiddenFieldName;
    using MemberConfigBase::kHorizonsFieldName;
    using MemberConfigBase::kHostFieldName;
    using MemberConfigBase::kIdFieldName;
    using MemberConfigBase::kNewlyAddedFieldName;
    using MemberConfigBase::kPriorityFieldName;
    using MemberConfigBase::kPriorityPortFieldName;
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
    const HostAndPort& getHostAndPort(
        std::string_view horizon = SplitHorizon::kDefaultHorizon) const {
        return _splitHorizon.getHostAndPort(horizon);
    }

    /**
     * Gets the host and priority port if the priority port is specified and the host and main
     * port if not. Always returns the value for the default horizon since this is only intended for
     * use by internal replication systems.
     */
    HostAndPort getHostAndPortPriority() const {
        if (getPriorityPort() &&
            !MONGO_unlikely(repl::disableReplicationUsageOfPriorityPort.load())) {
            return HostAndPort(getHostAndPort().host(), *getPriorityPort());
        }
        return getHostAndPort();
    }

    bool isUsingPriorityPort(const HostAndPort& hap) const {
        return getPriorityPort() == hap.port();
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
}  // namespace mongo
