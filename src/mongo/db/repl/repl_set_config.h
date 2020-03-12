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

/**
 * Representation of the configuration information about a particular replica set.
 */
class ReplSetConfig {
public:
    typedef std::vector<MemberConfig>::const_iterator MemberIterator;

    static const std::string kConfigServerFieldName;
    static const std::string kVersionFieldName;
    static const std::string kTermFieldName;
    static constexpr char kMajorityWriteConcernModeName[] = "$majority";
    static constexpr char kAllWriteConcernModeName[] = "$all";
    static constexpr char kConfigMajorityWriteConcernModeName[] = "$configMajority";
    static constexpr char kConfigAllWriteConcernName[] = "$configAll";

    // If this field is present, a repair operation potentially modified replicated data. This
    // should never be included in a valid configuration document.
    static const std::string kRepairedFieldName;

    /**
     * Inline `kMaxMembers` to allow others (e.g, `WriteConcernOptions`) use
     * the constant without linking to `repl_set_config.cpp`.
     */
    inline static const size_t kMaxMembers = 50;
    static const size_t kMaxVotingMembers = 7;
    static const Milliseconds kInfiniteCatchUpTimeout;
    static const Milliseconds kCatchUpDisabled;
    static const Milliseconds kCatchUpTakeoverDisabled;

    static const Milliseconds kDefaultElectionTimeoutPeriod;
    static const Milliseconds kDefaultHeartbeatInterval;
    static const Seconds kDefaultHeartbeatTimeoutPeriod;
    static const Milliseconds kDefaultCatchUpTimeoutPeriod;
    static const bool kDefaultChainingAllowed;
    static const Milliseconds kDefaultCatchUpTakeoverDelay;

    /**
     * Initializes this ReplSetConfig from the contents of "cfg".
     * Sets _replicaSetId to "defaultReplicaSetId" if a replica set ID is not specified in "cfg".
     * If forceTerm is not boost::none, sets _term to the given term. Otherwise, parses term from
     * config BSON.
     */
    Status initialize(const BSONObj& cfg,
                      boost::optional<long long> forceTerm = boost::none,
                      OID defaultReplicaSetId = OID());

    /**
     * Same as the generic initialize() above except will default "configsvr" setting to the value
     * of serverGlobalParams.configsvr.
     * Sets _term to kInitialTerm.
     */
    Status initializeForInitiate(const BSONObj& cfg);

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
     * Checks if this configuration can satisfy the given write concern.
     *
     * Things that are taken into consideration include:
     * 1. If the set has enough data-bearing members.
     * 2. If the write concern mode exists.
     * 3. If there are enough members for the write concern mode specified.
     */
    Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions& writeConcern) const;

    /**
     * Gets and sets the version of this configuration.
     *
     * The version number sequences configurations of the replica set, so that
     * nodes may distinguish between "older" and "newer" configurations.
     */
    long long getConfigVersion() const {
        return _version;
    }

    void setConfigVersion(long long version) {
        _version = version;
    }

    /**
     * Gets and sets the term of this configuration.
     *
     * The configuration term is the term of the primary that originally created this configuration.
     * Configurations in a replica set are totally ordered by their term and configuration version.
     */
    long long getConfigTerm() const {
        return _term;
    }

    void setConfigTerm(long long term) {
        _term = term;
    }

    /**
     * Gets the (version, term) pair of this configuration.
     */
    ConfigVersionAndTerm getConfigVersionAndTerm() const {
        return ConfigVersionAndTerm(_version, _term);
    }

    /**
     * Gets the name (_id field value) of the replica set described by this configuration.
     */
    const std::string& getReplSetName() const {
        return _replSetName;
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
        return _members.size();
    }

    /**
     * Gets the number of data-bearing members in this configuration.
     */
    int getNumDataBearingMembers() const;

    /**
     * Gets a begin iterator over the MemberConfigs stored in this ReplSetConfig.
     */
    MemberIterator membersBegin() const {
        return _members.begin();
    }

    /**
     * Gets an end iterator over the MemberConfigs stored in this ReplSetConfig.
     */
    MemberIterator membersEnd() const {
        return _members.end();
    }

    const std::vector<MemberConfig>& members() const {
        return _members;
    }

    /**
     * Returns all voting members in this ReplSetConfig.
     */
    std::vector<MemberConfig> votingMembers() const {
        std::vector<MemberConfig> votingMembers;
        for (const MemberConfig& m : _members) {
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
    int findMemberIndexByConfigId(long long configId) const;

    /**
     * Gets the default write concern for the replica set described by this configuration.
     */
    const WriteConcernOptions& getDefaultWriteConcern() const {
        return _defaultWriteConcern;
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
        return _electionTimeoutPeriod;
    }

    /**
     * Gets the amount of time to wait for a response to hearbeats sent to other
     * nodes in the replica set.
     */
    Seconds getHeartbeatTimeoutPeriod() const {
        return _heartbeatTimeoutPeriod;
    }

    /**
     * Gets the amount of time to wait for a response to hearbeats sent to other
     * nodes in the replica set, as above, but returns a Milliseconds instead of
     * Seconds object.
     */
    Milliseconds getHeartbeatTimeoutPeriodMillis() const {
        return _heartbeatTimeoutPeriod;
    }

    /**
     * Gets the timeout to wait for a primary to catch up its oplog.
     */
    Milliseconds getCatchUpTimeoutPeriod() const {
        return _catchUpTimeoutPeriod;
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
        return _chainingAllowed;
    }

    /**
     * Returns whether all members of this replica set have hostname localhost.
     */
    bool isLocalHostAllowed() const;

    /**
     * Returns whether or not majority write concerns should implicitly journal, if j has not been
     * explicitly set.
     */
    bool getWriteConcernMajorityShouldJournal() const {
        return _writeConcernMajorityJournalDefault;
    }

    /**
     * Returns true if this replica set is for use as a config server replica set.
     */
    bool isConfigServer() const {
        return _configServer;
    }

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
     * Returns the "tags configuration" for this replicaset.
     *
     * NOTE(schwerin): Not clear if this should be used other than for reporting/debugging.
     */
    const ReplSetTagConfig& getTagConfig() const {
        return _tagConfig;
    }

    /**
     * Returns the config as a BSONObj.
     */
    BSONObj toBSON() const;

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
     * Gets the protocol version for this configuration.
     *
     * The protocol version number currently determines what election protocol is used by the
     * cluster; 1 is the default.
     */
    long long getProtocolVersion() const {
        return _protocolVersion;
    }

    /**
     * Returns true if this configuration contains a valid replica set ID.
     * This ID is set at creation and is used to disambiguate replica set configurations that may
     * have the same replica set name (_id field) but meant for different replica set instances.
     */
    bool hasReplicaSetId() const {
        return _replicaSetId.isSet();
    }

    /**
     * Returns replica set ID.
     */
    OID getReplicaSetId() const {
        return _replicaSetId;
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
        return _catchUpTakeoverDelay;
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
     * Sets the 'newlyAdded' field of the MemberConfig at memberIndex to the value passed in.
     */
    void setNewlyAddedFieldForMemberAtIndex(int memberIndex, bool newlyAdded);

private:
    /**
     * Parses the "settings" subdocument of a replica set configuration.
     */
    Status _parseSettingsSubdocument(const BSONObj& settings);

    /**
     * Calculates and stores the majority for electing a primary (_majorityVoteCount).
     */
    void _calculateMajorities();

    /**
     * Adds internal write concern modes to the getLastErrorModes list.
     */
    void _addInternalWriteConcernModes();

    /**
     * Populate _connectionString based on the contents of _members and _replSetName.
     */
    void _initializeConnectionString();

    /**
     * Sets replica set ID to 'defaultReplicaSetId' if forInitiate is false and 'cfg' does not
     * contain an ID.
     * Sets _term to kInitialTerm for initiate.
     * Sets _term to forceTerm if it is not boost::none. Otherwise, parses term from 'cfg'.
     */
    Status _initialize(const BSONObj& cfg,
                       bool forInitiate,
                       boost::optional<long long> forceTerm,
                       OID defaultReplicaSetId);

    bool _isInitialized = false;
    long long _version = 1;
    long long _term = OpTime::kUninitializedTerm;
    std::string _replSetName;
    std::vector<MemberConfig> _members;
    WriteConcernOptions _defaultWriteConcern;
    Milliseconds _electionTimeoutPeriod = kDefaultElectionTimeoutPeriod;
    Milliseconds _heartbeatInterval = kDefaultHeartbeatInterval;
    Seconds _heartbeatTimeoutPeriod = kDefaultHeartbeatTimeoutPeriod;
    Milliseconds _catchUpTimeoutPeriod = kDefaultCatchUpTimeoutPeriod;
    Milliseconds _catchUpTakeoverDelay = kDefaultCatchUpTakeoverDelay;
    bool _chainingAllowed = kDefaultChainingAllowed;
    bool _writeConcernMajorityJournalDefault = false;
    int _majorityVoteCount = 0;
    int _writableVotingMembersCount = 0;
    int _writeMajority = 0;
    int _totalVotingMembers = 0;
    ReplSetTagConfig _tagConfig;
    StringMap<ReplSetTagPattern> _customWriteConcernModes;
    long long _protocolVersion = 1;
    bool _configServer = false;
    OID _replicaSetId;
    ConnectionString _connectionString;
};


}  // namespace repl
}  // namespace mongo
