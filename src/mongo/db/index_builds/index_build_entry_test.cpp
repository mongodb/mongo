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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index_builds/commit_quorum_options.h"
#include "mongo/db/index_builds/index_build_entry_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

std::vector<std::string> generateIndexes(size_t numIndexes) {
    std::vector<std::string> indexes;
    for (size_t i = 0; i < numIndexes; i++) {
        indexes.push_back("index_" + std::to_string(i));
    }
    return indexes;
}

std::vector<HostAndPort> generateCommitReadyMembers(size_t numMembers) {
    std::vector<HostAndPort> members;
    for (size_t i = 0; i < numMembers; i++) {
        members.push_back(HostAndPort("localhost:27017"));
    }
    return members;
}

void checkIfEqual(IndexBuildEntry lhs, IndexBuildEntry rhs) {
    ASSERT_EQ(lhs.getBuildUUID(), rhs.getBuildUUID());
    ASSERT_EQ(lhs.getCollectionUUID(), rhs.getCollectionUUID());

    BSONObj commitQuorumOptionsBsonLHS = lhs.getCommitQuorum().toBSON();
    BSONObj commitQuorumOptionsBsonRHS = rhs.getCommitQuorum().toBSON();
    ASSERT_BSONOBJ_EQ(commitQuorumOptionsBsonLHS, commitQuorumOptionsBsonRHS);

    auto lhsIndexNames = lhs.getIndexNames();
    auto rhsIndexNames = rhs.getIndexNames();
    ASSERT_TRUE(std::equal(lhsIndexNames.begin(), lhsIndexNames.end(), rhsIndexNames.begin()));

    if (lhs.getCommitReadyMembers() && rhs.getCommitReadyMembers()) {
        auto lhsMembers = lhs.getCommitReadyMembers().value();
        auto rhsMembers = rhs.getCommitReadyMembers().value();
        ASSERT_TRUE(std::equal(lhsMembers.begin(), lhsMembers.end(), rhsMembers.begin()));
    } else {
        ASSERT_FALSE(lhs.getCommitReadyMembers());
        ASSERT_FALSE(rhs.getCommitReadyMembers());
    }
}

TEST(IndexBuildEntryTest, IndexBuildEntryWithRequiredFields) {
    const UUID id = UUID::gen();
    const UUID collectionUUID = UUID::gen();
    const CommitQuorumOptions commitQuorum(1);
    const std::vector<std::string> indexes = generateIndexes(1);

    IndexBuildEntry entry(id, collectionUUID, commitQuorum, indexes);

    ASSERT_EQUALS(entry.getBuildUUID(), id);
    ASSERT_EQUALS(entry.getCollectionUUID(), collectionUUID);
    ASSERT_EQUALS(entry.getCommitQuorum().numNodes, 1);
    ASSERT_EQUALS(entry.getIndexNames().size(), indexes.size());
}

TEST(IndexBuildEntryTest, IndexBuildEntryWithOptionalFields) {
    const UUID id = UUID::gen();
    const UUID collectionUUID = UUID::gen();
    const CommitQuorumOptions commitQuorum(CommitQuorumOptions::kMajority);
    const std::vector<std::string> indexes = generateIndexes(3);

    IndexBuildEntry entry(id, collectionUUID, commitQuorum, indexes);

    entry.setCommitReadyMembers(generateCommitReadyMembers(2));

    ASSERT_EQUALS(entry.getBuildUUID(), id);
    ASSERT_EQUALS(entry.getCollectionUUID(), collectionUUID);
    ASSERT_EQUALS(entry.getCommitQuorum().mode, CommitQuorumOptions::kMajority);
    ASSERT_EQUALS(entry.getIndexNames().size(), indexes.size());
    ASSERT_EQ(entry.getCommitReadyMembers()->size(), 2U);
}

TEST(IndexBuildEntryTest, SerializeAndDeserialize) {
    const UUID id = UUID::gen();
    const UUID collectionUUID = UUID::gen();
    const CommitQuorumOptions commitQuorum("someTag");
    const std::vector<std::string> indexes = generateIndexes(1);

    IndexBuildEntry entry(id, collectionUUID, commitQuorum, indexes);
    entry.setCommitReadyMembers(generateCommitReadyMembers(3));

    BSONObj obj = entry.toBSON();
    ASSERT_TRUE(validateBSON(obj).isOK());

    IDLParserContext ctx("IndexBuildsEntry Parser");
    IndexBuildEntry rebuiltEntry = IndexBuildEntry::parse(obj, ctx);

    checkIfEqual(entry, rebuiltEntry);
}

}  // namespace
}  // namespace mongo
