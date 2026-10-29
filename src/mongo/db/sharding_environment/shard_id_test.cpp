// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/shard_id.h"

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <compare>
#include <string_view>

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
    std::string_view stringData(shardId);
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
