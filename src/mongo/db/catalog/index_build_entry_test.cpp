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

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

enum CommitQuorumOptions { Number, Majority, Tag };

const std::vector<std::string> generateIndexes(size_t numIndexes) {
    std::vector<std::string> indexes;
    for (size_t i = 0; i < numIndexes; i++) {
        indexes.push_back("Index" + std::to_string(i));
    }
    return indexes;
}

const WriteConcernOptions generateCommitQuorum(CommitQuorumOptions option) {
    switch (option) {
        case Number:
            return WriteConcernOptions(1, WriteConcernOptions::SyncMode::UNSET, 0);
            break;
        case Majority:
            return WriteConcernOptions(
                WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, 0);
            break;
        case Tag:
            return WriteConcernOptions("someTag", WriteConcernOptions::SyncMode::UNSET, 0);
            break;
        default:
            return WriteConcernOptions(0, WriteConcernOptions::SyncMode::UNSET, 0);
    }
}

const std::vector<HostAndPort> generateCommitReadyMembers(size_t numMembers) {
    std::vector<HostAndPort> members;
    for (size_t i = 0; i < numMembers; i++) {
        members.push_back(HostAndPort("localhost:27017"));
    }
    return members;
}

TEST(IndexBuildEntryTest, IndexBuildEntryWithRequiredFields) {
    const UUID id = UUID::gen();
    const UUID collectionUUID = UUID::gen();
    const WriteConcernOptions commitQuorum = generateCommitQuorum(CommitQuorumOptions::Number);
    const std::vector<std::string> indexes = generateIndexes(1);

    IndexBuildEntry entry(id, collectionUUID, commitQuorum, indexes);

    ASSERT_EQUALS(entry.getBuildUUID(), id);
    ASSERT_EQUALS(entry.getCollectionUUID(), collectionUUID);
    ASSERT_EQUALS(entry.getCommitQuorum().wNumNodes, 1);
    ASSERT_TRUE(entry.getCommitQuorum().syncMode == WriteConcernOptions::SyncMode::UNSET);
    ASSERT_EQUALS(entry.getCommitQuorum().wTimeout, 0);
    ASSERT_EQUALS(entry.getIndexNames().size(), indexes.size());
    ASSERT_FALSE(entry.getPrepareIndexBuild());
}

TEST(IndexBuildEntryTest, IndexBuildEntryWithOptionalFields) {
    const UUID id = UUID::gen();
    const UUID collectionUUID = UUID::gen();
    const WriteConcernOptions commitQuorum = generateCommitQuorum(CommitQuorumOptions::Majority);
    const std::vector<std::string> indexes = generateIndexes(3);

    IndexBuildEntry entry(id, collectionUUID, commitQuorum, indexes);

    ASSERT_FALSE(entry.getPrepareIndexBuild());
    entry.setPrepareIndexBuild(true);
    entry.setCommitReadyMembers(generateCommitReadyMembers(2));

    ASSERT_EQUALS(entry.getBuildUUID(), id);
    ASSERT_EQUALS(entry.getCollectionUUID(), collectionUUID);
    ASSERT_EQUALS(entry.getCommitQuorum().wMode, WriteConcernOptions::kMajority);
    ASSERT_TRUE(entry.getCommitQuorum().syncMode == WriteConcernOptions::SyncMode::UNSET);
    ASSERT_EQUALS(entry.getCommitQuorum().wTimeout, 0);
    ASSERT_EQUALS(entry.getIndexNames().size(), indexes.size());
    ASSERT_TRUE(entry.getPrepareIndexBuild());
    ASSERT_TRUE(entry.getCommitReadyMembers()->size() == 2);
}

TEST(IndexBuildEntryTest, SerializeAndDeserialize) {
    const UUID id = UUID::gen();
    const UUID collectionUUID = UUID::gen();
    const WriteConcernOptions commitQuorum = generateCommitQuorum(CommitQuorumOptions::Tag);
    const std::vector<std::string> indexes = generateIndexes(1);

    IndexBuildEntry entry(id, collectionUUID, commitQuorum, indexes);
    entry.setPrepareIndexBuild(false);
    entry.setCommitReadyMembers(generateCommitReadyMembers(3));

    BSONObj obj = entry.toBSON();
    ASSERT_TRUE(obj.valid(BSONVersion::kLatest));

    IDLParserErrorContext ctx("IndexBuildsEntry Parser");
    IndexBuildEntry rebuiltEntry = IndexBuildEntry::parse(ctx, obj);

    ASSERT_EQUALS(rebuiltEntry.getBuildUUID(), id);
    ASSERT_EQUALS(rebuiltEntry.getCollectionUUID(), collectionUUID);
    ASSERT_EQUALS(rebuiltEntry.getCommitQuorum().wMode, "someTag");
    ASSERT_TRUE(rebuiltEntry.getCommitQuorum().syncMode == WriteConcernOptions::SyncMode::UNSET);
    ASSERT_EQUALS(rebuiltEntry.getCommitQuorum().wTimeout, 0);
    ASSERT_EQUALS(rebuiltEntry.getIndexNames().size(), indexes.size());
    ASSERT_FALSE(rebuiltEntry.getPrepareIndexBuild());
    ASSERT_TRUE(rebuiltEntry.getCommitReadyMembers()->size() == 3);
}

}  // namespace
}  // namespace mongo
