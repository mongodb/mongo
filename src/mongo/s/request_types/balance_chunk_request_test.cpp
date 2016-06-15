/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/s/request_types/balance_chunk_request_type.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

TEST(BalanceChunkRequest, ParseFromConfigCommandNoSecondaryThrottle) {
    const ChunkVersion version(1, 0, OID::gen());
    auto request = assertGet(BalanceChunkRequest::parseFromConfigCommand(
        BSON("_configsvrMoveChunk" << 1 << "ns"
                                   << "TestDB.TestColl"
                                   << "min"
                                   << BSON("a" << -100LL)
                                   << "max"
                                   << BSON("a" << 100LL)
                                   << "shard"
                                   << "TestShard0000"
                                   << "lastmod"
                                   << Date_t::fromMillisSinceEpoch(version.toLong())
                                   << "lastmodEpoch"
                                   << version.epoch())));
    const auto& chunk = request.getChunk();
    ASSERT_EQ("TestDB.TestColl", chunk.getNS());
    ASSERT_EQ(BSON("a" << -100LL), chunk.getMin());
    ASSERT_EQ(BSON("a" << 100LL), chunk.getMax());
    ASSERT_EQ(ShardId("TestShard0000"), chunk.getShard());
    ASSERT_EQ(version, chunk.getVersion());

    const auto& secondaryThrottle = request.getSecondaryThrottle();
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault,
              secondaryThrottle.getSecondaryThrottle());
}

TEST(BalanceChunkRequest, ParseFromConfigCommandWithSecondaryThrottle) {
    const ChunkVersion version(1, 0, OID::gen());
    auto request = assertGet(BalanceChunkRequest::parseFromConfigCommand(
        BSON("_configsvrMoveChunk" << 1 << "ns"
                                   << "TestDB.TestColl"
                                   << "min"
                                   << BSON("a" << -100LL)
                                   << "max"
                                   << BSON("a" << 100LL)
                                   << "shard"
                                   << "TestShard0000"
                                   << "lastmod"
                                   << Date_t::fromMillisSinceEpoch(version.toLong())
                                   << "lastmodEpoch"
                                   << version.epoch()
                                   << "secondaryThrottle"
                                   << BSON("_secondaryThrottle" << true << "writeConcern"
                                                                << BSON("w" << 2)))));
    const auto& chunk = request.getChunk();
    ASSERT_EQ("TestDB.TestColl", chunk.getNS());
    ASSERT_EQ(BSON("a" << -100LL), chunk.getMin());
    ASSERT_EQ(BSON("a" << 100LL), chunk.getMax());
    ASSERT_EQ(ShardId("TestShard0000"), chunk.getShard());
    ASSERT_EQ(version, chunk.getVersion());

    const auto& secondaryThrottle = request.getSecondaryThrottle();
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOn, secondaryThrottle.getSecondaryThrottle());
    ASSERT_EQ(2, secondaryThrottle.getWriteConcern().wNumNodes);
}

}  // namespace
}  // namespace mongo
