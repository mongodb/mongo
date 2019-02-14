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

#include <string>

#include "mongo/db/jsobj.h"

namespace mongo {

class Status;

/**
 * The 'CommitQuorumOptions' has the same range of settings as the 'w' field from
 * 'WriteConcernOptions'. It can be set to an integer starting from 0 and up, or to a string. The
 * string option can either be 'majority' or a replica set tag.
 *
 * The principal idea behind 'CommitQuorumOptions' is to figure out when an index build should be
 * committed on the replica set based on the number of commit ready members.
 */
class CommitQuorumOptions {
public:
    static const StringData kCommitQuorumField;  // = "commitQuorum"
    static const char kMajority[];               // = "majority"

    static const BSONObj Majority;  // = {"commitQuorum": "majority"}

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
        numNodes = 0;
        mode = "";
    }

    // Returns the BSON representation of this object.
    BSONObj toBSON() const;

    // Appends the BSON representation of this object.
    void append(StringData fieldName, BSONObjBuilder* builder) const;

    // The 'commitQuorum' parameter to define the required quorum for the index builds to commit.
    // The 'mode' represents the string format and takes precedence over the number format
    // 'numNodes'.
    int numNodes;
    std::string mode;
};

}  // namespace mongo
