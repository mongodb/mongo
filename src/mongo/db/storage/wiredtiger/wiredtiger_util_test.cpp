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

#include <sstream>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_clock_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

using std::string;
using std::stringstream;

class WiredTigerConnection {
public:
    WiredTigerConnection(StringData dbpath,
                         StringData extraStrings,
                         WT_EVENT_HANDLER* eventHandler = nullptr)
        : _conn(nullptr) {
        std::stringstream ss;
        ss << "create,";
        ss << extraStrings;
        string config = ss.str();
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
        : _dbpath("wt_test"),
          _connection(_dbpath.path(),
                      extraStrings,
                      eventHandler == nullptr ? nullptr : eventHandler->getWtEventHandler()),
          _sessionCache(_connection.getConnection(), _connection.getClockSource()) {}


    WiredTigerSessionCache* getSessionCache() {
        return &_sessionCache;
    }

    WiredTigerOplogManager* getOplogManager() {
        return &_oplogManager;
    }

    OperationContext* newOperationContext() {
        return new OperationContextNoop(
            new WiredTigerRecoveryUnit(getSessionCache(), &_oplogManager));
    }

private:
    unittest::TempDir _dbpath;
    WiredTigerConnection _connection;
    WiredTigerSessionCache _sessionCache;
    WiredTigerOplogManager _oplogManager;
};

class WiredTigerUtilMetadataTest : public mongo::unittest::Test {
public:
    virtual void setUp() {
        _harnessHelper.reset(new WiredTigerUtilHarnessHelper(""));
        _opCtx.reset(_harnessHelper->newOperationContext());
    }

    virtual void tearDown() {
        _opCtx.reset(nullptr);
        _harnessHelper.reset(nullptr);
    }

protected:
    const char* getURI() const {
        return "table:mytable";
    }

    OperationContext* getOperationContext() const {
        ASSERT(_opCtx.get());
        return _opCtx.get();
    }

    void createSession(const char* config) {
        WT_SESSION* wtSession =
            WiredTigerRecoveryUnit::get(_opCtx.get())->getSession()->getSession();
        ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, getURI(), config), wtSession));
    }

private:
    std::unique_ptr<WiredTigerUtilHarnessHelper> _harnessHelper;
    std::unique_ptr<OperationContext> _opCtx;
};

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateInvalid) {
    StatusWith<std::string> result =
        WiredTigerUtil::getMetadataCreate(getOperationContext(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<std::string> result =
        WiredTigerUtil::getMetadataCreate(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().empty());
}

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateStringSimple) {
    const char* config = "app_metadata=(abc=123)";
    createSession(config);
    StatusWith<std::string> result =
        WiredTigerUtil::getMetadataCreate(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_STRING_CONTAINS(result.getValue(), config);
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringInvalidURI) {
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(getOperationContext(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().empty());
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringSimple) {
    const char* config = "app_metadata=(abc=123)";
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_STRING_CONTAINS(result.getValue(), config);
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataInvalidURI) {
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().isEmpty());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataString) {
    const char* config = "app_metadata=\"abc\"";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataDuplicateKeys) {
    const char* config = "app_metadata=(abc=123,abc=456)";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
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
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
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
    ASSERT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 1, 1));
    ASSERT_NOT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 2, 2));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionString) {
    createSession("app_metadata=(formatVersion=\"bar\")");
    ASSERT_NOT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 1, 1));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionNumber) {
    createSession("app_metadata=(formatVersion=2)");
    ASSERT_EQUALS(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getOperationContext(), getURI(), 2, 3)
            .getValue(),
        2);
    ASSERT_NOT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 1, 1));
    ASSERT_NOT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 3, 3));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatInvalidURI) {
    createSession("\"");
    Status result =
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getOperationContext(), getURI(), 0, 3)
            .getStatus();
    ASSERT_NOT_OK(result);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.code());
}

TEST(WiredTigerUtilTest, GetStatisticsValueMissingTable) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerRecoveryUnit recoveryUnit(harnessHelper.getSessionCache(),
                                        harnessHelper.getOplogManager());
    std::unique_ptr<OperationContext> opCtx{harnessHelper.newOperationContext()};
    recoveryUnit.setOperationContext(opCtx.get());
    WiredTigerSession* session = recoveryUnit.getSession();
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:no_such_table",
                                                     "statistics=(fast)",
                                                     WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST(WiredTigerUtilTest, GetStatisticsValueStatisticsDisabled) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(none)");
    WiredTigerRecoveryUnit recoveryUnit(harnessHelper.getSessionCache(),
                                        harnessHelper.getOplogManager());
    std::unique_ptr<OperationContext> opCtx{harnessHelper.newOperationContext()};
    recoveryUnit.setOperationContext(opCtx.get());
    WiredTigerSession* session = recoveryUnit.getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:mytable",
                                                     "statistics=(fast)",
                                                     WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST(WiredTigerUtilTest, GetStatisticsValueInvalidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerRecoveryUnit recoveryUnit(harnessHelper.getSessionCache(),
                                        harnessHelper.getOplogManager());
    std::unique_ptr<OperationContext> opCtx{harnessHelper.newOperationContext()};
    recoveryUnit.setOperationContext(opCtx.get());
    WiredTigerSession* session = recoveryUnit.getSession();
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

TEST(WiredTigerUtilTest, GetStatisticsValueValidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerRecoveryUnit recoveryUnit(harnessHelper.getSessionCache(),
                                        harnessHelper.getOplogManager());
    std::unique_ptr<OperationContext> opCtx{harnessHelper.newOperationContext()};
    recoveryUnit.setOperationContext(opCtx.get());
    WiredTigerSession* session = recoveryUnit.getSession();
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

TEST(WiredTigerUtilTest, ParseAPIMessages) {
    // Custom event handler.
    WiredTigerEventHandler eventHandler;

    // Define a WiredTiger connection configuration that enables JSON encoding for all messages
    // related to the WT_VERB_API category.
    const std::string connection_cfg = "json_output=[error,message],verbose=[api]";

    // Initialize WiredTiger.
    WiredTigerUtilHarnessHelper harnessHelper(connection_cfg.c_str(), &eventHandler);

    // Create a session.
    WiredTigerRecoveryUnit recoveryUnit(harnessHelper.getSessionCache(),
                                        harnessHelper.getOplogManager());
    std::unique_ptr<OperationContext> opCtx{harnessHelper.newOperationContext()};
    recoveryUnit.setOperationContext(opCtx.get());
    WT_SESSION* wtSession = recoveryUnit.getSession()->getSession();

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

TEST(WiredTigerUtilTest, ParseCompactMessages) {
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

TEST(WiredTigerUtilTest, GenerateVerboseConfiguration) {
    // Perform each test in their own limited scope in order to establish different
    // severity levels.

    // TODO SERVER-70651 Re-enable this test.
    return;

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
        ASSERT_TRUE(config.find("checkpoint:1") != std::string::npos);
        ASSERT_TRUE(config.find("checkpoint:0") == std::string::npos);
    }
}

}  // namespace mongo
