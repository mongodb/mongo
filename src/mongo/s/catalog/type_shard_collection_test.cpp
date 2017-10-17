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
const BSONObj kDefaultCollation = BSON("locale"
                                       << "fr_CA");

TEST(ShardCollectionType, ToFromBSON) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    const ChunkVersion lastRefreshedCollectionVersion(2, 0, epoch);

    BSONObjBuilder builder;
    builder.append(ShardCollectionType::ns.name(), kNss.ns());
    uuid.appendToBuilder(&builder, ShardCollectionType::uuid.name());
    builder.append(ShardCollectionType::epoch(), epoch);
    builder.append(ShardCollectionType::keyPattern.name(), kKeyPattern);
    builder.append(ShardCollectionType::defaultCollation(), kDefaultCollation);
    builder.append(ShardCollectionType::unique(), true);
    builder.append(ShardCollectionType::refreshing(), false);
    builder.appendTimestamp(ShardCollectionType::lastRefreshedCollectionVersion(),
                            lastRefreshedCollectionVersion.toLong());
    BSONObj obj = builder.obj();

    ShardCollectionType shardCollectionType = assertGet(ShardCollectionType::fromBSON(obj));

    ASSERT_EQUALS(shardCollectionType.getNss(), kNss);
    ASSERT(shardCollectionType.getUUID());
    ASSERT_EQUALS(*shardCollectionType.getUUID(), uuid);
    ASSERT_EQUALS(shardCollectionType.getEpoch(), epoch);
    ASSERT_BSONOBJ_EQ(shardCollectionType.getKeyPattern().toBSON(), kKeyPattern);
    ASSERT_BSONOBJ_EQ(shardCollectionType.getDefaultCollation(), kDefaultCollation);
    ASSERT_EQUALS(shardCollectionType.getUnique(), true);
    ASSERT_EQUALS(shardCollectionType.getRefreshing(), false);
    ASSERT_EQUALS(shardCollectionType.getLastRefreshedCollectionVersion(),
                  lastRefreshedCollectionVersion);

    ASSERT_BSONOBJ_EQ(obj, shardCollectionType.toBSON());
}

TEST(ShardCollectionType, ToFromShardBSONWithoutOptionals) {
    const OID epoch = OID::gen();

    BSONObjBuilder builder;
    builder.append(ShardCollectionType::ns.name(), kNss.ns());
    builder.append(ShardCollectionType::epoch(), epoch);
    builder.append(ShardCollectionType::keyPattern.name(), kKeyPattern);
    builder.append(ShardCollectionType::defaultCollation(), kDefaultCollation);
    builder.append(ShardCollectionType::unique(), true);
    BSONObj obj = builder.obj();

    ShardCollectionType shardCollectionType = assertGet(ShardCollectionType::fromBSON(obj));

    ASSERT_EQUALS(shardCollectionType.getNss(), kNss);
    ASSERT_EQUALS(shardCollectionType.getEpoch(), epoch);
    ASSERT_BSONOBJ_EQ(shardCollectionType.getKeyPattern().toBSON(), kKeyPattern);
    ASSERT_BSONOBJ_EQ(shardCollectionType.getDefaultCollation(), kDefaultCollation);
    ASSERT_EQUALS(shardCollectionType.getUnique(), true);
    ASSERT_FALSE(shardCollectionType.hasRefreshing());
    ASSERT_FALSE(shardCollectionType.hasLastRefreshedCollectionVersion());

    ASSERT_BSONOBJ_EQ(obj, shardCollectionType.toBSON());
}

TEST(ShardCollectionType, FromEmptyBSON) {
    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(BSONObj());
    ASSERT_FALSE(status.isOK());
}

TEST(ShardCollectionType, FromBSONNoUUIDIsOK) {
    BSONObjBuilder builder;
    builder.append(ShardCollectionType::ns.name(), kNss.ns());
    builder.append(ShardCollectionType::epoch(), OID::gen());
    builder.append(ShardCollectionType::keyPattern(), kKeyPattern);
    builder.append(ShardCollectionType::unique(), true);
    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(builder.obj());
    ASSERT_OK(status.getStatus());
    ASSERT_FALSE(status.getValue().getUUID());
}

TEST(ShardCollectionType, FromBSONNoNSFails) {
    BSONObjBuilder builder;
    UUID::gen().appendToBuilder(&builder, ShardCollectionType::uuid.name());
    builder.append(ShardCollectionType::epoch(), OID::gen());
    builder.append(ShardCollectionType::keyPattern(), kKeyPattern);
    builder.append(ShardCollectionType::unique(), true);
    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(builder.obj());
    ASSERT_EQUALS(status.getStatus().code(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(status.getStatus().reason(), ShardCollectionType::ns());
}

TEST(ShardCollectionType, FromBSONNoEpochFails) {
    BSONObjBuilder builder;
    builder.append(ShardCollectionType::ns.name(), kNss.ns());
    UUID::gen().appendToBuilder(&builder, ShardCollectionType::uuid.name());
    builder.append(ShardCollectionType::keyPattern(), kKeyPattern);
    builder.append(ShardCollectionType::unique(), true);
    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(builder.obj());
    ASSERT_EQUALS(status.getStatus().code(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(status.getStatus().reason(), ShardCollectionType::epoch());
}

TEST(ShardCollectionType, FromBSONNoShardKeyFails) {
    BSONObjBuilder builder;
    builder.append(ShardCollectionType::ns.name(), kNss.ns());
    UUID::gen().appendToBuilder(&builder, ShardCollectionType::uuid.name());
    builder.append(ShardCollectionType::epoch(), OID::gen());
    builder.append(ShardCollectionType::unique(), true);
    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(builder.obj());
    ASSERT_EQUALS(status.getStatus().code(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(status.getStatus().reason(), ShardCollectionType::keyPattern());
}

TEST(ShardCollectionType, FromBSONNoUniqueFails) {
    BSONObjBuilder builder;
    builder.append(ShardCollectionType::ns.name(), kNss.ns());
    UUID::gen().appendToBuilder(&builder, ShardCollectionType::uuid.name());
    builder.append(ShardCollectionType::epoch(), OID::gen());
    builder.append(ShardCollectionType::keyPattern.name(), kKeyPattern);
    builder.append(ShardCollectionType::defaultCollation(), kDefaultCollation);
    StatusWith<ShardCollectionType> status = ShardCollectionType::fromBSON(builder.obj());
    ASSERT_EQUALS(status.getStatus().code(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(status.getStatus().reason(), ShardCollectionType::unique());
}

TEST(ShardCollectionType, FromBSONNoDefaultCollationIsOK) {
    BSONObjBuilder builder;
    builder.append(ShardCollectionType::ns.name(), kNss.ns());
    UUID::gen().appendToBuilder(&builder, ShardCollectionType::uuid.name());
    builder.append(ShardCollectionType::epoch(), OID::gen());
    builder.append(ShardCollectionType::keyPattern.name(), kKeyPattern);
    builder.append(ShardCollectionType::unique(), true);

    assertGet(ShardCollectionType::fromBSON(builder.obj()));
}

}  // namespace
}  // namespace mongo
