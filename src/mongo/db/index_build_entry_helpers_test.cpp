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

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {
using namespace indexbuildentryhelpers;

const std::vector<std::string> generateIndexes(size_t numIndexes) {
    std::vector<std::string> indexes;
    for (size_t i = 0; i < numIndexes; i++) {
        indexes.push_back("index_" + std::to_string(i));
    }
    return indexes;
}

const std::vector<HostAndPort> generateCommitReadyMembers(size_t numMembers) {
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
        auto lhsMembers = lhs.getCommitReadyMembers().get();
        auto rhsMembers = rhs.getCommitReadyMembers().get();
        ASSERT_TRUE(std::equal(lhsMembers.begin(), lhsMembers.end(), rhsMembers.begin()));
    } else {
        ASSERT_FALSE(lhs.getCommitReadyMembers());
        ASSERT_FALSE(rhs.getCommitReadyMembers());
    }
}

class IndexBuildEntryHelpersTest : public CatalogTestFixture {
public:
    void setUp() {
        CatalogTestFixture::setUp();
        operationContext()->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

        const UUID collectionUUID = UUID::gen();
        const CommitQuorumOptions commitQuorum(CommitQuorumOptions::kMajority);

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
    ASSERT_EQUALS(status.code(), ErrorCodes::DuplicateKey);

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
    ASSERT_EQUALS(status, ErrorCodes::NoMatchingDocument);

    ASSERT_OK(removeIndexBuildEntry(operationContext(), _firstEntry.getBuildUUID()));
    status = removeIndexBuildEntry(operationContext(), _firstEntry.getBuildUUID());
    ASSERT_EQUALS(status, ErrorCodes::NoMatchingDocument);

    ASSERT_OK(removeIndexBuildEntry(operationContext(), _secondEntry.getBuildUUID()));
}

TEST_F(IndexBuildEntryHelpersTest, CommitQuorum) {
    ASSERT_OK(addIndexBuildEntry(operationContext(), _firstEntry));

    {
        StatusWith<CommitQuorumOptions> statusWith =
            getCommitQuorum(operationContext(), UUID::gen());
        ASSERT_EQUALS(statusWith.getStatus(), ErrorCodes::NoMatchingDocument);

        Status status =
            setCommitQuorum_forTest(operationContext(), UUID::gen(), CommitQuorumOptions(1));
        ASSERT_EQUALS(status.code(), ErrorCodes::NoMatchingDocument);
    }

    {
        CommitQuorumOptions opts =
            unittest::assertGet(getCommitQuorum(operationContext(), _firstEntry.getBuildUUID()));
        ASSERT_BSONOBJ_EQ(opts.toBSON(), _firstEntry.getCommitQuorum().toBSON());

        CommitQuorumOptions newCommitQuorum(0);
        ASSERT_OK(setCommitQuorum_forTest(
            operationContext(), _firstEntry.getBuildUUID(), newCommitQuorum));

        opts = unittest::assertGet(getCommitQuorum(operationContext(), _firstEntry.getBuildUUID()));
        ASSERT_BSONOBJ_EQ(opts.toBSON(), newCommitQuorum.toBSON());
    }
}

}  // namespace
}  // namespace mongo
