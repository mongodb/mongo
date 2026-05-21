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

#include "mongo/db/global_catalog/type_shard.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;

using std::string;

TEST(ShardType, MissingName) {
    BSONObj obj = BSON(ShardType::host("localhost:27017"));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT_FALSE(shardRes.isOK());
}

TEST(ShardType, MissingHost) {
    BSONObj obj = BSON(ShardType::name("shard0000"));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT_FALSE(shardRes.isOK());
}

TEST(ShardType, OnlyMandatory) {
    BSONObj obj = BSON(ShardType::name("shard0000") << ShardType::host("localhost:27017"));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT(shardRes.isOK());
    ShardType shard = shardRes.getValue();
    ASSERT(shard.validate().isOK());
}

TEST(ShardType, AllOptionalsPresent) {
    BSONObj obj = BSON(ShardType::name("shard0000")
                       << ShardType::host("localhost:27017") << ShardType::draining(true));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT(shardRes.isOK());
    ShardType shard = shardRes.getValue();
    ASSERT(shard.validate().isOK());
}

TEST(ShardType, BadType) {
    BSONObj obj = BSON(ShardType::name() << 0);
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT_FALSE(shardRes.isOK());
}

TEST(ShardType, ShardRefAbsentDefaultsToName) {
    BSONObj obj = BSON(ShardType::name("shard0000") << ShardType::host("localhost:27017"));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT(shardRes.isOK());
    const ShardType& shard = shardRes.getValue();
    ASSERT_EQ(shard.getName(), "shard0000");
    ASSERT_EQ(shard.getHandle().name().toString(), shard.getName());
    const auto& ref = shard.getShardRef();
    ASSERT(ref.isString());
    ASSERT_EQ(ref.getString(), "shard0000");
    ASSERT_EQ(shard.getHandle().ref(), ref);
}

TEST(ShardType, ShardRefString) {
    BSONObj obj = BSON(ShardType::name("shard0000") << ShardType::host("localhost:27017")
                                                    << ShardType::kShardRefFieldName << "myRef");
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT(shardRes.isOK());
    const ShardType& shard = shardRes.getValue();
    ASSERT_EQ(shard.getName(), "shard0000");
    ASSERT_EQ(shard.getHandle().name().toString(), shard.getName());
    const auto& ref = shard.getShardRef();
    ASSERT(ref.isString());
    ASSERT_EQ(ref.getString(), "myRef");
    ASSERT_EQ(shard.getHandle().ref(), ref);
}

TEST(ShardType, ShardRefUUID) {
    UUID uuid = UUID::gen();
    BSONObjBuilder builder;
    builder.append(ShardType::name(), "shard0000");
    builder.append(ShardType::host(), "localhost:27017");
    uuid.appendToBuilder(&builder, ShardType::kShardRefFieldName);
    StatusWith<ShardType> shardRes = ShardType::fromBSON(builder.obj());
    ASSERT(shardRes.isOK());
    const ShardType& shard = shardRes.getValue();
    ASSERT_EQ(shard.getName(), "shard0000");
    ASSERT_EQ(shard.getHandle().name().toString(), shard.getName());
    const auto& ref = shard.getShardRef();
    ASSERT(ref.isUUID());
    ASSERT_EQ(ref.getUUID(), uuid);
    ASSERT_EQ(shard.getHandle().ref(), ref);
}

TEST(ShardType, ShardRefBadType) {
    BSONObj obj = BSON(ShardType::name("shard0000") << ShardType::host("localhost:27017")
                                                    << ShardType::kShardRefFieldName << 42);
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT_FALSE(shardRes.isOK());
}

}  // unnamed namespace
