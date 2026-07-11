// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {
/**
 * 'CommitQuorumOptions' is used to determine when a primary should commit an index build. When the
 * specified 'quorum' of replica set members is reached, then the primary proceeds to commit the
 * index. commitQuorum ensures secondaries are ready to commit the index as quickly as possible:
 * secondary replication will stall on receipt of a commitIndexBuild oplog entry until the
 * secondary's index build is complete and ready to be committed.
 *
 * The 'CommitQuorumOptions' has the same range of settings as the 'w' field from
 * 'WriteConcernOptions'. It can be set to an integer starting from 0 and up, or to a string. The
 * string option can be 'majority', 'votingMembers' or a replica set tag.
 */
class CommitQuorumOptions {
public:
    static const std::string_view kCommitQuorumField;  // = "commitQuorum"
    static const char kMajority[];                     // = "majority"
    static const char kVotingMembers[];                // = "votingMembers"

    static constexpr int kUninitializedNumNodes = -1;
    static constexpr int kPrimarySelfVote = 1;  // Primary just needs 1 vote (from itself).
    static const BSONObj Majority;              // = {"commitQuorum": "majority"}
    static const BSONObj VotingMembers;         // = {"commitQuorum": "votingMembers"}

    CommitQuorumOptions() {
        reset();
    }

    explicit CommitQuorumOptions(int numNodesOpts);

    explicit CommitQuorumOptions(const std::string& modeOpts);

    Status parse(const BSONElement& commitQuorumElement);

    /**
     * Returns an instance of CommitQuorumOptions from a BSONElement.
     *
     * uasserts() if the 'commitQuorumElement' cannot be deserialized.
     */
    static CommitQuorumOptions deserializerForIDL(const BSONElement& commitQuorumElement);

    void reset() {
        numNodes = kUninitializedNumNodes;
        mode = "";
    }

    bool isInitialized() const {
        return !(mode.empty() && numNodes == kUninitializedNumNodes);
    }

    inline bool operator==(const CommitQuorumOptions& rhs) const {
        return (numNodes == rhs.numNodes && mode == rhs.mode) ? true : false;
    }

    /**
     * Returns the BSON representation of this object.
     * E.g. {commitQuorum: "majority"}
     */
    BSONObj toBSON() const;

    /**
     * Appends the commitQuorum value (mode or numNodes) with the given field name "fieldName".
     */
    void appendToBuilder(std::string_view fieldName, BSONObjBuilder* builder) const;

    // The 'commitQuorum' parameter to define the required quorum for the index builds to commit.
    // The 'mode' represents the string format and takes precedence over the number format
    // 'numNodes'.
    int numNodes = kUninitializedNumNodes;
    std::string mode = "";
};

}  // namespace mongo
