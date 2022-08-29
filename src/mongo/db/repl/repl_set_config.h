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
#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/repl_set_config_gen.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;

namespace repl {

/**
 * A structure that stores a ReplSetConfig (version, term) pair.
 *
 * This can be used to compare two ReplSetConfig objects to determine which is logically newer.
 */
class ConfigVersionAndTerm {
public:
    ConfigVersionAndTerm() : _version(0), _term(OpTime::kUninitializedTerm) {}
    ConfigVersionAndTerm(int version, long long term) : _version(version), _term(term) {}

    inline bool operator==(const ConfigVersionAndTerm& rhs) const {
        // If term of either item is uninitialized (-1), then we ignore terms entirely and only
        // compare versions.
        if (_term == OpTime::kUninitializedTerm || rhs._term == OpTime::kUninitializedTerm) {
            return _version == rhs._version;
        }
        // Compare term first, then the versions.
        return std::tie(_term, _version) == std::tie(rhs._term, rhs._version);
    }

    inline bool operator<(const ConfigVersionAndTerm& rhs) const {
        // If term of either item is uninitialized (-1), then we ignore terms entirely and only
        // compare versions. This allows force reconfigs, which set the config term to -1, to
        // override other configs by using a high config version.
        if (_term == OpTime::kUninitializedTerm || rhs._term == OpTime::kUninitializedTerm) {
            return _version < rhs._version;
        }
        // Compare term first, then the versions.
        return std::tie(_term, _version) < std::tie(rhs._term, rhs._version);
    }

    inline bool operator!=(const ConfigVersionAndTerm& rhs) const {
        return !(*this == rhs);
    }

    inline bool operator<=(const ConfigVersionAndTerm& rhs) const {
        return *this < rhs || *this == rhs;
    }

    inline bool operator>(const ConfigVersionAndTerm& rhs) const {
        return !(*this <= rhs);
    }

    inline bool operator>=(const ConfigVersionAndTerm& rhs) const {
        return !(*this < rhs);
    }

    std::string toString() const {
        return str::stream() << "{version: " << _version << ", term: " << _term << "}";
    };

    friend std::ostream& operator<<(std::ostream& out, const ConfigVersionAndTerm& cvt) {
        return out << cvt.toString();
    }

private:
    long long _version;
    long long _term;
};

class ReplSetConfig;
using ReplSetConfigPtr = std::shared_ptr<ReplSetConfig>;

/**
 * This class is used for mutating the ReplicaSetConfig.  Call ReplSetConfig::getMutable()
 * to get a mutable copy, mutate it, and use the ReplSetConfig(MutableReplSetConfig&&) constructor
 * to get a usable immutable config from it.
 */
class MutableReplSetConfig : public ReplSetConfigBase {
public:
    ReplSetConfigSettings& getMutableSettings() {
        invariant(ReplSetConfigBase::getSettings());
        // TODO(SERVER-47937): Get rid of the const_cast when the IDL supports that.
        return const_cast<ReplSetConfigSettings&>(*ReplSetConfigBase::getSettings());
    }

    /**
     * Adds 'newlyAdded=true' to the MemberConfig of the specified member.
     */
    void addNewlyAddedFieldForMember(MemberId memberId);

    /**
     * Removes the 'newlyAdded' field from the MemberConfig of the specified member.
     */
    void removeNewlyAddedFieldForMember(MemberId memberId);

    /**
     * Sets the member config's 'secondaryDelaySecs' field to the default value of 0.
     */
    void setSecondaryDelaySecsFieldDefault(MemberId memberId);

protected:
    MutableReplSetConfig() = default;

    /**
     * Returns a pointer to a mutable MemberConfig.
     */
    MemberConfig* _findMemberByID(MemberId id);

    ReplSetConfigPtr _recipientConfig;
};

/**
 * Representation of the configuration information about a particular replica set.
 */
class ReplSetConfig : private MutableReplSetConfig {
public:
    typedef std::vector<MemberConfig>::const_iterator MemberIterator;

    using ReplSetConfigBase::kConfigServerFieldName;
    using ReplSetConfigBase::kConfigTermFieldName;
    static constexpr char kMajorityWriteConcernModeName[] = "$majority";
    static constexpr char kVotingMembersWriteConcernModeName[] = "$votingMembers";
    static constexpr char kConfigMajorityWriteConcernModeName[] = "$configMajority";
    static constexpr char kConfigAllWriteConcernName[] = "$configAll";

    // If this field is present, a repair operation potentially modified replicated data. This
    // should never be included in a valid configuration document.
    using ReplSetConfigBase::kRepairedFieldName;

    /**
     * Inline `kMaxMembers` and `kMaxVotingMembers` to allow others (e.g, `WriteConcernOptions`) use
     * the constant without linking to `repl_set_config.cpp`.
     */
    inline static const size_t kMaxMembers = 50;
    inline static const size_t kMaxVotingMembers = 7;
    static const Milliseconds kInfiniteCatchUpTimeout;
    static const Milliseconds kCatchUpDisabled;
    static const Milliseconds kCatchUpTakeoverDisabled;

    static const Milliseconds kDefaultElectionTimeoutPeriod;
    static const Milliseconds kDefaultHeartbeatInterval;
    static const Seconds kDefaultHeartbeatTimeoutPeriod;
    static const Milliseconds kDefaultCatchUpTimeoutPeriod;
    static const bool kDefaultChainingAllowed;
    static const Milliseconds kDefaultCatchUpTakeoverDelay;

    // Methods inherited from the base IDL class.  Do not include any setters here.
    using ReplSetConfigBase::getConfigServer;
    using ReplSetConfigBase::getConfigTerm;
    using ReplSetConfigBase::getConfigVersion;
    using ReplSetConfigBase::getProtocolVersion;
    using ReplSetConfigBase::getReplSetName;
    using ReplSetConfigBase::getWriteConcernMajorityShouldJournal;
    using ReplSetConfigBase::serialize;

    /**
     * Constructor used for converting a mutable config to an immutable one.
     */
    explicit ReplSetConfig(MutableReplSetConfig&& base);

    ReplSetConfig() {
        // This is not defaultable in the IDL.
        // SERVER-47938 would make it possible to be defaulted.

        setSettings(ReplSetConfigSettings());
        _setRequiredFields();
    }
    /**
     * Initializes a new ReplSetConfig from the contents of "cfg".
     * Sets replicaSetId to "defaultReplicaSetId" if a replica set ID is not specified in "cfg";
     * If forceTerm is not boost::none, sets _term to the given term. Otherwise, uses term from
     * config BSON.
     *
     * Parse errors are reported via exceptions.
     */
    static ReplSetConfig parse(const BSONObj& cfg,
                               boost::optional<long long> forceTerm = boost::none,
                               OID defaultReplicaSetId = OID());

    /**
     * Same as the generic parse() above except will default "configsvr" setting to the value
     * of serverGlobalParams.configsvr.
     * Sets term to kInitialTerm.
     * Sets replicaSetId to "newReplicaSetId", which must be set.
     */
    static ReplSetConfig parseForInitiate(const BSONObj& cfg, OID newReplicaSetId);

    /**
     * Override ReplSetConfigBase::toBSON to conditionally include the recipient config.
     */
    BSONObj toBSON() const;

    /**
     * Returns true if this object has been successfully initialized or copied from
     * an initialized object.
     */
    bool isInitialized() const {
        return _isInitialized;
    }

    /**
     * Performs basic consistency checks on the replica set configuration.
     */
    Status validate() const;

    /**
     * Performs basic consistency checks on the replica set configuration, but does not fail on
     * IP addresses in split horizon configuration
     */
    Status validateAllowingSplitHorizonIP() const;

    /**
     * Checks if this configuration can satisfy the given write concern.
     *
     * Things that are taken into consideration include:
     * 1. If the set has enough data-bearing members.
     * 2. If the write concern mode exists.
     * 3. If there are enough members for the write concern mode specified.
     */
    Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions& writeConcern) const;

    /**
     * Gets the (version, term) pair of this configuration.
     */
    ConfigVersionAndTerm getConfigVersionAndTerm() const {
        return ConfigVersionAndTerm(getConfigVersion(), getConfigTerm());
    }

    /**
     * Gets the connection string that can be used to talk to this replica set.
     */
    ConnectionString getConnectionString() const {
        return _connectionString;
    }

    /**
     * Gets the number of members in this configuration.
     */
    int getNumMembers() const {
        return getMembers().size();
    }

    /**
     * Gets the number of data-bearing members in this configuration.
     */
    int getNumDataBearingMembers() const;

    /**
     * Gets a begin iterator over the MemberConfigs stored in this ReplSetConfig.
     */
    MemberIterator membersBegin() const {
        return getMembers().begin();
    }

    /**
     * Gets an end iterator over the MemberConfigs stored in this ReplSetConfig.
     */
    MemberIterator membersEnd() const {
        return getMembers().end();
    }

    const std::vector<MemberConfig>& members() const {
        return getMembers();
    }

    /**
     * Returns all voting members in this ReplSetConfig.
     */
    std::vector<MemberConfig> votingMembers() const {
        std::vector<MemberConfig> votingMembers;
        for (const MemberConfig& m : getMembers()) {
            if (m.getNumVotes() > 0) {
                votingMembers.push_back(m);
            }
        }
        return votingMembers;
    };

    /**
     * Access a MemberConfig element by index.
     */
    const MemberConfig& getMemberAt(size_t i) const;

    /**
     * Returns a pointer to the MemberConfig corresponding to the member with the given _id in
     * the config, or NULL if there is no member with that ID.
     */
    const MemberConfig* findMemberByID(int id) const;

    /**
     * Returns a pointer to the MemberConfig corresponding to the member with the given
     * HostAndPort in the config, or NULL if there is no member with that address.
     */
    const MemberConfig* findMemberByHostAndPort(const HostAndPort& hap) const;

    /**
     * Returns a MemberConfig index position corresponding to the member with the given
     * HostAndPort in the config, or -1 if there is no member with that address.
     */
    int findMemberIndexByHostAndPort(const HostAndPort& hap) const;

    /**
     * Returns a MemberConfig index position corresponding to the member with the given
     * _id in the config, or -1 if there is no member with that address.
     */
    int findMemberIndexByConfigId(int configId) const;

    /**
     * Gets the default write concern for the replica set described by this configuration.
     */
    const WriteConcernOptions& getDefaultWriteConcern() const {
        return getSettings()->getDefaultWriteConcern();
    }

    /**
     * Interval between the time the last heartbeat from a node was received successfully, or
     * the time when we gave up retrying, and when the next heartbeat should be sent to a target.
     * Returns default heartbeat interval if this configuration is not initialized.
     */
    Milliseconds getHeartbeatInterval() const;

    /**
     * Gets the timeout for determining when the current PRIMARY is dead, which triggers a node to
     * run for election.
     */
    Milliseconds getElectionTimeoutPeriod() const {
        return Milliseconds(getSettings()->getElectionTimeoutMillis());
    }

    /**
     * Gets the amount of time to wait for a response to hearbeats sent to other
     * nodes in the replica set.
     */
    Seconds getHeartbeatTimeoutPeriod() const {
        return Seconds(getSettings()->getHeartbeatTimeoutSecs());
    }

    /**
     * Gets the amount of time to wait for a response to hearbeats sent to other
     * nodes in the replica set, as above, but returns a Milliseconds instead of
     * Seconds object.
     */
    Milliseconds getHeartbeatTimeoutPeriodMillis() const {
        return duration_cast<Milliseconds>(getHeartbeatTimeoutPeriod());
    }

    /**
     * Gets the timeout to wait for a primary to catch up its oplog.
     */
    Milliseconds getCatchUpTimeoutPeriod() const {
        return Milliseconds(getSettings()->getCatchUpTimeoutMillis());
    }

    /**
     * Gets the number of votes required to win an election.
     */
    int getMajorityVoteCount() const {
        return _majorityVoteCount;
    }

    /**
     * Gets the number of voters.
     */
    int getTotalVotingMembers() const {
        return _totalVotingMembers;
    }

    /**
     * Returns true if automatic (not explicitly set) chaining is allowed.
     */
    bool isChainingAllowed() const {
        return getSettings()->getChainingAllowed();
    }

    /**
     * Returns whether all members of this replica set have hostname localhost.
     */
    bool isLocalHostAllowed() const;

    /**
     * Returns a ReplSetTag with the given "key" and "value", or an invalid
     * tag if the configuration describes no such tag.
     */
    ReplSetTag findTag(StringData key, StringData value) const;

    /**
     * Returns the pattern corresponding to "patternName" in this configuration.
     * If "patternName" is not a valid pattern in this configuration, returns
     * ErrorCodes::NoSuchKey.
     */
    StatusWith<ReplSetTagPattern> findCustomWriteMode(StringData patternName) const;

    /**
     * Returns a pattern constructed from a raw set of tags provided as the `w` value
     * of a write concern.
     *
     * @returns `ErrorCodes::NoSuchKey` if a tag was provided which is not found in
     * the local tag config.
     */
    StatusWith<ReplSetTagPattern> makeCustomWriteMode(const WTags& wTags) const;

    /**
     * Returns the "tags configuration" for this replicaset.
     *
     * NOTE(schwerin): Not clear if this should be used other than for reporting/debugging.
     */
    const ReplSetTagConfig& getTagConfig() const {
        return _tagConfig;
    }

    /**
     * Returns the config as a BSONObj. Omits 'newlyAdded' fields.
     */
    BSONObj toBSONWithoutNewlyAdded() const;

    /**
     * Returns a vector of strings which are the names of the WriteConcernModes.
     * Currently used in unit tests to compare two configs.
     */
    std::vector<std::string> getWriteConcernNames() const;

    /**
     *  Returns the number of voting data-bearing members.
     */
    int getWritableVotingMembersCount() const {
        return _writableVotingMembersCount;
    }

    /**
     * Returns the number of voting data-bearing members that must acknowledge a write
     * in order to satisfy a write concern of {w: "majority"}.
     */
    int getWriteMajority() const {
        return _writeMajority;
    }

    /**
     * Returns true if this configuration contains a valid replica set ID.
     * This ID is set at creation and is used to disambiguate replica set configurations that may
     * have the same replica set name (_id field) but meant for different replica set instances.
     */
    bool hasReplicaSetId() const {
        return getSettings()->getReplicaSetId() != boost::none;
    }

    /**
     * Returns replica set ID.
     */
    OID getReplicaSetId() const {
        return getSettings()->getReplicaSetId() ? *getSettings()->getReplicaSetId() : OID();
    }

    /**
     * Returns the duration to wait before running for election when this node (indicated by
     * "memberIdx") sees that it has higher priority than the current primary.
     */
    Milliseconds getPriorityTakeoverDelay(int memberIdx) const;

    /**
     * Returns the duration to wait before running for election when this node
     * sees that it is more caught up than the current primary.
     */
    Milliseconds getCatchUpTakeoverDelay() const {
        return Milliseconds(getSettings()->getCatchUpTakeoverDelayMillis());
    }

    /**
     * Return the number of members with a priority greater than "priority".
     */
    int calculatePriorityRank(double priority) const;

    /**
     * Returns true if this replica set has at least one arbiter.
     */
    bool containsArbiter() const;

    /**
     * Returns a mutable (but not directly usable) copy of the config.
     */
    MutableReplSetConfig getMutable() const;

    /**
     * Returns true if implicit default write concern should be majority.
     */
    bool isImplicitDefaultWriteConcernMajority() const;

    /**
     * Returns true if the config consists of a Primary-Secondary-Arbiter (PSA) architecture.
     */
    bool isPSASet() const {
        return getNumMembers() == 3 && getNumDataBearingMembers() == 2;
    }

    /**
     * Returns true if the getLastErrorDefaults has been customized.
     */
    bool containsCustomizedGetLastErrorDefaults() const;

    /**
     * Returns Status::OK if write concern is valid for this config, or appropriate status
     * otherwise.
     */
    Status validateWriteConcern(const WriteConcernOptions& writeConcern) const;

    /**
     * Returns true if this config is a split config, which is determined by checking if it contains
     * a recipient config for a shard split operation.
     */
    bool isSplitConfig() const;

    /**
     * Returns the config for the recipient during a tenant split operation, if it exists.
     */
    ReplSetConfigPtr getRecipientConfig() const;

    /**
     * Compares the write concern modes with another config and returns 'true' if they are
     * identical.
     */
    bool areWriteConcernModesTheSame(ReplSetConfig* otherConfig) const;

private:
    /**
     * Sets replica set ID to 'defaultReplicaSetId' if 'cfg' does not contain an ID.
     * Sets term to kInitialTerm for initiate.
     * Sets term to forceTerm if it is not boost::none. Otherwise, parses term from 'cfg'.
     */
    ReplSetConfig(const BSONObj& cfg,
                  bool forInitiate,
                  boost::optional<long long> forceTerm,
                  OID defaultReplicaSetId);

    /**
     * Calculates and stores the majority for electing a primary (_majorityVoteCount).
     */
    void _calculateMajorities();

    /**
     * Adds internal write concern modes to the getLastErrorModes list.
     */
    void _addInternalWriteConcernModes();

    /**
     * Populate _connectionString based on the contents of members and replSetName.
     */
    void _initializeConnectionString();

    /**
     * Sets the required fields of the IDL object.
     */
    void _setRequiredFields();

    /**
     * Performs basic consistency checks on the replica set configuration.
     */
    Status _validate(bool allowSplitHorizonIP) const;

    /**
     * Common code used by constructors
     */
    Status _initialize(bool forInitiate,
                       boost::optional<long long> forceTerm,
                       OID defaultReplicaSetId);

    bool _isInitialized = false;
    int _majorityVoteCount = 0;
    int _writableVotingMembersCount = 0;
    int _writeMajority = 0;
    int _totalVotingMembers = 0;
    ReplSetTagConfig _tagConfig;
    StringMap<ReplSetTagPattern> _customWriteConcernModes;
    ConnectionString _connectionString;
};

}  // namespace repl
}  // namespace mongo
