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

#include "mongo/db/sharding_environment/shard_id.h"

#include "mongo/base/string_data.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <compare>

namespace mongo {
namespace {

using std::string;
using namespace mongo;

TEST(ShardId, Valid) {
    ShardId shardId("my_shard_id");
    ASSERT(shardId.isValid());
}

TEST(ShardId, Invalid) {
    ShardId shardId("");
    ASSERT(!shardId.isValid());
}

TEST(ShardId, Roundtrip) {
    string shard_id_str("my_shard_id");
    ShardId shardId(shard_id_str);
    ASSERT(shard_id_str == shardId.toString());
}

TEST(ShardId, ToStringData) {
    string shard_id_str("my_shard_id");
    ShardId shardId(shard_id_str);
    StringData stringData(shardId);
    ASSERT(stringData == shard_id_str);
}

TEST(ShardId, Assign) {
    ShardId shardId1("my_shard_id");
    auto shardId2 = shardId1;
    ASSERT(shardId1 == shardId2);
}

TEST(ShardId, Less) {
    string a("aaa");
    string a1("aaa");
    string b("bbb");
    ShardId sa(a);
    ShardId sa1(a1);
    ShardId sb(b);
    ASSERT_EQUALS(sa < sa1, a < a1);
    ASSERT_EQUALS(sb < sa1, b < a1);
    ASSERT_EQUALS(sa < sb, a < b);
}

TEST(ShardId, Compare) {
    string a("aaa");
    string a1("aaa");
    string b("bbb");
    ShardId sa(a);
    ShardId sa1(a1);
    ShardId sb(b);
    ASSERT_EQUALS(sa.compare(sa1), a.compare(a1));
    ASSERT_EQUALS(sb.compare(sa1) > 0, b.compare(a1) > 0);
    ASSERT_EQUALS(sa.compare(sb) < 0, a.compare(b) < 0);
}

TEST(ShardId, Equals) {
    string a("aaa");
    string a1("aaa");
    string b("bbb");
    ShardId sa(a);
    ShardId sa1(a1);
    ShardId sb(b);
    ASSERT(sa == sa1);
    ASSERT(sa != sb);
    ASSERT(sa == "aaa");
    ASSERT(sa != "bbb");
}

TEST(ShardId, isShardURL) {
    ShardId url("shardName/ip-10-122-13-136:20043");
    ShardId urlWithoutPort("shardName/ip-10-122-13-136");
    ShardId emptyUrl("");
    ShardId urlWithoutName("/ip-10-122-13-136:20043");
    ShardId urlOnlyPort("/:20043");
    ShardId localhost("shardName/localhost:3000");
    ShardId multipleHosts(
        "shardName/ip-10-122-13-136:20043,ip-10-122-13-136:20042,ip-11-122-33-156:20100");
    ASSERT(url.isShardURL() == true);
    ASSERT(urlWithoutPort.isShardURL() == false);
    ASSERT(emptyUrl.isShardURL() == false);
    ASSERT(urlWithoutName.isShardURL() == false);
    ASSERT(urlOnlyPort.isShardURL() == false);
    ASSERT(localhost.isShardURL() == true);
    ASSERT(multipleHosts.isShardURL() == true);
}

}  // namespace
}  // namespace mongo
