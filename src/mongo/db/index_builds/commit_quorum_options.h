/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <string>

namespace mongo {

class Status;

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
    static const StringData kCommitQuorumField;  // = "commitQuorum"
    static const char kMajority[];               // = "majority"
    static const char kVotingMembers[];          // = "votingMembers"

    static const int kUninitializedNumNodes = -1;
    static const int kDisabled = 0;
    static const BSONObj Majority;       // = {"commitQuorum": "majority"}
    static const BSONObj VotingMembers;  // = {"commitQuorum": "votingMembers"}

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
    void appendToBuilder(StringData fieldName, BSONObjBuilder* builder) const;

    // The 'commitQuorum' parameter to define the required quorum for the index builds to commit.
    // The 'mode' represents the string format and takes precedence over the number format
    // 'numNodes'.
    int numNodes = kUninitializedNumNodes;
    std::string mode = "";
};

}  // namespace mongo
