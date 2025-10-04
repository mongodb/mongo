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

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_event_handler.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/system_clock_source.h"

#include <memory>
#include <sstream>
#include <string>

#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
        int ret =
            wiredtiger_open(std::string{dbpath}.c_str(), eventHandler, config.c_str(), &_conn);
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
          _connection(_connectionTest.getConnection(),
                      _connectionTest.getClockSource(),
                      /*sessionCacheMax=*/33000) {}

    WiredTigerConnection* getConnection() {
        return &_connection;
    }

    WiredTigerSession openSession() {
        return WiredTigerSession(getConnection());
    }

private:
    unittest::TempDir _dbpath{"wt_test"};
    WiredTigerConnectionTest _connectionTest;
    WiredTigerConnection _connection;
};

class WiredTigerUtilMetadataTest : public ServiceContextTest {
protected:
    WiredTigerUtilMetadataTest() : _harnessHelper("") {}

    const char* getURI() const {
        return "table:mytable";
    }

    WiredTigerSession& getSessionNoTxn() {
        if (!_managedSession) {
            _managedSession = _harnessHelper.getConnection()->getUninterruptibleSession();
        }
        return *_managedSession;
    }

    void createSession(const char* config) {
        WiredTigerSession wtSession(_harnessHelper.getConnection());
        ASSERT_OK(wtRCToStatus(wtSession.create(getURI(), config), wtSession));
    }

private:
    WiredTigerUtilHarnessHelper _harnessHelper;
    WiredTigerManagedSession _managedSession;
};

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateInvalid) {
    StatusWith<std::string> result = WiredTigerUtil::getMetadataCreate(getSessionNoTxn(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadataCreate(getSessionNoTxn(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().empty());
}

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateStringSimple) {
    const char* config = "app_metadata=(abc=123)";
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadataCreate(getSessionNoTxn(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_STRING_CONTAINS(result.getValue(), config);
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringInvalidURI) {
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(getSessionNoTxn(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(getSessionNoTxn(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().empty());
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringSimple) {
    const char* config = "app_metadata=(abc=123)";
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(getSessionNoTxn(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_STRING_CONTAINS(result.getValue(), config);
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataInvalidURI) {
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getSessionNoTxn(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getSessionNoTxn(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().isEmpty());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataString) {
    const char* config = "app_metadata=\"abc\"";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getSessionNoTxn(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataDuplicateKeys) {
    const char* config = "app_metadata=(abc=123,abc=456)";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getSessionNoTxn(), getURI());
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
        WiredTigerUtil::getApplicationMetadata(getSessionNoTxn(), getURI());
    ASSERT_OK(result.getStatus());
    const BSONObj& obj = result.getValue();

    BSONElement stringElement = obj.getField("stringkey");
    ASSERT_EQUALS(BSONType::string, stringElement.type());
    ASSERT_EQUALS("abc", stringElement.String());

    BSONElement boolElement1 = obj.getField("boolkey1");
    ASSERT_TRUE(boolElement1.isBoolean());
    ASSERT_TRUE(boolElement1.boolean());

    BSONElement boolElement2 = obj.getField("boolkey2");
    ASSERT_TRUE(boolElement2.isBoolean());
    ASSERT_FALSE(boolElement2.boolean());

    BSONElement identifierElement = obj.getField("idkey");
    ASSERT_EQUALS(BSONType::string, identifierElement.type());
    ASSERT_EQUALS("def", identifierElement.String());

    BSONElement numberElement = obj.getField("numkey");
    ASSERT_TRUE(numberElement.isNumber());
    ASSERT_EQUALS(123, numberElement.numberInt());

    BSONElement structElement = obj.getField("structkey");
    ASSERT_EQUALS(BSONType::string, structElement.type());
    ASSERT_EQUALS("(k1=v2,k2=v2)", structElement.String());
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionMissingKey) {
    createSession("app_metadata=(abc=123)");
    ASSERT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getSessionNoTxn(), getURI(), 1, 1));
    ASSERT_NOT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getSessionNoTxn(), getURI(), 2, 2));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionString) {
    createSession("app_metadata=(formatVersion=\"bar\")");
    ASSERT_NOT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getSessionNoTxn(), getURI(), 1, 1));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionNumber) {
    createSession("app_metadata=(formatVersion=2)");
    ASSERT_EQUALS(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getSessionNoTxn(), getURI(), 2, 3)
            .getValue(),
        2);
    ASSERT_NOT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getSessionNoTxn(), getURI(), 1, 1));
    ASSERT_NOT_OK(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getSessionNoTxn(), getURI(), 3, 3));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatInvalidURI) {
    createSession("\"");
    Status result =
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getSessionNoTxn(), getURI(), 0, 3)
            .getStatus();
    ASSERT_NOT_OK(result);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.code());
}

class WiredTigerUtilTest : public ServiceContextTest {};
using WiredTigerUtilDeathTest = WiredTigerUtilTest;

TEST_F(WiredTigerUtilTest, GetStatisticsValueMissingTable) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerSession session(harnessHelper.getConnection());
    auto result = WiredTigerUtil::getStatisticsValue(
        session, "statistics:table:no_such_table", "statistics=(fast)", WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST_F(WiredTigerUtilTest, GetStatisticsValueStatisticsDisabled) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(none)");
    WiredTigerSession session(harnessHelper.getConnection());
    ASSERT_OK(wtRCToStatus(session.create("table:mytable", nullptr), session));
    auto result = WiredTigerUtil::getStatisticsValue(
        session, "statistics:table:mytable", "statistics=(fast)", WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST_F(WiredTigerUtilTest, GetStatisticsValueInvalidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerSession session(harnessHelper.getConnection());
    ASSERT_OK(wtRCToStatus(session.create("table:mytable", nullptr), session));
    // Use connection statistics key which does not apply to a table.
    auto result = WiredTigerUtil::getStatisticsValue(
        session, "statistics:table:mytable", "statistics=(fast)", WT_STAT_CONN_SESSION_OPEN);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilTest, GetStatisticsValueValidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerSession session(harnessHelper.getConnection());
    ASSERT_OK(wtRCToStatus(session.create("table:mytable", nullptr), session));
    // Use connection statistics key which does not apply to a table.
    auto result = WiredTigerUtil::getStatisticsValue(
        session, "statistics:table:mytable", "statistics=(fast)", WT_STAT_DSRC_BTREE_ENTRIES);
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
    WiredTigerSession session(harnessHelper.getConnection());

    // Perform simple WiredTiger operations while capturing the generated logs.
    unittest::LogCaptureGuard logs;
    ASSERT_OK(wtRCToStatus(session.create("table:ev_api", nullptr), session));
    logs.stop();

    // Verify there is at least one message from WiredTiger and their content.
    bool foundWTMessage = false;
    for (auto&& bson : logs.getBSON()) {
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
    unittest::LogCaptureGuard logs;
    ASSERT_OK(wtRCToStatus(wtSession.create(uri.c_str(), nullptr), wtSession));
    ASSERT_OK(wtRCToStatus(wtSession.compact(uri.c_str(), nullptr), wtSession));
    logs.stop();

    // Verify there is at least one message from WiredTiger and their content.
    bool foundWTMessage = false;
    for (auto&& bson : logs.getBSON()) {
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
        auto input = BSON("wiredTiger" << BSON("configString" << "split_pct=88"));
        auto expectedOutput = input;
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Remove encryption options from WT config string in results.
        auto input = BSON(
            "wiredTiger" << BSON("configString"
                                 << "encryption=(name=AES256-CBC,keyid=\".system\"),split_pct=88"));
        auto expectedOutput = BSON("wiredTiger" << BSON("configString" << "split_pct=88"));
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Leave non-WT settings intact.
        auto input = BSON("inMemory" << BSON("configString" << "split_pct=66"));
        auto expectedOutput = input;
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Change only WT settings in storage options containing a mix of WT and non-WT settings.
        auto input = BSON(
            "inMemory" << BSON("configString" << "split_pct=66") << "wiredTiger"
                       << BSON("configString"
                               << "encryption=(name=AES256-CBC,keyid=\".system\"),split_pct=88"));
        auto expectedOutput =
            BSON("inMemory" << BSON("configString" << "split_pct=66") << "wiredTiger"
                            << BSON("configString" << "split_pct=88"));
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

TEST_F(WiredTigerUtilTest, ExportTableToBSONFilter) {
    const std::string uri = "table:test";
    const std::vector<std::string> categoryFilter{"autocommit"};
    const std::vector<std::string> statFilter{"autocommit: retries for readonly operations"};
    // exportTableToBSON will always return the uri and version fields.
    const int baseNumFields = 2;

    // Initialize WiredTiger.
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerSession session(harnessHelper.getConnection());
    ASSERT_OK(wtRCToStatus(session.create(uri.c_str(), nullptr), session));

    // No filter
    BSONObjBuilder bob0;
    Status status =
        WiredTigerUtil::exportTableToBSON(session, "statistics:" + uri, "statistics=(fast)", bob0);
    ASSERT_OK(status);
    auto totalNumFields = bob0.obj().nFields();

    // Correctly excludes categories
    BSONObjBuilder bob1;
    status = WiredTigerUtil::exportTableToBSON(session,
                                               "statistics:" + uri,
                                               "statistics=(fast)",
                                               bob1,
                                               categoryFilter,
                                               WiredTigerUtil::FilterBehavior::kExcludeCategories);
    ASSERT_OK(status);
    auto excludeFilterRes = bob1.obj();
    ASSERT_FALSE(excludeFilterRes.hasField("autocommit"));
    ASSERT_EQ(excludeFilterRes.nFields(), totalNumFields - 1);

    // Filters that are not stats are not included
    BSONObjBuilder bob2;
    status = WiredTigerUtil::exportTableToBSON(session,
                                               "statistics:" + uri,
                                               "statistics=(fast)",
                                               bob2,
                                               categoryFilter,
                                               WiredTigerUtil::FilterBehavior::kIncludeStats);
    ASSERT_OK(status);
    ASSERT_EQ(bob2.obj().nFields(), baseNumFields);

    // Correctly includes only stats in filter
    BSONObjBuilder bob3;
    status = WiredTigerUtil::exportTableToBSON(session,
                                               "statistics:" + uri,
                                               "statistics=(fast)",
                                               bob3,
                                               statFilter,
                                               WiredTigerUtil::FilterBehavior::kIncludeStats);
    ASSERT_OK(status);
    auto insertFilterRes = bob3.obj();
    ASSERT_TRUE(insertFilterRes.getField("autocommit")["retries for readonly operations"].ok());
    ASSERT_EQ(insertFilterRes.nFields(), baseNumFields + 1);

    // Filters that are not categories are not excluded
    BSONObjBuilder bob4;
    status = WiredTigerUtil::exportTableToBSON(session,
                                               "statistics:" + uri,
                                               "statistics=(fast)",
                                               bob4,
                                               statFilter,
                                               WiredTigerUtil::FilterBehavior::kExcludeCategories);
    ASSERT_OK(status);
    ASSERT_EQ(bob4.obj().nFields(), totalNumFields);
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

TEST_F(WiredTigerUtilTest, ReconfigureBackgroundCompaction) {
    WiredTigerEventHandler eventHandler;

    // Define a WiredTiger connection configuration that enables JSON encoding for all messages
    // related to the WT_VERB_COMPACT category.
    const std::string connection_cfg =
        "json_output=[error,message],verbose=[compact],statistics=(all)";

    // Initialize WiredTiger.
    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);
    WiredTigerSession wtSession(harnessHelper.getConnection());

    // Turn on background compaction.
    const std::string uri = "table:ev_compact";
    ASSERT_OK(wtRCToStatus(wtSession.create(uri.c_str(), nullptr), wtSession));
    ASSERT_OK(wtRCToStatus(wtSession.compact(nullptr, "background=true,timeout=0"), wtSession));

    // Starting compaction requires setting up background threads, which may take some time. We
    // query WiredTiger Statistics to ensure these threads have started before we continue.
    StatusWith<int64_t> backgroundCompactionRunning = WiredTigerUtil::getStatisticsValue(
        wtSession, "statistics:", "statistics=(fast)", WT_STAT_CONN_BACKGROUND_COMPACT_RUNNING);
    ASSERT_OK(backgroundCompactionRunning);

    // A value of 0 here indicates that the background threads are not set up yet.
    while (backgroundCompactionRunning.getValue() == 0) {
        sleepmillis(100);
        backgroundCompactionRunning = WiredTigerUtil::getStatisticsValue(
            wtSession, "statistics:", "statistics=(fast)", WT_STAT_CONN_BACKGROUND_COMPACT_RUNNING);
        ASSERT_OK(backgroundCompactionRunning);
    }

    // We should get an error when we try reconfigure background compaction
    // while it is already running.
    int ret = wtSession.compact(nullptr, "background=true,timeout=0,run_once=true");
    ASSERT_EQ(EINVAL, ret);

    // WT_SESSION::get_last_error should give us more information about reconfiguring background
    // compaction while it is already running.
    WiredTigerSession::GetLastError err = wtSession.getLastError();

    ASSERT_EQUALS(EINVAL, err.err);
    ASSERT_EQUALS(WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err.sub_level_err);
    ASSERT_EQUALS("Cannot reconfigure background compaction while it's already running."_sd,
                  StringData(err.err_msg));
}

TEST_F(WiredTigerUtilTest, GetLastErrorFromSuccessfulCall) {
    WiredTigerEventHandler eventHandler;
    const std::string connection_cfg = "json_output=[error,message],verbose=[compact]";

    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);
    WiredTigerSession wtSession(harnessHelper.getConnection());


    const std::string uri = "table:get_last_err_on_success";

    ASSERT_OK(wtRCToStatus(wtSession.create(uri.c_str(), nullptr), wtSession));

    WT_CURSOR* cursor;
    ASSERT_EQUALS(0, wtSession.open_cursor(uri.c_str(), nullptr, nullptr, &cursor));

    // The previous session api call to open the cursor was successful, so we should expect no
    // error code and a WT_NONE sub-level error code
    int err, sub_level_err;
    const char* err_msg;
    wtSession.get_last_error(&err, &sub_level_err, &err_msg);

    ASSERT_EQUALS(0, err);
    ASSERT_EQUALS(WT_NONE, sub_level_err);
    ASSERT_EQUALS("last API call was successful"_sd, StringData(err_msg));
}

TEST_F(WiredTigerUtilTest, GetLastErrorFromFailedCall) {
    WiredTigerEventHandler eventHandler;
    const std::string connection_cfg = "json_output=[error,message],verbose=[compact]";

    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);
    WiredTigerSession wtSession(harnessHelper.getConnection());

    const std::string uri = "table:get_last_err_from_failed_call";

    ASSERT_OK(wtRCToStatus(wtSession.create(uri.c_str(), nullptr), wtSession));

    WT_CURSOR* cursor;
    ASSERT_NOT_EQUALS(0, wtSession.open_cursor(nullptr, nullptr, nullptr, &cursor));

    int err, sub_level_err;
    const char* err_msg;
    wtSession.get_last_error(&err, &sub_level_err, &err_msg);

    // The last session api call to open the cursor failed, but no sub-level error code is
    // defined for this case. Thus we should get EINVAL and WT_NONE as the error code, and
    // sub-level error code respectively.
    ASSERT_EQUALS(EINVAL, err);
    ASSERT_EQUALS(WT_NONE, sub_level_err);
    ASSERT_EQUALS("should be passed either a URI or a cursor to duplicate, but not both"_sd,
                  StringData(err_msg));
}

TEST_F(WiredTigerUtilTest, GetLastErrorFromLatestAPICall) {
    WiredTigerEventHandler eventHandler;
    const std::string connection_cfg = "json_output=[error,message],verbose=[compact]";

    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);
    WiredTigerSession wtSession(harnessHelper.getConnection());

    const std::string uri = "table:get_last_err_from_last_call";

    ASSERT_OK(wtRCToStatus(wtSession.create(uri.c_str(), nullptr), wtSession));

    // The last session API call to create the table was successful, so we should expect no
    // error code and a WT_NONE sub-level error code.
    int err, sub_level_err;
    const char* err_msg;
    wtSession.get_last_error(&err, &sub_level_err, &err_msg);

    ASSERT_EQUALS(0, err);
    ASSERT_EQUALS(WT_NONE, sub_level_err);
    ASSERT_EQUALS("last API call was successful"_sd, StringData(err_msg));

    WT_CURSOR* cursor;
    ASSERT_NOT_EQUALS(0, wtSession.open_cursor(nullptr, nullptr, nullptr, &cursor));

    // Now the last API call has failed, so we should make sure we get the right errors.
    wtSession.get_last_error(&err, &sub_level_err, &err_msg);
    ASSERT_EQUALS(EINVAL, err);
    ASSERT_EQUALS(WT_NONE, sub_level_err);
    ASSERT_EQUALS("should be passed either a URI or a cursor to duplicate, but not both"_sd,
                  StringData(err_msg));

    ASSERT_EQUALS(0, wtSession.open_cursor(uri.c_str(), nullptr, nullptr, &cursor));

    // Now the last API call has succeeded, so we should make sure we get no error code and a
    // WT_NONE sub-level error code.
    wtSession.get_last_error(&err, &sub_level_err, &err_msg);
    ASSERT_EQUALS(0, err);
    ASSERT_EQUALS(WT_NONE, sub_level_err);
    ASSERT_EQUALS("last API call was successful"_sd, StringData(err_msg));
}

TEST_F(WiredTigerUtilTest, CursorWriteConflict) {
    WiredTigerEventHandler eventHandler;
    const std::string connection_cfg = "json_output=[error,message],verbose=[compact]";

    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);

    WiredTigerSession session1 = harnessHelper.openSession();
    WiredTigerSession session2 = harnessHelper.openSession();

    const std::string uri = "table:cursor_write_conflict";
    ASSERT_OK(wtRCToStatus(session1.create(uri.c_str(), "key_format=S,value_format=S"), session1));

    // Session 1 uses cursor 1 to create an entry in the table.
    WT_CURSOR* cursor1;
    ASSERT_EQ(0, session1.begin_transaction("ignore_prepare=false"));
    ASSERT_EQ(0, session1.open_cursor(uri.c_str(), nullptr, nullptr, &cursor1));
    cursor1->set_key(cursor1, "abc");
    cursor1->set_value(cursor1, "test");
    ASSERT_EQ(0, cursor1->insert(cursor1));
    ASSERT_EQ(0, session1.commit_transaction(nullptr));
    ASSERT_EQ(0, cursor1->close(cursor1));

    // Session 1 opens a transaction with cursor1 but does not commit it to yield an error.
    WT_CURSOR* cursor2;
    ASSERT_EQ(0, session1.begin_transaction("ignore_prepare=false"));
    ASSERT_EQ(0, session1.open_cursor(uri.c_str(), nullptr, nullptr, &cursor2));

    cursor2->set_key(cursor2, "abc");
    cursor2->set_value(cursor2, "test");
    ASSERT_EQ(0, cursor2->update(cursor2));

    // Session 2 opens a separate transaction with cursor 2 and try write to the same key.
    WT_CURSOR* cursor3;
    ASSERT_EQ(0, session2.begin_transaction("ignore_prepare=false"));
    ASSERT_EQ(0, session2.open_cursor(uri.c_str(), nullptr, nullptr, &cursor3));

    cursor3->set_key(cursor3, "abc");
    cursor3->set_value(cursor3, "test");

    // We should get WT_ROLLBACK here because the other transaction has not been committed.
    ASSERT_EQ(WT_ROLLBACK, cursor3->update(cursor3));

    // Check that the sub-level error codes from this case is correct.
    int err, sub_level_err;
    const char* err_msg;
    session2.get_last_error(&err, &sub_level_err, &err_msg);

    ASSERT_EQUALS(WT_ROLLBACK, err);
    ASSERT_EQUALS(WT_WRITE_CONFLICT, sub_level_err);
    ASSERT_EQUALS("Write conflict between concurrent operations"_sd, StringData(err_msg));
}

TEST_F(WiredTigerUtilTest, CursorOldestForEviction) {
    // This test depends on sleeping to recreate the error, so we sleep for tryCount seconds the
    // test and retry if it fails, up till kRetryLimit seconds.
    int tryCount = 1;
    const int kRetryLimit = 5;
    do {
        WiredTigerEventHandler eventHandler;

        // Configure to have extremely low cache to force eviction.
        const std::string connection_cfg =
            "json_output=[error,message],verbose=[compact],cache_size=1MB";

        WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);

        WiredTigerSession wtSession = harnessHelper.openSession();

        const std::string uri = "table:oldest_for_eviction";

        ASSERT_OK(
            wtRCToStatus(wtSession.create(uri.c_str(), "key_format=S,value_format=S"), wtSession));

        // Start a new transaction and insert a record too large for cache.
        WT_CURSOR* cursor;
        ASSERT_EQ(0, wtSession.begin_transaction("ignore_prepare=false"));
        ASSERT_EQ(0, wtSession.open_cursor(uri.c_str(), nullptr, nullptr, &cursor));
        cursor->set_key(cursor, "dummy_key_1");
        std::string s1(1024 * 1000, 'a');
        cursor->set_value(cursor, s1.c_str());
        ASSERT_EQ(0, cursor->update(cursor));

        // Let WT's accounting catch up after the large insertion by sleeping for an increasing
        // amount of time based on how many times we've retried.
        sleepsecs(tryCount);

        // In the same transaction, try and insert a new record that would require evicting the
        // previous record trying to be inserted in this same transaction.
        cursor->set_key(cursor, "dummy_key_2");
        std::string s2(1024, 'b');
        cursor->set_value(cursor, s2.c_str());

        // We should get WT_ROLLBACK in this case because the cache has been configured to be
        // only 1MB, and is not large enough to fit both values. We might not get WT_ROLLBACK if we
        // didn't sleep for long enough, so retry with a longer sleep if needed.
        int ret = cursor->update(cursor);
        if (ret != WT_ROLLBACK) {
            tryCount++;
            continue;
        }

        // Check that the sub-level error codes from this case is correct.
        int err, sub_level_err;
        const char* err_msg;
        wtSession.get_last_error(&err, &sub_level_err, &err_msg);

        ASSERT_EQUALS(WT_ROLLBACK, err);
        ASSERT_EQUALS(WT_OLDEST_FOR_EVICTION, sub_level_err);
        ASSERT_EQUALS("Transaction has the oldest pinned transaction ID"_sd, StringData(err_msg));
        break;
    } while (tryCount <= kRetryLimit);

    // If we tried more times than the limit, then we were not able to successfully recreate the
    // error and should fail.
    ASSERT(tryCount <= kRetryLimit);
}

TEST_F(WiredTigerUtilTest, DropWithConflictingDHandle) {
    WiredTigerEventHandler eventHandler;

    const std::string connection_cfg = "json_output=[error,message],verbose=[compact]";
    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);
    WiredTigerSession wtSession = harnessHelper.openSession();

    const std::string uri = "table:conflicting_dhandle";
    ASSERT_OK(wtRCToStatus(wtSession.create(uri.c_str(), nullptr), wtSession));

    // Open and don't close the cursor.
    WT_CURSOR* cursor;
    ASSERT_EQUALS(0, wtSession.open_cursor(uri.c_str(), nullptr, nullptr, &cursor));

    // Expect the drop call to fail because the cursor is still open.
    int ret = wtSession.drop(uri.c_str(), nullptr);
    Status status = wtRCToStatus(ret, wtSession);

    ASSERT_EQUALS(EBUSY, ret);
    ASSERT_EQUALS(ErrorCodes::ObjectIsBusy, status);

    int err, sub_level_err;
    const char* err_msg;
    wtSession.get_last_error(&err, &sub_level_err, &err_msg);

    ASSERT_EQUALS(WT_CONFLICT_DHANDLE, sub_level_err);
    ASSERT_EQUALS("another thread is currently holding the data handle of the table"_sd,
                  StringData(err_msg));
}

TEST_F(WiredTigerUtilTest, DropWithUncommittedData) {
    WiredTigerEventHandler eventHandler;

    const std::string connection_cfg = "json_output=[error,message],verbose=[compact]";
    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);
    WiredTigerSession wtSession = harnessHelper.openSession();

    const std::string uri = "table:conflicting_dhandle";
    ASSERT_OK(
        wtRCToStatus(wtSession.create(uri.c_str(), "key_format=S,value_format=S"), wtSession));

    // Start a transaction to insert a key-value pair but do not commit it.
    ASSERT_OK(wtRCToStatus(wtSession.begin_transaction(nullptr), wtSession));
    WT_CURSOR* cursor;
    ASSERT_EQUALS(0, wtSession.open_cursor(uri.c_str(), nullptr, nullptr, &cursor));

    cursor->set_key(cursor, "dummy key");
    cursor->set_value(cursor, "dummy value");
    ASSERT_EQUALS(0, cursor->insert(cursor));
    ASSERT_EQUALS(0, cursor->close(cursor));

    // Expect the call to drop to fail because the transaction was not committed.
    int ret = wtSession.drop(uri.c_str(), nullptr);
    Status status = wtRCToStatus(ret, wtSession);

    ASSERT_EQUALS(EBUSY, ret);
    ASSERT_EQUALS(ErrorCodes::ObjectIsBusy, status);

    int err = 0;
    int sub_level_err = WT_NONE;
    const char* err_msg = "";
    wtSession.get_last_error(&err, &sub_level_err, &err_msg);

    ASSERT_EQUALS(WT_UNCOMMITTED_DATA, sub_level_err);
    ASSERT_EQUALS("the table has uncommitted data and cannot be dropped yet"_sd,
                  StringData(err_msg));
}

TEST_F(WiredTigerUtilTest, DropWithDirtyData) {
    WiredTigerEventHandler eventHandler;

    const std::string connection_cfg = "json_output=[error,message],verbose=[compact]";
    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);
    WiredTigerSession wtSession = harnessHelper.openSession();

    const std::string uri = "table:dirty_data";
    ASSERT_OK(
        wtRCToStatus(wtSession.create(uri.c_str(), "key_format=S,value_format=S"), wtSession));

    // Create and commit a transaction, but don't checkpoint so the data is still dirty.
    WT_CURSOR* cursor;
    ASSERT_EQUALS(0, wtSession.open_cursor(uri.c_str(), nullptr, nullptr, &cursor));
    ASSERT_OK(wtRCToStatus(wtSession.begin_transaction(nullptr), wtSession));

    cursor->set_key(cursor, "dummy key");
    cursor->set_value(cursor, "dummy value");
    ASSERT_EQUALS(0, cursor->insert(cursor));
    ASSERT_EQUALS(0, wtSession.commit_transaction(nullptr));
    ASSERT_EQUALS(0, cursor->close(cursor));

    // The transaction takes time to be committed to disk, so we may get
    // the sub-level error code WT_UNCOMMITTED_DATA if it is still committing in WT. In this case we
    // should sleep, then retry with increasing sleep time.
    int tryCount = 1;
    const int kRetryLimit = 5;
    do {
        sleepsecs(tryCount);
        int ret = wtSession.drop(uri.c_str(), nullptr);

        int err = 0;
        int sub_level_err = WT_NONE;
        const char* err_msg = "";
        wtSession.get_last_error(&err, &sub_level_err, &err_msg);

        if (sub_level_err == WT_UNCOMMITTED_DATA) {
            ++tryCount;
            continue;
        }

        // We should expect this drop to fail because the data has been committed
        // but not checkpointed.
        ASSERT_EQUALS(EBUSY, ret);
        ASSERT_EQUALS(WT_DIRTY_DATA, sub_level_err);
        ASSERT_EQUALS("the table has dirty data and can not be dropped yet"_sd,
                      StringData(err_msg));
        break;
    } while (tryCount <= kRetryLimit);

    // If we tried more times than the limit, then we were not able to successfully recreate the
    // error and should fail.
    ASSERT(tryCount <= kRetryLimit);
}

TEST(SimpleWiredTigerUtilTest, WTMainCacheSizeCalculation) {
    ProcessInfo pi;
    const double memSizeMB = pi.getMemSizeMB();
    const auto tooLargeCacheMB = 100 * 1000 * 1000;
    const auto tooLargeCachePct = 1;
    const size_t defaultCacheSizeMB = std::max((memSizeMB - 1024) * 0.5, 256.0);
    const auto maxCacheSizeMB = 10 * 1000 * 1000;

    // Testing cacheSizeGB
    ASSERT_EQUALS(WiredTigerUtil::getMainCacheSizeMB(-10, 0), defaultCacheSizeMB);
    ASSERT_EQUALS(WiredTigerUtil::getMainCacheSizeMB(0),
                  defaultCacheSizeMB); /* requestedCachePct = 0 */
    ASSERT_EQUALS(WiredTigerUtil::getMainCacheSizeMB(10, 0), 10 * 1024);
    ASSERT_EQUALS(WiredTigerUtil::getMainCacheSizeMB(tooLargeCacheMB, 0), maxCacheSizeMB);

    // Testing cacheSizePct
    ASSERT_EQUALS(WiredTigerUtil::getMainCacheSizeMB(0, -0.1), defaultCacheSizeMB);
    ASSERT_EQUALS(WiredTigerUtil::getMainCacheSizeMB(0, 0.1), std::floor(0.1 * memSizeMB));
    ASSERT_EQUALS(WiredTigerUtil::getMainCacheSizeMB(0, tooLargeCachePct),
                  std::floor(0.8 * memSizeMB));
}

DEATH_TEST_F(WiredTigerUtilDeathTest, WTMainCacheSizeInvalidValues, "invariant") {
    WiredTigerUtil::getMainCacheSizeMB(10, 0.1);
}

TEST(SimpleWiredTigerUtilTest, SpillCacheSize) {
    ASSERT_EQ(
        WiredTigerUtil::getSpillCacheSizeMB(1024 * 8, 5, 1, std::numeric_limits<int32_t>::max()),
        static_cast<int32_t>(1024 * 8 * 0.05));
    ASSERT_EQ(WiredTigerUtil::getSpillCacheSizeMB(1024 * 8, 5, 1, 100), 100);
    ASSERT_EQ(WiredTigerUtil::getSpillCacheSizeMB(1024 * 8, 0, 1, 100), 1);
    ASSERT_EQ(WiredTigerUtil::getSpillCacheSizeMB(1024 * 8, 0, 100, 100), 100);
    ASSERT_THROWS_CODE(
        WiredTigerUtil::getSpillCacheSizeMB(1024 * 8, 5, 101, 100), DBException, 10698700);
}

}  // namespace
}  // namespace mongo
