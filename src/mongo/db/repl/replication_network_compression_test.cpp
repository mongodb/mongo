/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/repl/replication_network_compression.h"

#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace repl {
namespace {

TEST(ReplicationNetworkCompression, ParseDistinguishesInheritDisabledAndAllowList) {
    ReplicationNetworkCompressionSetting setting;

    ASSERT_OK(parseReplicationNetworkCompression("", &setting));
    ASSERT_TRUE(setting.inheritProcessDefault);
    ASSERT_FALSE(setting.disabled);
    ASSERT_TRUE(setting.allowList.empty());

    ASSERT_OK(parseReplicationNetworkCompression("disabled", &setting));
    ASSERT_FALSE(setting.inheritProcessDefault);
    ASSERT_TRUE(setting.disabled);
    ASSERT_TRUE(setting.allowList.empty());

    ASSERT_OK(parseReplicationNetworkCompression(" zstd, snappy, zstd, ", &setting));
    ASSERT_FALSE(setting.inheritProcessDefault);
    ASSERT_FALSE(setting.disabled);
    ASSERT_EQ(2U, setting.allowList.size());
    ASSERT_EQ(std::string{"zstd"}, setting.allowList[0]);
    ASSERT_EQ(std::string{"snappy"}, setting.allowList[1]);
}

TEST(ReplicationNetworkCompression, ParseRejectsMalformedValues) {
    ReplicationNetworkCompressionSetting setting;

    ASSERT_NOT_OK(parseReplicationNetworkCompression(",", &setting));
    ASSERT_NOT_OK(parseReplicationNetworkCompression("disabled,snappy", &setting));
}

TEST(ReplicationNetworkCompression, ExplicitAllowListDrivesRegistryUnion) {
    ReplicationNetworkCompressionSetting setting;
    ASSERT_OK(parseReplicationNetworkCompression("snappy", &setting));
    ASSERT_FALSE(setting.inheritProcessDefault);
    ASSERT_FALSE(setting.disabled);

    MessageCompressorRegistry registry;
    registry.setSupportedCompressors(std::vector<std::string>{});
    if (!setting.disabled && !setting.inheritProcessDefault) {
        registry.addReplicationCompressors(setting.allowList);
    }
    registry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
    ASSERT_OK(registry.finalizeSupportedCompressors());

    ASSERT_TRUE(registry.getNetCompressorNames().empty());
    const auto& unionNames = registry.getCompressorNames();
    ASSERT_TRUE(std::find(unionNames.begin(), unionNames.end(), "snappy") != unionNames.end());
    const auto& replNames = registry.getReplCompressorNames();
    ASSERT_TRUE(std::find(replNames.begin(), replNames.end(), "snappy") != replNames.end());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
