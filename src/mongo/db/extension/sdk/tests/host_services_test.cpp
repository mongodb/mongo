/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/sdk/host_services.h"

#include "mongo/db/extension/host_connector/host_services_adapter.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::extension {
namespace {

class HostServicesTest : public unittest::Test {
public:
    void setUp() override {
        sdk::HostServicesHandle::setHostServices(host_connector::HostServicesAdapter::get());
    }
};

/**
 * Tests that the MongoExtensionLogMessage created by HostServicesHandle::createLogMessageStruct
 * has the correct fields populated.
 */
TEST_F(HostServicesTest, CreateLogMessageStruct) {
    std::string logMessage = "Test log message";
    std::int32_t logCode = 12345;
    ::MongoExtensionLogSeverity logSeverity = ::MongoExtensionLogSeverity::kInfo;

    ::MongoExtensionLogMessage structuredLog =
        sdk::HostServicesHandle::createLogMessageStruct(logMessage, logCode, logSeverity);

    ASSERT_EQUALS(structuredLog.code, static_cast<uint32_t>(logCode));
    ASSERT_EQUALS(structuredLog.type, ::MongoExtensionLogType::kLog);
    ASSERT_EQUALS(structuredLog.severityOrLevel.severity, logSeverity);

    auto messageView = byteViewAsStringView(structuredLog.message);
    ASSERT_EQUALS(std::string(messageView), logMessage);
    // TODO SERVER-111339 Test attributes.
}

/**
 * Tests that the MongoExtensionLogMessage created by
 * HostServicesHandle::createDebugLogMessageStruct has the correct fields populated.
 */
TEST_F(HostServicesTest, CreateDebugLogMessageStruct) {
    std::string logMessage = "Test debug log message";
    std::int32_t logCode = 12345;
    std::int32_t logLevel = 1;

    ::MongoExtensionLogMessage structuredDebugLog =
        sdk::HostServicesHandle::createDebugLogMessageStruct(logMessage, logCode, logLevel);

    ASSERT_EQUALS(structuredDebugLog.code, static_cast<uint32_t>(logCode));
    ASSERT_EQUALS(structuredDebugLog.type, ::MongoExtensionLogType::kDebug);
    ASSERT_EQUALS(structuredDebugLog.severityOrLevel.level, logLevel);
    // TODO SERVER-111339 Test attributes.

    auto messageView = byteViewAsStringView(structuredDebugLog.message);
    ASSERT_EQUALS(std::string(messageView), logMessage);
}

TEST_F(HostServicesTest, userAsserted) {
    auto errmsg = "an error";
    int errorCode = 11111;
    BSONObj errInfo = BSON("message" << errmsg << "errorCode" << errorCode);
    ::MongoExtensionByteView errInfoByteView = objAsByteView(errInfo);

    StatusHandle status(sdk::HostServicesHandle::getHostServices()->userAsserted(errInfoByteView));

    ASSERT_EQ(status.getCode(), errorCode);
    // Reason is not populated on the status for re-throwable exceptions.
    ASSERT_EQ(status.getReason(), "");
}

DEATH_TEST_REGEX_F(HostServicesTest, tripwireAsserted, "22222") {
    auto errmsg = "fatal error";
    int errorCode = 22222;
    BSONObj errInfo = BSON("message" << errmsg << "errorCode" << errorCode);
    ::MongoExtensionByteView errInfoByteView = objAsByteView(errInfo);

    [[maybe_unused]] auto status =
        sdk::HostServicesHandle::getHostServices()->tripwireAsserted(errInfoByteView);
}

TEST_F(HostServicesTest, CreateIdLookup_ValidSpecReturnsHostNode) {
    auto bsonSpec = BSON("$_internalSearchIdLookup" << BSONObj());
    auto hostAstNode =
        extension::sdk::HostServicesHandle::getHostServices()->createIdLookup(bsonSpec);
    ASSERT_TRUE(hostAstNode.getName() ==
                std::string(DocumentSourceInternalSearchIdLookUp::kStageName));
}

TEST_F(HostServicesTest, CreateIdLookup_InvalidSpecFails) {
    auto bsonSpec = BSON("$match" << BSONObj());
    ASSERT_THROWS_CODE(
        extension::sdk::HostServicesHandle::getHostServices()->createIdLookup(bsonSpec),
        DBException,
        11134200);

    bsonSpec = BSON("$_internalSearchIdLookup" << 5);
    ASSERT_THROWS_CODE(
        extension::sdk::HostServicesHandle::getHostServices()->createIdLookup(bsonSpec),
        DBException,
        11134200);
}

}  // namespace
}  // namespace mongo::extension
