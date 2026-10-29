// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
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
    EXPECT_EQ(lhs.getBuildUUID(), rhs.getBuildUUID());
    EXPECT_EQ(lhs.getCollectionUUID(), rhs.getCollectionUUID());

    BSONObj commitQuorumOptionsBsonLHS = lhs.getCommitQuorum().toBSON();
    BSONObj commitQuorumOptionsBsonRHS = rhs.getCommitQuorum().toBSON();
    ASSERT_BSONOBJ_EQ(commitQuorumOptionsBsonLHS, commitQuorumOptionsBsonRHS);

    auto lhsIndexNames = lhs.getIndexNames();
    auto rhsIndexNames = rhs.getIndexNames();
    EXPECT_TRUE(std::equal(lhsIndexNames.begin(), lhsIndexNames.end(), rhsIndexNames.begin()));

    if (lhs.getCommitReadyMembers() && rhs.getCommitReadyMembers()) {
        auto lhsMembers = lhs.getCommitReadyMembers().value();
        auto rhsMembers = rhs.getCommitReadyMembers().value();
        EXPECT_TRUE(std::equal(lhsMembers.begin(), lhsMembers.end(), rhsMembers.begin()));
    } else {
        EXPECT_FALSE(lhs.getCommitReadyMembers());
        EXPECT_FALSE(rhs.getCommitReadyMembers());
    }
}

TEST(IndexBuildEntryTest, IndexBuildEntryWithRequiredFields) {
    const UUID id = UUID::gen();
    const UUID collectionUUID = UUID::gen();
    const CommitQuorumOptions commitQuorum(1);
    const std::vector<std::string> indexes = generateIndexes(1);

    IndexBuildEntry entry(id, collectionUUID, commitQuorum, indexes);

    EXPECT_EQ(entry.getBuildUUID(), id);
    EXPECT_EQ(entry.getCollectionUUID(), collectionUUID);
    EXPECT_EQ(entry.getCommitQuorum().numNodes, 1);
    EXPECT_EQ(entry.getIndexNames().size(), indexes.size());
}

TEST(IndexBuildEntryTest, IndexBuildEntryWithOptionalFields) {
    const UUID id = UUID::gen();
    const UUID collectionUUID = UUID::gen();
    const CommitQuorumOptions commitQuorum(CommitQuorumOptions::kMajority);
    const std::vector<std::string> indexes = generateIndexes(3);

    IndexBuildEntry entry(id, collectionUUID, commitQuorum, indexes);

    entry.setCommitReadyMembers(generateCommitReadyMembers(2));

    EXPECT_EQ(entry.getBuildUUID(), id);
    EXPECT_EQ(entry.getCollectionUUID(), collectionUUID);
    EXPECT_EQ(entry.getCommitQuorum().mode, CommitQuorumOptions::kMajority);
    EXPECT_EQ(entry.getIndexNames().size(), indexes.size());
    EXPECT_EQ(entry.getCommitReadyMembers()->size(), 2U);
}

TEST(IndexBuildEntryTest, SerializeAndDeserialize) {
    const UUID id = UUID::gen();
    const UUID collectionUUID = UUID::gen();
    const CommitQuorumOptions commitQuorum("someTag");
    const std::vector<std::string> indexes = generateIndexes(1);

    IndexBuildEntry entry(id, collectionUUID, commitQuorum, indexes);
    entry.setCommitReadyMembers(generateCommitReadyMembers(3));

    BSONObj obj = entry.toBSON();
    EXPECT_TRUE(validateBSON(obj).isOK());

    IDLParserContext ctx("IndexBuildsEntry Parser");
    IndexBuildEntry rebuiltEntry = IndexBuildEntry::parse(obj, ctx);

    checkIfEqual(entry, rebuiltEntry);
}

}  // namespace
}  // namespace mongo
