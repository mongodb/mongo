/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/s/catalog/type_shard.h"

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

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
    BSONObj obj = BSON(ShardType::name("shard0000") << ShardType::host("localhost:27017")
                                                    << ShardType::draining(true)
                                                    << ShardType::maxSizeMB(100));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT(shardRes.isOK());
    ShardType shard = shardRes.getValue();
    ASSERT(shard.validate().isOK());
}

TEST(ShardType, MaxSizeAsFloat) {
    BSONObj obj = BSON(ShardType::name("shard0000") << ShardType::host("localhost:27017")
                                                    << ShardType::maxSizeMB()
                                                    << 100.0);
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

}  // unnamed namespace
