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

#include <boost/move/utility_core.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <wiredtiger.h>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_event_handler.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/system_clock_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

class WiredTigerConnectionTest {
public:
    WiredTigerConnectionTest(StringData dbpath,
                             StringData extraStrings,
                             WT_EVENT_HANDLER* eventHandler = nullptr)
        : _conn(nullptr) {
        std::stringstream ss;
        ss << "create,";
        ss << extraStrings;
        std::string config = ss.str();
        _fastClockSource = std::make_unique<SystemClockSource>();
        int ret = wiredtiger_open(dbpath.toString().c_str(), eventHandler, config.c_str(), &_conn);
        ASSERT_OK(wtRCToStatus(ret, nullptr));
        ASSERT(_conn);
    }
    ~WiredTigerConnectionTest() {
        _conn->close(_conn, nullptr);
    }
    WT_CONNECTION* getConnection() const {
        return _conn;
    }
    ClockSource* getClockSource() {
        return _fastClockSource.get();
    }

private:
    WT_CONNECTION* _conn;
    std::unique_ptr<ClockSource> _fastClockSource;
};

class WiredTigerUtilHarnessHelper {
public:
    explicit WiredTigerUtilHarnessHelper(StringData extraStrings,
                                         WiredTigerEventHandler* eventHandler = nullptr)
        : _connectionTest(_dbpath.path(),
                          extraStrings,
                          eventHandler == nullptr ? nullptr : eventHandler->getWtEventHandler()),
          _connection(_connectionTest.getConnection(), _connectionTest.getClockSource()) {}

    WiredTigerConnection* getConnection() {
        return &_connection;
    }

    WiredTigerSession openSession() {
        return WiredTigerSession(getConnection()->conn());
    }

private:
    unittest::TempDir _dbpath{"wt_test"};
    WiredTigerConnectionTest _connectionTest;
    WiredTigerConnection _connection;
};

class WiredTigerUtilMetadataTest : public ServiceContextTest {
protected:
    WiredTigerUtilMetadataTest() : _harnessHelper("") {
        _ru = std::make_unique<WiredTigerRecoveryUnit>(_harnessHelper.getConnection(), nullptr);
    }

    const char* getURI() const {
        return "table:mytable";
    }

    WiredTigerRecoveryUnit* getRecoveryUnit() {
        return _ru.get();
    }

    void createSession(const char* config) {
        WT_SESSION* wtSession = _ru->getSession()->getSession();
        ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, getURI(), config), wtSession));
    }

private:
    WiredTigerUtilHarnessHelper _harnessHelper;
    std::unique_ptr<WiredTigerRecoveryUnit> _ru;
};

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateInvalid) {
    StatusWith<std::string> result =
        WiredTigerUtil::getMetadataCreate(*getRecoveryUnit(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<std::string> result =
        WiredTigerUtil::getMetadataCreate(*getRecoveryUnit(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().empty());
}

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateStringSimple) {
    const char* config = "app_metadata=(abc=123)";
    createSession(config);
    StatusWith<std::string> result =
        WiredTigerUtil::getMetadataCreate(*getRecoveryUnit(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_STRING_CONTAINS(result.getValue(), config);
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringInvalidURI) {
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(*getRecoveryUnit(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(*getRecoveryUnit(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().empty());
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringSimple) {
    const char* config = "app_metadata=(abc=123)";
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(*getRecoveryUnit(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_STRING_CONTAINS(result.getValue(), config);
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataInvalidURI) {
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(*getRecoveryUnit(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(*getRecoveryUnit(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().isEmpty());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataString) {
    const char* config = "app_metadata=\"abc\"";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(*getRecoveryUnit(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataDuplicateKeys) {
    const char* config = "app_metadata=(abc=123,abc=456)";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(*getRecoveryUnit(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(50998, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataTypes) {
    const char* config =
        "app_metadata=(stringkey=\"abc\",boolkey1=true,boolkey2=false,"
        "idkey=def,numkey=123,"
        "structkey=(k1=v2,k2=v2))";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(*getRecoveryUnit(), getURI());
    ASSERT_OK(result.getStatus());
    const BSONObj& obj = result.getValue();

    BSONElement stringElement = obj.getField("stringkey");
    ASSERT_EQUALS(mongo::String, stringElement.type());
    ASSERT_EQUALS("abc", stringElement.String());

    BSONElement boolElement1 = obj.getField("boolkey1");
    ASSERT_TRUE(boolElement1.isBoolean());
    ASSERT_TRUE(boolElement1.boolean());

    BSONElement boolElement2 = obj.getField("boolkey2");
    ASSERT_TRUE(boolElement2.isBoolean());
    ASSERT_FALSE(boolElement2.boolean());

    BSONElement identifierElement = obj.getField("idkey");
    ASSERT_EQUALS(mongo::String, identifierElement.type());
    ASSERT_EQUALS("def", identifierElement.String());

    BSONElement numberElement = obj.getField("numkey");
    ASSERT_TRUE(numberElement.isNumber());
    ASSERT_EQUALS(123, numberElement.numberInt());

    BSONElement structElement = obj.getField("structkey");
    ASSERT_EQUALS(mongo::String, structElement.type());
    ASSERT_EQUALS("(k1=v2,k2=v2)", structElement.String());
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionMissingKey) {
    createSession("app_metadata=(abc=123)");
    ASSERT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(*getRecoveryUnit(), getURI(), 1, 1));
    ASSERT_NOT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(*getRecoveryUnit(), getURI(), 2, 2));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionString) {
    createSession("app_metadata=(formatVersion=\"bar\")");
    ASSERT_NOT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(*getRecoveryUnit(), getURI(), 1, 1));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionNumber) {
    createSession("app_metadata=(formatVersion=2)");
    ASSERT_EQUALS(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(*getRecoveryUnit(), getURI(), 2, 3)
            .getValue(),
        2);
    ASSERT_NOT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(*getRecoveryUnit(), getURI(), 1, 1));
    ASSERT_NOT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(*getRecoveryUnit(), getURI(), 3, 3));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatInvalidURI) {
    createSession("\"");
    Status result =
        WiredTigerUtil::checkApplicationMetadataFormatVersion(*getRecoveryUnit(), getURI(), 0, 3)
            .getStatus();
    ASSERT_NOT_OK(result);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.code());
}

class WiredTigerUtilTest : public ServiceContextTest {};

TEST_F(WiredTigerUtilTest, GetStatisticsValueMissingTable) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getConnection(), nullptr);
    WiredTigerSession* session = ru->getSession();
    auto result = WiredTigerUtil::getStatisticsValue(
        *session, "statistics:table:no_such_table", "statistics=(fast)", WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST_F(WiredTigerUtilTest, GetStatisticsValueStatisticsDisabled) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(none)");
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getConnection(), nullptr);
    WiredTigerSession* session = ru->getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    auto result = WiredTigerUtil::getStatisticsValue(
        *session, "statistics:table:mytable", "statistics=(fast)", WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST_F(WiredTigerUtilTest, GetStatisticsValueInvalidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getConnection(), nullptr);
    WiredTigerSession* session = ru->getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    // Use connection statistics key which does not apply to a table.
    auto result = WiredTigerUtil::getStatisticsValue(
        *session, "statistics:table:mytable", "statistics=(fast)", WT_STAT_CONN_SESSION_OPEN);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilTest, GetStatisticsValueValidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getConnection(), nullptr);
    WiredTigerSession* session = ru->getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    // Use connection statistics key which does not apply to a table.
    auto result = WiredTigerUtil::getStatisticsValue(
        *session, "statistics:table:mytable", "statistics=(fast)", WT_STAT_DSRC_BTREE_ENTRIES);
    ASSERT_OK(result.getStatus());
    // Expect statistics value to be zero as there are no entries in the Btree.
    ASSERT_EQUALS(0U, result.getValue());
}

TEST_F(WiredTigerUtilTest, ParseAPIMessages) {
    // Custom event handler.
    WiredTigerEventHandler eventHandler;

    // Define a WiredTiger connection configuration that enables JSON encoding for all messages
    // related to the WT_VERB_API category.
    const std::string connection_cfg = "json_output=[error,message],verbose=[api]";

    // Initialize WiredTiger.
    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);

    // Create a session.
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getConnection(), nullptr);
    WiredTigerSession* session = ru->getSession();
    WT_SESSION* wtSession = session->getSession();

    // Perform simple WiredTiger operations while capturing the generated logs.
    startCapturingLogMessages();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:ev_api", nullptr), wtSession));
    stopCapturingLogMessages();

    // Verify there is at least one message from WiredTiger and their content.
    bool foundWTMessage = false;
    for (auto&& bson : getCapturedBSONFormatLogMessages()) {
        if (bson["c"].String() == "WT") {
            foundWTMessage = true;
            ASSERT_EQUALS(bson["attr"]["message"]["category"].String(), "WT_VERB_API");
            ASSERT_EQUALS(bson["attr"]["message"]["category_id"].Int(), WT_VERB_API);
        }
    }
    ASSERT_TRUE(foundWTMessage);
}

TEST_F(WiredTigerUtilTest, ParseCompactMessages) {
    // Custom event handler.
    WiredTigerEventHandler eventHandler;

    // Define a WiredTiger connection configuration that enables JSON encoding for all messages
    // related to the WT_VERB_COMPACT category.
    const std::string connection_cfg = "json_output=[error,message],verbose=[compact]";

    // Initialize WiredTiger.
    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);

    // Create a session.
    WiredTigerSession wtSession = harnessHelper.openSession();

    // Perform simple WiredTiger operations while capturing the generated logs.
    const std::string uri = "table:ev_compact";
    startCapturingLogMessages();
    ASSERT_OK(wtRCToStatus(wtSession.create(uri.c_str(), nullptr), wtSession));
    ASSERT_OK(wtRCToStatus(wtSession.compact(uri.c_str(), nullptr), wtSession));
    stopCapturingLogMessages();

    // Verify there is at least one message from WiredTiger and their content.
    bool foundWTMessage = false;
    for (auto&& bson : getCapturedBSONFormatLogMessages()) {
        if (bson["c"].String() == "WTCMPCT") {
            foundWTMessage = true;
            ASSERT_EQUALS(bson["attr"]["message"]["category"].String(), "WT_VERB_COMPACT");
            ASSERT_EQUALS(bson["attr"]["message"]["category_id"].Int(), WT_VERB_COMPACT);
        }
    }
    ASSERT_TRUE(foundWTMessage);
}

TEST_F(WiredTigerUtilTest, GenerateVerboseConfiguration) {
    // Perform each test in their own limited scope in order to establish different
    // severity levels.

    {
        // Set the WiredTiger Checkpoint LOGV2 component severity to the Log level.
        auto severityGuard = unittest::MinimumLoggedSeverityGuard{
            logv2::LogComponent::kWiredTigerCheckpoint, logv2::LogSeverity::Log()};
        // Generate the configuration string and verify the default severity of the
        // checkpoint component is at the Log level.
        std::string config = WiredTigerUtil::generateWTVerboseConfiguration();
        ASSERT_TRUE(config.find("checkpoint:0") != std::string::npos);
        ASSERT_TRUE(config.find("checkpoint:1") == std::string::npos);
    }
    {
        // Set the WiredTiger Checkpoint LOGV2 component severity to the Debug(2) level.
        // We want to ensure this setting is subsequently reflected in a new WiredTiger
        // verbose configuration string.
        auto severityGuard = unittest::MinimumLoggedSeverityGuard{
            logv2::LogComponent::kWiredTigerCheckpoint, logv2::LogSeverity::Debug(2)};
        std::string config = WiredTigerUtil::generateWTVerboseConfiguration();
        ASSERT_TRUE(config.find("checkpoint:2") != std::string::npos);
        ASSERT_TRUE(config.find("checkpoint:0") == std::string::npos);
    }
}

TEST_F(WiredTigerUtilTest, RemoveEncryptionFromConfigString) {
    {  // Found at the middle.
        std::string input{
            "debug_mode=(table_logging=true,checkpoint_retention=4),encryption=(name=AES256-CBC,"
            "keyid="
            "\".system\"),extensions=[local={entry=mongo_addWiredTigerEncryptors,early_load=true},,"
            "],"};
        const std::string expectedOutput{
            "debug_mode=(table_logging=true,checkpoint_retention=4),extensions=[local={entry=mongo_"
            "addWiredTigerEncryptors,early_load=true},,],"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Found at start.
        std::string input{
            "encryption=(name=AES256-CBC,keyid=\".system\"),extensions=[local={entry=mongo_"
            "addWiredTigerEncryptors,early_load=true},,],"};
        const std::string expectedOutput{
            "extensions=[local={entry=mongo_addWiredTigerEncryptors,early_load=true},,],"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Found at the end.
        std::string input{
            "debug_mode=(table_logging=true,checkpoint_retention=4),encryption=(name=AES256-CBC,"
            "keyid=\".system\")"};
        const std::string expectedOutput{"debug_mode=(table_logging=true,checkpoint_retention=4),"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Matches full configString.
        std::string input{"encryption=(name=AES256-CBC,keyid=\".system\")"};
        const std::string expectedOutput{""};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Matches full configString, trailing comma.
        std::string input{"encryption=(name=AES256-CBC,keyid=\".system\"),"};
        const std::string expectedOutput{""};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // No match.
        std::string input{"debug_mode=(table_logging=true,checkpoint_retention=4)"};
        const std::string expectedOutput{"debug_mode=(table_logging=true,checkpoint_retention=4)"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // No match, empty.
        std::string input{""};
        const std::string expectedOutput{""};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Removes multiple instances.
        std::string input{
            "encryption=(name=AES256-CBC,keyid=\".system\"),debug_mode=(table_logging=true,"
            "checkpoint_retention=4),encryption=(name=AES256-CBC,keyid=\".system\")"};
        const std::string expectedOutput{"debug_mode=(table_logging=true,checkpoint_retention=4),"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
}

TEST_F(WiredTigerUtilTest, GetSanitizedStorageOptionsForSecondaryReplication) {
    {  // Empty storage options.
        auto input = BSONObj();
        auto expectedOutput = input;
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Preserve WT config string without encryption options.
        auto input = BSON("wiredTiger" << BSON("configString"
                                               << "split_pct=88"));
        auto expectedOutput = input;
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Remove encryption options from WT config string in results.
        auto input = BSON(
            "wiredTiger" << BSON("configString"
                                 << "encryption=(name=AES256-CBC,keyid=\".system\"),split_pct=88"));
        auto expectedOutput = BSON("wiredTiger" << BSON("configString"
                                                        << "split_pct=88"));
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Leave non-WT settings intact.
        auto input = BSON("inMemory" << BSON("configString"
                                             << "split_pct=66"));
        auto expectedOutput = input;
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Change only WT settings in storage options containing a mix of WT and non-WT settings.
        auto input = BSON(
            "inMemory" << BSON("configString"
                               << "split_pct=66")
                       << "wiredTiger"
                       << BSON("configString"
                               << "encryption=(name=AES256-CBC,keyid=\".system\"),split_pct=88"));
        auto expectedOutput = BSON("inMemory" << BSON("configString"
                                                      << "split_pct=66")
                                              << "wiredTiger"
                                              << BSON("configString"
                                                      << "split_pct=88"));
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
}

TEST_F(WiredTigerUtilTest, SkipPreparedUpdateBounded) {
    // Initialize WiredTiger.
    WiredTigerEventHandler eventHandler;
    WiredTigerUtilHarnessHelper harnessHelper("", &eventHandler);

    WiredTigerSession session1 = harnessHelper.openSession();
    WiredTigerSession session2 = harnessHelper.openSession();

    const std::string uri = "table:test";
    ASSERT_OK(wtRCToStatus(session1.create(uri.c_str(), "key_format=S,value_format=S"), session1));
    WT_CURSOR* cursor1;
    ASSERT_EQ(0, session1.begin_transaction("ignore_prepare=false"));
    ASSERT_EQ(0, session1.open_cursor(uri.c_str(), nullptr, nullptr, &cursor1));
    cursor1->set_key(cursor1, "abc");
    cursor1->set_value(cursor1, "test");
    ASSERT_EQ(0, cursor1->insert(cursor1));
    session1.prepare_transaction("prepare_timestamp=1");

    WT_CURSOR* cursor2;
    ASSERT_EQ(0, session2.begin_transaction("ignore_prepare=false"));
    ASSERT_EQ(0, session2.open_cursor(uri.c_str(), nullptr, nullptr, &cursor2));

    cursor2->set_key(cursor2, "abc");
    cursor2->bound(cursor2, "bound=lower");

    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));
    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));

    session1.commit_transaction("durable_timestamp=1,commit_timestamp=1");
    ASSERT_EQ(0, cursor2->next(cursor2));
}

TEST_F(WiredTigerUtilTest, SkipPreparedUpdateNoBound) {
    // Initialize WiredTiger.
    WiredTigerEventHandler eventHandler;
    WiredTigerUtilHarnessHelper harnessHelper("", &eventHandler);

    WiredTigerSession session1 = harnessHelper.openSession();
    WiredTigerSession session2 = harnessHelper.openSession();

    const std::string uri = "table:test";
    ASSERT_OK(wtRCToStatus(session1.create(uri.c_str(), "key_format=S,value_format=S"), session1));
    WT_CURSOR* cursor1;
    ASSERT_EQ(0, session1.begin_transaction("ignore_prepare=false"));
    ASSERT_EQ(0, session1.open_cursor(uri.c_str(), nullptr, nullptr, &cursor1));
    cursor1->set_key(cursor1, "abc");
    cursor1->set_value(cursor1, "test");
    ASSERT_EQ(0, cursor1->insert(cursor1));
    session1.prepare_transaction("prepare_timestamp=1");

    WT_CURSOR* cursor2;
    ASSERT_EQ(0, session2.begin_transaction("ignore_prepare=false"));
    ASSERT_EQ(0, session2.open_cursor(uri.c_str(), nullptr, nullptr, &cursor2));

    // Continuously return WT_PREPARE_CONFLICT
    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));
    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));
    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));

    session1.commit_transaction("durable_timestamp=1,commit_timestamp=1");
    ASSERT_EQ(0, cursor2->next(cursor2));
}

TEST(WiredTigerConfigParserTest, IterationAndKeyLookup) {
    // Configuration string containing a mix of value types, including a repeated key.
    WiredTigerConfigParser parser(
        ",a=123,b=abc,c=\"def\",a=true,d=false,e=,f,g=500M,h=(x=7,y=8,z=9)");
    WT_CONFIG_ITEM key;
    WT_CONFIG_ITEM value;

    // Leading comma at the beginning of the config string is ignored.

    // a=123 ... a=true
    // Tests that getting repeated key returns the last instance.
    ASSERT_EQUALS(parser.get("a", &value), 0);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL);
    ASSERT_TRUE(value.val);

    // a=123
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "a"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM);
    ASSERT_EQUALS(value.val, 123);

    // b=abc
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "b"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(value.str, value.len), "abc"_sd);

    // c="def"
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "c"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRING);
    ASSERT_EQUALS(StringData(value.str, value.len), "def"_sd);

    // a=true
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "a"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL);
    ASSERT_TRUE(value.val);

    // d=false
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "d"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL);
    ASSERT_FALSE(value.val);

    // e=
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "e"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL);
    ASSERT_TRUE(value.val);

    // f
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "f"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL);
    ASSERT_TRUE(value.val);

    // g=500M
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "g"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM);
    ASSERT_EQUALS(value.val, 500 * 1024 * 1024);

    // h=(x=7,y=8,z=9)
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "h"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);
    ASSERT_EQUALS(StringData(value.str, value.len), "(x=7,y=8,z=9)"_sd);
    {
        WiredTigerConfigParser structParser(value);

        // x=7
        ASSERT_EQUALS(structParser.next(&key, &value), 0);
        ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
        ASSERT_EQUALS(StringData(key.str, key.len), "x"_sd);
        ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM);
        ASSERT_EQUALS(value.val, 7);

        // y=8
        ASSERT_EQUALS(structParser.next(&key, &value), 0);
        ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
        ASSERT_EQUALS(StringData(key.str, key.len), "y"_sd);
        ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM);
        ASSERT_EQUALS(value.val, 8);

        // z=9
        ASSERT_EQUALS(structParser.next(&key, &value), 0);
        ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
        ASSERT_EQUALS(StringData(key.str, key.len), "z"_sd);
        ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM);
        ASSERT_EQUALS(value.val, 9);
    }

    ASSERT_EQUALS(parser.next(&key, &value), WT_NOTFOUND);

    // a=123 ... a=true
    // Tests that, after we have iterated through all the keys in the configuration string,
    // that getting the value of a repeated key still returns the last instance.
    ASSERT_EQUALS(parser.get("a", &value), 0);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL);
    ASSERT_TRUE(value.val);
}

TEST(WiredTigerConfigParserTest, IsTableLoggingEnabled) {
    // Reject non-boolean values for "enabled".
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=12345)").isTableLoggingEnabled());
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=abc)").isTableLoggingEnabled());
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=\"def\")").isTableLoggingEnabled());
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=(a=1,b=2))").isTableLoggingEnabled());

    // Reject "log" keys without "enabled" sub-key.
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled_not_really=true)").isTableLoggingEnabled());

    // Configuration strings without "log" key.
    ASSERT_FALSE(WiredTigerConfigParser("no_log_key=123").isTableLoggingEnabled());
    ASSERT_FALSE(WiredTigerConfigParser("").isTableLoggingEnabled());

    // Test cases below expect non-optional results from isTableLoggingEnabled().
    auto assertGetEnabled = [](StringData config) {
        WiredTigerConfigParser parser(config);
        auto enabled = parser.isTableLoggingEnabled();
        ASSERT(enabled);
        return *enabled;
    };

    ASSERT(assertGetEnabled("log=(enabled=true)"));
    ASSERT(assertGetEnabled("log=(enabled=true,remove=false)"));
    ASSERT(assertGetEnabled("log=(remove=false,enabled=true)"));

    ASSERT_FALSE(assertGetEnabled("log=(enabled=false)"));
    ASSERT_FALSE(assertGetEnabled("log=(enabled=false,remove=false)"));
    ASSERT_FALSE(assertGetEnabled("log=(remove=false,enabled=false)"));

    // Only the last "enabled" sub-key of the last "log" key in the configuration string
    // is evaluated for the table logging setting queried using this helper function.
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=true),log=12345").isTableLoggingEnabled());
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=true),log=(enabled=false,enabled=123)")
                     .isTableLoggingEnabled());
    ASSERT(assertGetEnabled("log=(enabled=true,enabled=false),log=(enabled=false,enabled=true)"));
    ASSERT_FALSE(
        assertGetEnabled("log=(enabled=false,enabled=true),log=(enabled=true,enabled=false)"));
}

TEST(WiredTigerConfigParserTest, IsTableLoggingSettingValid) {
    ASSERT(WiredTigerConfigParser("log=(enabled=true)").isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("log=(enabled=false)").isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("a=123,log=(enabled=true)").isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("a=123,log=(enabled=false)").isTableLoggingSettingValid());

    // Ignore non-struct "log" values.
    ASSERT(WiredTigerConfigParser("log=123").isTableLoggingSettingValid());

    // No "log" key.
    ASSERT(WiredTigerConfigParser("x=y").isTableLoggingSettingValid());

    // Every "log" key only honors the last "enabled" subkey.
    ASSERT(WiredTigerConfigParser("log=(enabled=true),log=(enabled=true)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("a=123,log=(enabled=true),log=(enabled=true)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("log=(enabled=true),log=(enabled=true),log=(enabled=true)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser(
               "log=(enabled=true),log=(enabled=true),log=(,remove=false,enabled=true)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("log=(enabled=false),log=(enabled=false)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("a=123,log=(enabled=false),log=(enabled=false)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("log=(enabled=false),log=(enabled=false),log=(enabled=false)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser(
               "log=(enabled=false),log=(remove=false,enabled=false),log=(enabled=false)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("log=(enabled=false,enabled=true),log=(remove=false,enabled="
                                  "false,enabled=true),log=(enabled=false,enabled=true)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("log=(enabled=true,enabled=false),log=(remove=false,enabled=true,"
                                  "enabled=false),log=(enabled=true,enabled=false)")
               .isTableLoggingSettingValid());
    ASSERT_FALSE(
        WiredTigerConfigParser("log=(enabled=true,enabled=false),log=(remove=false,enabled=false,"
                               "enabled=true),log=(enabled=false,enabled=true)")
            .isTableLoggingSettingValid());
    ASSERT_FALSE(
        WiredTigerConfigParser("log=(enabled=false,enabled=true),log=(remove=false,enabled=true,"
                               "enabled=false),log=(enabled=true,enabled=false)")
            .isTableLoggingSettingValid());

    // Keys with "log" suffixes should be ignored.
    ASSERT(WiredTigerConfigParser("log=(enabled=true),some_other_log=(enabled=false)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("some_other_log=(enabled=true),log=(enabled=false)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("log=(enabled=false),some_other_log=(enabled=true)")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("some_other_log=(enabled=false),log=(enabled=true)")
               .isTableLoggingSettingValid());

    // Keys with nested "log" fields should be ignored as we are only interested in top level "log"
    // keys
    ASSERT(WiredTigerConfigParser("log=(enabled=true),mystruct=(log=(enabled=false))")
               .isTableLoggingSettingValid());
    ASSERT(WiredTigerConfigParser("log=(enabled=false),mystruct=(log=(enabled=false))")
               .isTableLoggingSettingValid());

    // Multiple "log" keys with different "enabled" values should be rejected.
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=true),log=(enabled=false)")
                     .isTableLoggingSettingValid());
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=false),log=(enabled=true)")
                     .isTableLoggingSettingValid());
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=true),log=(remove=false,enabled=false)")
                     .isTableLoggingSettingValid());
    ASSERT_FALSE(WiredTigerConfigParser("log=(enabled=false),log=(remove=false,enabled=true)")
                     .isTableLoggingSettingValid());
    ASSERT_FALSE(WiredTigerConfigParser("a=123,log=(enabled=true),log=(enabled=false)")
                     .isTableLoggingSettingValid());
    ASSERT_FALSE(WiredTigerConfigParser("a=123,log=(enabled=false),log=(enabled=true)")
                     .isTableLoggingSettingValid());
}

DEATH_TEST_REGEX(WiredTigerConfigParserTest,
                 DoesNotSupportMultipleCalls,
                 "Invariant failure.*!_nextCalled") {
    WiredTigerConfigParser parser("log=(enabled=true)");

    ASSERT(parser.isTableLoggingSettingValid());

    parser.isTableLoggingSettingValid();
}

DEATH_TEST_REGEX(WiredTigerConfigParserTest,
                 IterationAlreadyStarted,
                 "Invariant failure.*!_nextCalled") {
    WiredTigerConfigParser parser("a=123,log=(enabled=true)");

    WT_CONFIG_ITEM key;
    WT_CONFIG_ITEM value;
    ASSERT_EQUALS(parser.next(&key, &value), 0);
    ASSERT_EQUALS(key.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    ASSERT_EQUALS(StringData(key.str, key.len), "a"_sd);
    ASSERT_EQUALS(value.type, WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM);
    ASSERT_EQUALS(value.val, 123);

    parser.isTableLoggingSettingValid();
}

}  // namespace
}  // namespace mongo
