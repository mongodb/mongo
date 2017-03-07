/**
 *    Copyright (C) 2017 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_shard_collection.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using unittest::assertGet;

const NamespaceString kNss = NamespaceString("db.coll");
const BSONObj kKeyPattern = BSON("a" << 1);

TEST(ShardCollectionType, ToFromShardBSONWithLastConsistentCollectionVersion) {
    const ChunkVersion lastConsistent(1, 0, OID::gen());

    BSONObjBuilder builder;
    builder.append(ShardCollectionType::uuid.name(), kNss.ns());
    builder.append(ShardCollectionType::ns.name(), kNss.ns());
    builder.append(ShardCollectionType::keyPattern.name(), kKeyPattern);
    lastConsistent.appendWithFieldForCommands(
        &builder, ShardCollectionType::lastConsistentCollectionVersion.name());
    BSONObj obj = builder.obj();

    ShardCollectionType shardCollectionType = assertGet(ShardCollectionType::fromBSON(obj));

    ASSERT_EQUALS(shardCollectionType.getUUID(), kNss);
    ASSERT_EQUALS(shardCollectionType.getNs(), kNss);
    ASSERT_BSONOBJ_EQ(shardCollectionType.getKeyPattern().toBSON(), kKeyPattern);
    ASSERT_EQUALS(shardCollectionType.getLastConsistentCollectionVersion(), lastConsistent);

    ASSERT_BSONOBJ_EQ(obj, shardCollectionType.toBSON());
}

TEST(ShardCollectionType, ToFromShardBSONWithoutLastConsistentCollectionVersion) {
    BSONObjBuilder builder;
    builder.append(ShardCollectionType::uuid.name(), kNss.ns());
    builder.append(ShardCollectionType::ns.name(), kNss.ns());
    builder.append(ShardCollectionType::keyPattern.name(), kKeyPattern);
    BSONObj obj = builder.obj();

    ShardCollectionType shardCollectionType = assertGet(ShardCollectionType::fromBSON(obj));

    ASSERT_EQUALS(shardCollectionType.getUUID(), kNss);
    ASSERT_EQUALS(shardCollectionType.getNs(), kNss);
    ASSERT_BSONOBJ_EQ(shardCollectionType.getKeyPattern().toBSON(), kKeyPattern);
    ASSERT_FALSE(shardCollectionType.isLastConsistentCollectionVersionSet());

    ASSERT_BSONOBJ_EQ(obj, shardCollectionType.toBSON());
}

TEST(ShardCollectionType, FromEmptyBSON) {
    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(BSONObj());
    ASSERT_FALSE(status.isOK());
}

TEST(ShardCollectionType, FromBSONNoUUIDFails) {
    BSONObj obj =
        BSON(ShardCollectionType::ns(kNss.ns()) << ShardCollectionType::keyPattern(kKeyPattern));

    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(obj);
    ASSERT_EQUALS(status.getStatus().code(), ErrorCodes::NoSuchKey);
}

TEST(ShardCollectionType, FromBSONNoNSFails) {
    BSONObj obj =
        BSON(ShardCollectionType::uuid(kNss.ns()) << ShardCollectionType::keyPattern(kKeyPattern));

    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(obj);
    ASSERT_EQUALS(status.getStatus().code(), ErrorCodes::NoSuchKey);
}

TEST(ShardCollectionType, FromBSONNoShardKeyFails) {
    BSONObj obj = BSON(ShardCollectionType::uuid(kNss.ns()) << ShardCollectionType::ns(kNss.ns()));

    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(obj);
    ASSERT_EQUALS(status.getStatus().code(), ErrorCodes::NoSuchKey);
}

TEST(ShardCollectionType, ConstructFromCollectionType) {
    const OID oid = OID::gen();

    BSONObj obj = BSON(CollectionType::fullNs(kNss.ns())
                       << CollectionType::epoch(oid)
                       << CollectionType::updatedAt(Date_t::fromMillisSinceEpoch(1))
                       << CollectionType::keyPattern(kKeyPattern));
    CollectionType collectionType = assertGet(CollectionType::fromBSON(obj));
    ASSERT_TRUE(collectionType.validate().isOK());

    BSONObjBuilder shardCollectionTypeBuilder;
    shardCollectionTypeBuilder.append(ShardCollectionType::uuid.name(), kNss.ns());
    shardCollectionTypeBuilder.append(ShardCollectionType::ns.name(), kNss.ns());
    shardCollectionTypeBuilder.append(ShardCollectionType::keyPattern.name(), kKeyPattern);

    ASSERT_BSONOBJ_EQ(ShardCollectionType(collectionType).toBSON(),
                      shardCollectionTypeBuilder.obj());
}

}  // namespace
}  // namespace mongo
