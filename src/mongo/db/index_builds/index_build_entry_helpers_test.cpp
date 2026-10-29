// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_entry_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index_builds/commit_quorum_options.h"
#include "mongo/db/index_builds/index_build_entry_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

namespace {
using namespace indexbuildentryhelpers;

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

Status removeIndexBuildEntry(OperationContext* opCtx, UUID indexBuildUUID) {
    auto acq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, NamespaceString::kIndexBuildEntryNamespace, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    return indexbuildentryhelpers::removeIndexBuildEntry(
        opCtx, acq.getCollectionPtr(), indexBuildUUID);
}

class IndexBuildEntryHelpersTest : public CatalogTestFixture {
public:
    void setUp() override {
        CatalogTestFixture::setUp();

        const UUID collectionUUID = UUID::gen();

        // `_firstEntry` and `_secondEntry` are index builds on the same collection.
        _firstEntry = IndexBuildEntry(UUID::gen(),
                                      collectionUUID,
                                      CommitQuorumOptions(CommitQuorumOptions::kMajority),
                                      generateIndexes(3));
        _secondEntry = IndexBuildEntry(
            UUID::gen(), collectionUUID, CommitQuorumOptions(5), generateIndexes(6));

        _thirdEntry = IndexBuildEntry(
            UUID::gen(), UUID::gen(), CommitQuorumOptions("someTag"), generateIndexes(10));

        ensureIndexBuildEntriesNamespaceExists(operationContext());
    }

protected:
    IndexBuildEntry _firstEntry;
    IndexBuildEntry _secondEntry;
    IndexBuildEntry _thirdEntry;
};

TEST_F(IndexBuildEntryHelpersTest, AddIndexBuildEntry) {
    // Insert an entry twice. The second time we should get a DuplicateKey error.
    ASSERT_OK(addIndexBuildEntry(operationContext(), _firstEntry));

    Status status = addIndexBuildEntry(operationContext(), _firstEntry);
    EXPECT_EQ(status.code(), ErrorCodes::DuplicateKey);

    ASSERT_OK(addIndexBuildEntry(operationContext(), _secondEntry));
    ASSERT_OK(addIndexBuildEntry(operationContext(), _thirdEntry));

    unittest::assertGet(getIndexBuildEntry(operationContext(), _firstEntry.getBuildUUID()));
    unittest::assertGet(getIndexBuildEntry(operationContext(), _secondEntry.getBuildUUID()));
    unittest::assertGet(getIndexBuildEntry(operationContext(), _thirdEntry.getBuildUUID()));
}

TEST_F(IndexBuildEntryHelpersTest, RemoveIndexBuildEntry) {
    ASSERT_OK(addIndexBuildEntry(operationContext(), _firstEntry));
    ASSERT_OK(addIndexBuildEntry(operationContext(), _secondEntry));

    // Remove an entry with an incorrect index build UUID.
    Status status = removeIndexBuildEntry(operationContext(), UUID::gen());
    EXPECT_EQ(status, ErrorCodes::NoMatchingDocument);

    ASSERT_OK(removeIndexBuildEntry(operationContext(), _firstEntry.getBuildUUID()));
    status = removeIndexBuildEntry(operationContext(), _firstEntry.getBuildUUID());
    EXPECT_EQ(status, ErrorCodes::NoMatchingDocument);

    ASSERT_OK(removeIndexBuildEntry(operationContext(), _secondEntry.getBuildUUID()));
}

TEST_F(IndexBuildEntryHelpersTest, CommitQuorum) {
    ASSERT_OK(addIndexBuildEntry(operationContext(), _firstEntry));

    {
        StatusWith<CommitQuorumOptions> statusWith =
            getCommitQuorum(operationContext(), UUID::gen());
        EXPECT_EQ(statusWith.getStatus(), ErrorCodes::NoMatchingDocument);

        Status status =
            setCommitQuorum_forTest(operationContext(), UUID::gen(), CommitQuorumOptions(1));
        EXPECT_EQ(status.code(), ErrorCodes::NoMatchingDocument);
    }

    {
        CommitQuorumOptions opts =
            unittest::assertGet(getCommitQuorum(operationContext(), _firstEntry.getBuildUUID()));
        ASSERT_BSONOBJ_EQ(opts.toBSON(), _firstEntry.getCommitQuorum().toBSON());

        CommitQuorumOptions newCommitQuorum(2);
        ASSERT_OK(setCommitQuorum_forTest(
            operationContext(), _firstEntry.getBuildUUID(), newCommitQuorum));

        opts = unittest::assertGet(getCommitQuorum(operationContext(), _firstEntry.getBuildUUID()));
        ASSERT_BSONOBJ_EQ(opts.toBSON(), newCommitQuorum.toBSON());
    }
}

}  // namespace
}  // namespace mongo
