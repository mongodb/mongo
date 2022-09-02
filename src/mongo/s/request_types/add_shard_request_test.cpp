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

#include "mongo/s/request_types/add_shard_request_type.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

const char kConnString[] = "setname/localhost:27017,localhost:27018,localhost:27019";
const char kConnStringNonLocalHost[] = "setname/host1:27017,host2:27017,host3:27017";
const char kShardName[] = "shardName";
const long long kMaxSizeMB = 10;

// Test parsing the internal fields from a command BSONObj. The internal fields (besides the
// top-level command name) are identical between the external mongos version and internal config
// version.

TEST(AddShardRequest, ParseInternalFieldsInvalidConnectionString) {
    {
        BSONObj obj = BSON(AddShardRequest::mongosAddShard << ",,,");

        auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(obj);
        ASSERT_NOT_OK(swAddShardRequest.getStatus());
        ASSERT_EQUALS(ErrorCodes::FailedToParse, swAddShardRequest.getStatus());
    }

    {
        BSONObj obj = BSON(AddShardRequest::configsvrAddShard << ",,,");

        auto swAddShardRequest = AddShardRequest::parseFromConfigCommand(obj);
        ASSERT_NOT_OK(swAddShardRequest.getStatus());
        ASSERT_EQUALS(ErrorCodes::FailedToParse, swAddShardRequest.getStatus());
    }
}

TEST(AddShardRequest, ParseInternalFieldsMissingMaxSize) {
    {
        BSONObj obj = BSON(AddShardRequest::mongosAddShard
                           << kConnString << AddShardRequest::shardName << kShardName);

        auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(obj);
        ASSERT_OK(swAddShardRequest.getStatus());

        auto req = swAddShardRequest.getValue();
        ASSERT_EQ(req.getConnString().toString(), kConnString);
        ASSERT_TRUE(req.hasName());
        ASSERT_EQ(req.getName(), kShardName);
        ASSERT_FALSE(req.hasMaxSize());
    }

    {
        BSONObj obj = BSON(AddShardRequest::configsvrAddShard
                           << kConnString << AddShardRequest::shardName << kShardName);


        auto swAddShardRequest = AddShardRequest::parseFromConfigCommand(obj);
        ASSERT_OK(swAddShardRequest.getStatus());

        auto req = swAddShardRequest.getValue();
        ASSERT_EQ(req.getConnString().toString(), kConnString);
        ASSERT_TRUE(req.hasName());
        ASSERT_EQ(req.getName(), kShardName);
        ASSERT_FALSE(req.hasMaxSize());
    }
}

TEST(AddShardRequest, ParseInternalFieldsMissingName) {
    {
        BSONObj obj = BSON(AddShardRequest::mongosAddShard
                           << kConnString << AddShardRequest::maxSizeMB << kMaxSizeMB);

        auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(obj);
        ASSERT_OK(swAddShardRequest.getStatus());

        auto req = swAddShardRequest.getValue();
        ASSERT_EQ(req.getConnString().toString(), kConnString);
        ASSERT_TRUE(req.hasMaxSize());
        ASSERT_EQ(req.getMaxSize(), kMaxSizeMB);
        ASSERT_FALSE(req.hasName());
    }

    {
        BSONObj obj = BSON(AddShardRequest::configsvrAddShard
                           << kConnString << AddShardRequest::maxSizeMB << kMaxSizeMB);

        auto swAddShardRequest = AddShardRequest::parseFromConfigCommand(obj);
        ASSERT_OK(swAddShardRequest.getStatus());

        auto req = swAddShardRequest.getValue();
        ASSERT_EQ(req.getConnString().toString(), kConnString);
        ASSERT_TRUE(req.hasMaxSize());
        ASSERT_EQ(req.getMaxSize(), kMaxSizeMB);
        ASSERT_FALSE(req.hasName());
    }
}

TEST(AddShardRequest, ParseInternalFieldsAllFieldsPresent) {
    {
        BSONObj obj = BSON(AddShardRequest::mongosAddShard
                           << kConnString << AddShardRequest::shardName << kShardName
                           << AddShardRequest::maxSizeMB << kMaxSizeMB);

        auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(obj);
        ASSERT_OK(swAddShardRequest.getStatus());

        auto req = swAddShardRequest.getValue();
        ASSERT_EQ(req.getConnString().toString(), kConnString);
        ASSERT_TRUE(req.hasMaxSize());
        ASSERT_EQ(req.getMaxSize(), kMaxSizeMB);
        ASSERT_TRUE(req.hasName());
        ASSERT_EQ(req.getName(), kShardName);
    }

    {
        BSONObj obj = BSON(AddShardRequest::configsvrAddShard
                           << kConnString << AddShardRequest::shardName << kShardName
                           << AddShardRequest::maxSizeMB << kMaxSizeMB);

        auto swAddShardRequest = AddShardRequest::parseFromConfigCommand(obj);
        ASSERT_OK(swAddShardRequest.getStatus());

        auto req = swAddShardRequest.getValue();
        ASSERT_EQ(req.getConnString().toString(), kConnString);
        ASSERT_TRUE(req.hasMaxSize());
        ASSERT_EQ(req.getMaxSize(), kMaxSizeMB);
        ASSERT_TRUE(req.hasName());
        ASSERT_EQ(req.getName(), kShardName);
    }
}

// Test converting a valid AddShardRequest to the internal config version of the command.

TEST(AddShardRequest, ToCommandForConfig) {
    BSONObj mongosCmdObj = BSON(AddShardRequest::mongosAddShard
                                << kConnString << AddShardRequest::shardName << kShardName
                                << AddShardRequest::maxSizeMB << kMaxSizeMB);

    auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(mongosCmdObj);
    ASSERT_OK(swAddShardRequest.getStatus());
    auto req = swAddShardRequest.getValue();

    auto configCmdObj = req.toCommandForConfig();
    ASSERT_EQ(configCmdObj[AddShardRequest::configsvrAddShard.name()].String(), kConnString);
    ASSERT_EQ(configCmdObj[AddShardRequest::shardName.name()].String(), kShardName);
    ASSERT_EQ(configCmdObj[AddShardRequest::maxSizeMB.name()].Long(), kMaxSizeMB);
}

TEST(AddShardRequest, ToCommandForConfigMissingName) {
    BSONObj mongosCmdObj = BSON(AddShardRequest::mongosAddShard
                                << kConnString << AddShardRequest::maxSizeMB << kMaxSizeMB);

    auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(mongosCmdObj);
    ASSERT_OK(swAddShardRequest.getStatus());
    auto req = swAddShardRequest.getValue();

    auto configCmdObj = req.toCommandForConfig();
    ASSERT_EQ(configCmdObj[AddShardRequest::configsvrAddShard.name()].String(), kConnString);
    ASSERT_EQ(configCmdObj[AddShardRequest::maxSizeMB.name()].Long(), kMaxSizeMB);
    ASSERT_FALSE(configCmdObj.hasField(AddShardRequest::shardName.name()));
}

TEST(AddShardRequest, ToCommandForConfigMissingMaxSize) {
    BSONObj mongosCmdObj = BSON(AddShardRequest::mongosAddShard
                                << kConnString << AddShardRequest::shardName << kShardName);

    auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(mongosCmdObj);
    ASSERT_OK(swAddShardRequest.getStatus());
    auto req = swAddShardRequest.getValue();

    auto configCmdObj = req.toCommandForConfig();
    ASSERT_EQ(configCmdObj[AddShardRequest::configsvrAddShard.name()].String(), kConnString);
    ASSERT_EQ(configCmdObj[AddShardRequest::shardName.name()].String(), kShardName);
    ASSERT_FALSE(configCmdObj.hasField(AddShardRequest::maxSizeMB.name()));
}

// Test validating an AddShardRequest that was successfully parsed.

TEST(AddShardRequest, ValidateLocalHostAllowed) {
    // Using a connection string with localhost should succeed.
    {
        BSONObj mongosCmdObj = BSON(AddShardRequest::mongosAddShard << kConnString);

        auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(mongosCmdObj);
        ASSERT_OK(swAddShardRequest.getStatus());
        auto req = swAddShardRequest.getValue();

        auto validateStatus = req.validate(true);
        ASSERT_OK(validateStatus);
    }

    // Using a connection string with non-localhost hostnames should fail.
    {
        BSONObj mongosCmdObj = BSON(AddShardRequest::mongosAddShard << kConnStringNonLocalHost);

        auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(mongosCmdObj);
        ASSERT_OK(swAddShardRequest.getStatus());
        auto req = swAddShardRequest.getValue();

        auto validateStatus = req.validate(true);
        ASSERT_NOT_OK(validateStatus);
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, validateStatus);
    }
}

TEST(AddShardRequest, ValidateLocalHostNotAllowed) {
    // Using a connection string with localhost should fail.
    {
        BSONObj mongosCmdObj = BSON(AddShardRequest::mongosAddShard << kConnString);

        auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(mongosCmdObj);
        ASSERT_OK(swAddShardRequest.getStatus());
        auto req = swAddShardRequest.getValue();

        auto validateStatus = req.validate(false);
        ASSERT_NOT_OK(validateStatus);
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, validateStatus);
    }

    // Using a connection string with non-localhost hostnames should succeed.
    {
        BSONObj mongosCmdObj = BSON(AddShardRequest::mongosAddShard << kConnStringNonLocalHost);

        auto swAddShardRequest = AddShardRequest::parseFromMongosCommand(mongosCmdObj);
        ASSERT_OK(swAddShardRequest.getStatus());
        auto req = swAddShardRequest.getValue();

        auto validateStatus = req.validate(false);
        ASSERT_OK(validateStatus);
    }
}

}  // namespace
}  // namespace mongo
