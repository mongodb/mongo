/**
 *    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/replica_set_tag.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

namespace mongo {

    class BSONObj;

namespace repl {

    /**
     * Representation of the configuration information about a particular replica set.
     */
    class ReplicaSetConfig {
    public:
        typedef std::vector<MemberConfig>::const_iterator MemberIterator;

        static const std::string kIdFieldName;
        static const std::string kVersionFieldName;
        static const std::string kMembersFieldName;
        static const std::string kSettingsFieldName;

        static const size_t kMaxMembers = 12;
        static const size_t kMaxVotingMembers = 7;
        static const Seconds kDefaultHeartbeatTimeoutPeriod;

        ReplicaSetConfig();
        std::string asBson() { return ""; }
        /**
         * Initializes this ReplicaSetConfig from the contents of "cfg".
         */
        Status initialize(const BSONObj& cfg);

        /**
         * Returns true if this object has been successfully initialized or copied from
         * an initialized object.
         */
        bool isInitialized() const { return _isInitialized; }

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
         * Gets the version of this configuration.
         *
         * The version number sequences configurations of the replica set, so that
         * nodes may distinguish between "older" and "newer" configurations.
         */
        long long getConfigVersion() const { return _version; }

        /**
         * Gets the name (_id field value) of the replica set described by this configuration.
         */
        const std::string& getReplSetName() const { return _replSetName; }

        /**
         * Gets the number of members in this configuration.
         */
        int getNumMembers() const { return _members.size(); }

        /**
         * Gets a begin iterator over the MemberConfigs stored in this ReplicaSetConfig.
         */
        MemberIterator membersBegin() const { return _members.begin(); }

        /**
         * Gets an end iterator over the MemberConfigs stored in this ReplicaSetConfig.
         */
        MemberIterator membersEnd() const { return _members.end(); }

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
         * Gets the default write concern for the replica set described by this configuration.
         */
        const WriteConcernOptions& getDefaultWriteConcern() const { return _defaultWriteConcern; }

        /**
         * Gets the amount of time to wait for a response to hearbeats sent to other
         * nodes in the replica set.
         */
        Seconds getHeartbeatTimeoutPeriod() const { return _heartbeatTimeoutPeriod; }

        /**
         * Gets the amount of time to wait for a response to hearbeats sent to other
         * nodes in the replica set, as above, but returns a Milliseconds instead of
         * Seconds object.
         */
        Milliseconds getHeartbeatTimeoutPeriodMillis() const {
            return Milliseconds(_heartbeatTimeoutPeriod.total_milliseconds());
        }

        /**
         * Gets the number of nodes that constitutes a "majority" in this replica set,
         * for purposes of replicating data.
         */
        int getMajorityNumber() const { return _majorityNumber; }

        /**
         * Gets the number of votes required to win an election.
         */
        int getMajorityVoteCount() const { return _majorityVoteCount; }

        /**
         * Returns true if automatic (not explicitly set) chaining is allowed.
         */
        bool isChainingAllowed() const { return _chainingAllowed; }

        /**
         * Returns a ReplicaSetTag with the given "key" and "value", or an invalid
         * tag if the configuration describes no such tag.
         */
        ReplicaSetTag findTag(const StringData& key, const StringData& value) const;

        /**
         * Returns the pattern corresponding to "patternName" in this configuration.
         * If "patternName" is not a valid pattern in this configuration, returns
         * ErrorCodes::NoSuchKey.
         */
        StatusWith<ReplicaSetTagPattern> findCustomWriteMode(const StringData& patternName) const;

        /**
         * Returns the "tags configuration" for this replicaset.
         *
         * NOTE(schwerin): Not clear if this should be used other than for reporting/debugging.
         */
        const ReplicaSetTagConfig& getTagConfig() const { return _tagConfig; }

        /**
         * Returns the config as a BSONObj.
         */
        BSONObj toBSON() const;

        /**
         * Returns a vector of strings which are the names of the WriteConcernModes.
         * Currently used in unit tests to compare two configs.
         */
         std::vector<std::string> getWriteConcernNames() const;

    private:
        /**
         * Parses the "settings" subdocument of a replica set configuration.
         */
        Status _parseSettingsSubdocument(const BSONObj& settings);

        /**
         * Calculates and stores the majorities for replicating data (_majorityNumber) and for
         * electing a primary (_majorityVoteCount).
         */
        void _calculateMajorities();

        bool _isInitialized;
        long long _version;
        std::string _replSetName;
        std::vector<MemberConfig> _members;
        WriteConcernOptions _defaultWriteConcern;
        Seconds _heartbeatTimeoutPeriod;
        bool _chainingAllowed;
        int _majorityNumber;
        int _majorityVoteCount;
        ReplicaSetTagConfig _tagConfig;
        StringMap<ReplicaSetTagPattern> _customWriteConcernModes;
    };


}  // namespace repl
}  // namespace mongo
