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

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/system_clock_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

class WiredTigerConnection {
public:
    WiredTigerConnection(StringData dbpath,
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
    ~WiredTigerConnection() {
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
        : _connection(_dbpath.path(),
                      extraStrings,
                      eventHandler == nullptr ? nullptr : eventHandler->getWtEventHandler()),
          _sessionCache(_connection.getConnection(), _connection.getClockSource()) {}

    WiredTigerSessionCache* getSessionCache() {
        return &_sessionCache;
    }

    WiredTigerOplogManager* getOplogManager() {
        return &_oplogManager;
    }

private:
    unittest::TempDir _dbpath{"wt_test"};
    WiredTigerConnection _connection;
    WiredTigerSessionCache _sessionCache;
    WiredTigerOplogManager _oplogManager;
};

class WiredTigerUtilMetadataTest : public ServiceContextTest {
protected:
    WiredTigerUtilMetadataTest() : _harnessHelper("") {
        _ru = std::make_unique<WiredTigerRecoveryUnit>(_harnessHelper.getSessionCache(),
                                                       _harnessHelper.getOplogManager());
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
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getSessionCache(),
                                                       harnessHelper.getOplogManager());
    WiredTigerSession* session = ru->getSession();
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:no_such_table",
                                                     "statistics=(fast)",
                                                     WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST_F(WiredTigerUtilTest, GetStatisticsValueStatisticsDisabled) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(none)");
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getSessionCache(),
                                                       harnessHelper.getOplogManager());
    WiredTigerSession* session = ru->getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:mytable",
                                                     "statistics=(fast)",
                                                     WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST_F(WiredTigerUtilTest, GetStatisticsValueInvalidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getSessionCache(),
                                                       harnessHelper.getOplogManager());
    WiredTigerSession* session = ru->getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    // Use connection statistics key which does not apply to a table.
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:mytable",
                                                     "statistics=(fast)",
                                                     WT_STAT_CONN_SESSION_OPEN);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilTest, GetStatisticsValueValidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getSessionCache(),
                                                       harnessHelper.getOplogManager());
    WiredTigerSession* session = ru->getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    // Use connection statistics key which does not apply to a table.
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:mytable",
                                                     "statistics=(fast)",
                                                     WT_STAT_DSRC_LSM_CHUNK_COUNT);
    ASSERT_OK(result.getStatus());
    // Expect statistics value to be zero for a LSM key on a Btree.
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
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(harnessHelper.getSessionCache(),
                                                       harnessHelper.getOplogManager());
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
    WT_SESSION* wtSession;
    ASSERT_OK(
        wtRCToStatus(harnessHelper.getSessionCache()->conn()->open_session(
                         harnessHelper.getSessionCache()->conn(), nullptr, nullptr, &wtSession),
                     nullptr));

    // Perform simple WiredTiger operations while capturing the generated logs.
    const std::string uri = "table:ev_compact";
    startCapturingLogMessages();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, uri.c_str(), nullptr), wtSession));
    ASSERT_OK(wtRCToStatus(wtSession->compact(wtSession, uri.c_str(), nullptr), wtSession));
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

    WT_SESSION* session1;
    ASSERT_OK(wtRCToStatus(
        harnessHelper.getSessionCache()->conn()->open_session(
            harnessHelper.getSessionCache()->conn(), nullptr, "isolation=snapshot", &session1),
        nullptr));

    WT_SESSION* session2;
    ASSERT_OK(wtRCToStatus(
        harnessHelper.getSessionCache()->conn()->open_session(
            harnessHelper.getSessionCache()->conn(), nullptr, "isolation=snapshot", &session2),
        nullptr));


    const std::string uri = "table:test";
    ASSERT_OK(wtRCToStatus(session1->create(session1, uri.c_str(), "key_format=S,value_format=S"),
                           session1));
    WT_CURSOR* cursor1;
    ASSERT_EQ(0, session1->begin_transaction(session1, "ignore_prepare=false"));
    ASSERT_EQ(0, session1->open_cursor(session1, uri.c_str(), nullptr, nullptr, &cursor1));
    cursor1->set_key(cursor1, "abc");
    cursor1->set_value(cursor1, "test");
    ASSERT_EQ(0, cursor1->insert(cursor1));
    session1->prepare_transaction(session1, "prepare_timestamp=1");

    WT_CURSOR* cursor2;
    ASSERT_EQ(0, session2->begin_transaction(session2, "ignore_prepare=false"));
    ASSERT_EQ(0, session2->open_cursor(session2, uri.c_str(), nullptr, nullptr, &cursor2));

    cursor2->set_key(cursor2, "abc");
    cursor2->bound(cursor2, "bound=lower");

    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));
    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));

    session1->commit_transaction(session1, "durable_timestamp=1,commit_timestamp=1");
    ASSERT_EQ(0, cursor2->next(cursor2));
}

TEST_F(WiredTigerUtilTest, SkipPreparedUpdateNoBound) {
    // Initialize WiredTiger.
    WiredTigerEventHandler eventHandler;
    WiredTigerUtilHarnessHelper harnessHelper("", &eventHandler);

    WT_SESSION* session1;
    ASSERT_OK(wtRCToStatus(
        harnessHelper.getSessionCache()->conn()->open_session(
            harnessHelper.getSessionCache()->conn(), nullptr, "isolation=snapshot", &session1),
        nullptr));

    WT_SESSION* session2;
    ASSERT_OK(wtRCToStatus(
        harnessHelper.getSessionCache()->conn()->open_session(
            harnessHelper.getSessionCache()->conn(), nullptr, "isolation=snapshot", &session2),
        nullptr));


    const std::string uri = "table:test";
    ASSERT_OK(wtRCToStatus(session1->create(session1, uri.c_str(), "key_format=S,value_format=S"),
                           session1));
    WT_CURSOR* cursor1;
    ASSERT_EQ(0, session1->begin_transaction(session1, "ignore_prepare=false"));
    ASSERT_EQ(0, session1->open_cursor(session1, uri.c_str(), nullptr, nullptr, &cursor1));
    cursor1->set_key(cursor1, "abc");
    cursor1->set_value(cursor1, "test");
    ASSERT_EQ(0, cursor1->insert(cursor1));
    session1->prepare_transaction(session1, "prepare_timestamp=1");

    WT_CURSOR* cursor2;
    ASSERT_EQ(0, session2->begin_transaction(session2, "ignore_prepare=false"));
    ASSERT_EQ(0, session2->open_cursor(session2, uri.c_str(), nullptr, nullptr, &cursor2));

    // Continuously return WT_PREPARE_CONFLICT
    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));
    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));
    ASSERT_EQ(WT_PREPARE_CONFLICT, cursor2->next(cursor2));

    session1->commit_transaction(session1, "durable_timestamp=1,commit_timestamp=1");
    ASSERT_EQ(0, cursor2->next(cursor2));
}

}  // namespace
}  // namespace mongo
