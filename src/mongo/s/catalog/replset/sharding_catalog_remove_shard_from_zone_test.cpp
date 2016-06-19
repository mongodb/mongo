/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/config_server_test_fixture.h"

namespace mongo {
namespace {


ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly);

using RemoveShardFromZoneTest = ConfigServerTestFixture;

TEST_F(RemoveShardFromZoneTest, RemoveZoneThatNoLongerExistsShouldNotError) {
    ShardType shard;
    shard.setName("a");
    shard.setHost("a:1234");

    setupShards({shard});

    ASSERT_OK(catalogManager()->removeShardFromZone(operationContext(), shard.getName(), "z"));
    auto shardDocStatus = getShardDoc(operationContext(), shard.getName());
    ASSERT_OK(shardDocStatus.getStatus());

    auto shardDoc = shardDocStatus.getValue();
    auto tags = shardDoc.getTags();
    ASSERT_TRUE(tags.empty());
}

TEST_F(RemoveShardFromZoneTest, RemovingZoneThatIsOnlyReferencedByAnotherShardShouldSucceed) {
    ShardType shardA;
    shardA.setName("a");
    shardA.setHost("a:1234");
    shardA.setTags({"z"});

    ShardType shardB;
    shardB.setName("b");
    shardB.setHost("b:1234");

    setupShards({shardA, shardB});

    ASSERT_OK(catalogManager()->removeShardFromZone(operationContext(), shardB.getName(), "z"));

    // Shard A should still be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_EQ(1u, shardATags.size());
    ASSERT_EQ("z", shardATags.front());

    // Shard B should not be in zone 'z'.
    auto shardBDocStatus = getShardDoc(operationContext(), shardB.getName());
    ASSERT_OK(shardBDocStatus.getStatus());

    auto shardBDoc = shardBDocStatus.getValue();
    auto shardBTags = shardBDoc.getTags();
    ASSERT_TRUE(shardBTags.empty());
}

TEST_F(RemoveShardFromZoneTest, RemoveLastZoneFromShardShouldSucceedWhenNoChunksReferToIt) {
    ShardType shardA;
    shardA.setName("a");
    shardA.setHost("a:1234");
    shardA.setTags({"z"});

    ShardType shardB;
    shardB.setName("b");
    shardB.setHost("b:1234");

    setupShards({shardA, shardB});

    // Insert a chunk range document referring to a different zone
    TagsType tagDoc;
    tagDoc.setNS("test.foo");
    tagDoc.setMinKey(BSON("x" << 0));
    tagDoc.setMaxKey(BSON("x" << 10));
    tagDoc.setTag("y");
    insertToConfigCollection(
        operationContext(), NamespaceString(TagsType::ConfigNS), tagDoc.toBSON());

    ASSERT_OK(catalogManager()->removeShardFromZone(operationContext(), shardA.getName(), "z"));

    // Shard A should not be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_TRUE(shardATags.empty());

    // Shard B should not be in zone 'z'.
    auto shardBDocStatus = getShardDoc(operationContext(), shardB.getName());
    ASSERT_OK(shardBDocStatus.getStatus());

    auto shardBDoc = shardBDocStatus.getValue();
    auto shardBTags = shardBDoc.getTags();
    ASSERT_TRUE(shardBTags.empty());
}

TEST_F(RemoveShardFromZoneTest, RemoveLastZoneFromShardShouldFailWhenAChunkRefersToIt) {
    ShardType shardA;
    shardA.setName("a");
    shardA.setHost("a:1234");
    shardA.setTags({"y", "z"});

    ShardType shardB;
    shardB.setName("b");
    shardB.setHost("b:1234");

    setupShards({shardA, shardB});

    TagsType tagDoc;
    tagDoc.setNS("test.foo");
    tagDoc.setMinKey(BSON("x" << 0));
    tagDoc.setMaxKey(BSON("x" << 10));
    tagDoc.setTag("z");
    insertToConfigCollection(
        operationContext(), NamespaceString(TagsType::ConfigNS), tagDoc.toBSON());

    auto status = catalogManager()->removeShardFromZone(operationContext(), shardA.getName(), "z");
    ASSERT_EQ(ErrorCodes::ZoneStillInUse, status);

    // Shard A should still be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_EQ(2u, shardATags.size());
    ASSERT_EQ("y", shardATags.front());
    ASSERT_EQ("z", shardATags.back());

    // Shard B should not be in zone 'z'.
    auto shardBDocStatus = getShardDoc(operationContext(), shardB.getName());
    ASSERT_OK(shardBDocStatus.getStatus());

    auto shardBDoc = shardBDocStatus.getValue();
    auto shardBTags = shardBDoc.getTags();
    ASSERT_TRUE(shardBTags.empty());
}

TEST_F(RemoveShardFromZoneTest, RemoveZoneShouldFailIfShardDoesntExist) {
    ShardType shardA;
    shardA.setName("a");
    shardA.setHost("a:1234");
    shardA.setTags({"z"});

    setupShards({shardA});

    auto status = catalogManager()->removeShardFromZone(operationContext(), "b", "z");
    ASSERT_EQ(ErrorCodes::ShardNotFound, status);

    // Shard A should still be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_EQ(1u, shardATags.size());
    ASSERT_EQ("z", shardATags.front());
}

TEST_F(RemoveShardFromZoneTest, RemoveZoneFromShardShouldOnlyRemoveZoneOnSpecifiedShard) {
    ShardType shardA;
    shardA.setName("a");
    shardA.setHost("a:1234");
    shardA.setTags({"z"});

    ShardType shardB;
    shardB.setName("b");
    shardB.setHost("b:1234");
    shardB.setTags({"y", "z"});

    setupShards({shardA, shardB});

    ASSERT_OK(catalogManager()->removeShardFromZone(operationContext(), shardB.getName(), "z"));

    // Shard A should still be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_EQ(1u, shardATags.size());
    ASSERT_EQ("z", shardATags.front());

    // Shard B should not be in zone 'z'.
    auto shardBDocStatus = getShardDoc(operationContext(), shardB.getName());
    ASSERT_OK(shardBDocStatus.getStatus());

    auto shardBDoc = shardBDocStatus.getValue();
    auto shardBTags = shardBDoc.getTags();
    ASSERT_EQ(1u, shardBTags.size());
    ASSERT_EQ("y", shardBTags.front());
}

TEST_F(RemoveShardFromZoneTest, RemoveZoneFromShardShouldErrorIfShardDocIsMalformed) {
    // Note: invalid because tags is in string instead of array.
    BSONObj invalidShardDoc(BSON("_id"
                                 << "a"
                                 << "host"
                                 << "a:1"
                                 << "tags"
                                 << "z"));

    insertToConfigCollection(
        operationContext(), NamespaceString(ShardType::ConfigNS), invalidShardDoc);


    auto status = catalogManager()->removeShardFromZone(operationContext(), "a", "z");
    ASSERT_EQ(ErrorCodes::TypeMismatch, status);
}

}  // unnamed namespace
}  // namespace mongo
