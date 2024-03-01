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

// IWYU pragma: no_include "cxxabi.h"
#include <string>
#include <system_error>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using unittest::assertGet;

const auto kFooBarNss = NamespaceString::createNamespaceString_forTest(boost::none, "foo.bar");

class InfoLoggingTest : public ShardServerTestFixture {
protected:
    enum CollType { ActionLog, ChangeLog };

    InfoLoggingTest(CollType configCollType, int cappedSize)
        : _configCollType(configCollType), _cappedSize(cappedSize) {}

    /**
     * Waits for an operation which creates a capped config collection with the specified name and
     * capped size.
     */
    void expectConfigCollectionCreate(const HostAndPort& configHost,
                                      StringData collName,
                                      int cappedSize,
                                      const BSONObj& response) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            ASSERT_EQUALS(DatabaseName::kConfig, request.dbname);

            BSONObj expectedCreateCmd = BSON("create" << collName << "capped" << true << "size"
                                                      << cappedSize << "writeConcern"
                                                      << BSON("w"
                                                              << "majority"
                                                              << "wtimeout" << 60000)
                                                      << "maxTimeMS" << 30000);
            ASSERT_BSONOBJ_EQ(expectedCreateCmd, request.cmdObj);

            return response;
        });
    }

    /**
     * Wait for a single insert in one of the change or action log collections with the specified
     * contents and return a successful response.
     */
    void expectConfigCollectionInsert(const HostAndPort& configHost,
                                      StringData collName,
                                      Date_t timestamp,
                                      const std::string& what,
                                      const NamespaceString& ns,
                                      const BSONObj& detail) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            ASSERT_EQUALS(DatabaseName::kConfig, request.dbname);

            const auto opMsg = static_cast<OpMsgRequest>(request);
            const auto batchRequest(BatchedCommandRequest::parseInsert(opMsg));
            const auto& insertReq(batchRequest.getInsertRequest());

            ASSERT_EQ(DatabaseName::kConfig.db(omitTenant), insertReq.getNamespace().db_forTest());
            ASSERT_EQ(collName, insertReq.getNamespace().coll());

            const auto& inserts = insertReq.getDocuments();
            ASSERT_EQUALS(1U, inserts.size());

            const ChangeLogType& actualChangeLog =
                assertGet(ChangeLogType::fromBSON(inserts.front()));

            ASSERT_EQUALS(operationContext()->getClient()->clientAddress(true),
                          actualChangeLog.getClientAddr());
            ASSERT_BSONOBJ_EQ(detail, actualChangeLog.getDetails());
            ASSERT_EQUALS(ns, actualChangeLog.getNS());
            const std::string expectedServer = network()->getHostName();
            ASSERT_EQUALS(expectedServer, actualChangeLog.getServer());
            ASSERT_EQUALS(timestamp, actualChangeLog.getTime());
            ASSERT_EQUALS(what, actualChangeLog.getWhat());

            // Handle changeId specially because there's no way to know what OID was generated
            std::string changeId = actualChangeLog.getChangeId();
            size_t firstDash = changeId.find('-');
            size_t lastDash = changeId.rfind('-');

            const std::string serverPiece = changeId.substr(0, firstDash);
            const std::string timePiece = changeId.substr(firstDash + 1, lastDash - firstDash - 1);
            const std::string oidPiece = changeId.substr(lastDash + 1);

            const std::string expectedServerPiece =
                Grid::get(operationContext())->getNetwork()->getHostName();
            ASSERT_EQUALS(expectedServerPiece, serverPiece);
            ASSERT_EQUALS(timestamp.toString(), timePiece);

            OID generatedOID;
            // Just make sure this doesn't throws and assume the OID is valid
            generatedOID.init(oidPiece);

            BatchedCommandResponse response;
            response.setStatus(Status::OK());

            return response.toBSON();
        });
    }

    void noRetryAfterSuccessfulCreate() {
        auto future = launchAsync([this] {
            ASSERT_OK(log("moved a chunk", kFooBarNss, BSON("min" << 3 << "max" << 4)));
        });

        expectConfigCollectionCreate(
            kConfigHostAndPort, getConfigCollName(), _cappedSize, BSON("ok" << 1));
        expectConfigCollectionInsert(kConfigHostAndPort,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a chunk",
                                     kFooBarNss,
                                     BSON("min" << 3 << "max" << 4));

        // Now wait for the logChange call to return
        future.default_timed_get();

        // Now log another change and confirm that we don't re-attempt to create the collection
        future = launchAsync([this] {
            ASSERT_OK(log("moved a second chunk", kFooBarNss, BSON("min" << 4 << "max" << 5)));
        });

        expectConfigCollectionInsert(kConfigHostAndPort,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a second chunk",
                                     kFooBarNss,
                                     BSON("min" << 4 << "max" << 5));

        // Now wait for the logChange call to return
        future.default_timed_get();
    }

    void noRetryCreateIfAlreadyExists() {
        auto future = launchAsync([this] {
            ASSERT_OK(log("moved a chunk", kFooBarNss, BSON("min" << 3 << "max" << 4)));
        });

        BSONObjBuilder createResponseBuilder;
        CommandHelpers::appendCommandStatusNoThrow(
            createResponseBuilder, Status(ErrorCodes::NamespaceExists, "coll already exists"));
        expectConfigCollectionCreate(
            kConfigHostAndPort, getConfigCollName(), _cappedSize, createResponseBuilder.obj());
        expectConfigCollectionInsert(kConfigHostAndPort,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a chunk",
                                     kFooBarNss,
                                     BSON("min" << 3 << "max" << 4));

        // Now wait for the logAction call to return
        future.default_timed_get();

        // Now log another change and confirm that we don't re-attempt to create the collection
        future = launchAsync([this] {
            log("moved a second chunk", kFooBarNss, BSON("min" << 4 << "max" << 5))
                .transitional_ignore();
        });

        expectConfigCollectionInsert(kConfigHostAndPort,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a second chunk",
                                     kFooBarNss,
                                     BSON("min" << 4 << "max" << 5));

        // Now wait for the logChange call to return
        future.default_timed_get();
    }

    void createFailure() {
        auto future = launchAsync([this] {
            log("moved a chunk", kFooBarNss, BSON("min" << 3 << "max" << 4)).transitional_ignore();
        });

        BSONObjBuilder createResponseBuilder;
        CommandHelpers::appendCommandStatusNoThrow(
            createResponseBuilder, Status(ErrorCodes::Interrupted, "operation interrupted"));
        expectConfigCollectionCreate(
            kConfigHostAndPort, getConfigCollName(), _cappedSize, createResponseBuilder.obj());

        // Now wait for the logAction call to return
        future.default_timed_get();

        // Now log another change and confirm that we *do* attempt to create the collection
        future = launchAsync([this] {
            log("moved a second chunk", kFooBarNss, BSON("min" << 4 << "max" << 5))
                .transitional_ignore();
        });

        expectConfigCollectionCreate(
            kConfigHostAndPort, getConfigCollName(), _cappedSize, BSON("ok" << 1));
        expectConfigCollectionInsert(kConfigHostAndPort,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a second chunk",
                                     kFooBarNss,
                                     BSON("min" << 4 << "max" << 5));

        // Now wait for the logChange call to return
        future.default_timed_get();
    }

    std::string getConfigCollName() const {
        return (_configCollType == ChangeLog ? "changelog" : "actionlog");
    }

    Status log(const std::string& what, const NamespaceString& ns, const BSONObj& detail) {
        if (_configCollType == ChangeLog) {
            return ShardingLogging::get(operationContext())
                ->logChangeChecked(operationContext(),
                                   what,
                                   ns,
                                   detail,
                                   ShardingCatalogClient::kMajorityWriteConcern);
        } else {
            return ShardingLogging::get(operationContext())
                ->logAction(operationContext(), what, ns, detail);
        }
    }

private:
    const CollType _configCollType;
    const int _cappedSize;
};

class ActionLogTest : public InfoLoggingTest {
protected:
    ActionLogTest() : InfoLoggingTest(ActionLog, 20 * 1024 * 1024) {}
};

class ChangeLogTest : public InfoLoggingTest {
protected:
    ChangeLogTest() : InfoLoggingTest(ChangeLog, 200 * 1024 * 1024) {}
};

TEST_F(ActionLogTest, NoRetryAfterSuccessfulCreate) {
    noRetryAfterSuccessfulCreate();
}
TEST_F(ChangeLogTest, NoRetryAfterSuccessfulCreate) {
    noRetryAfterSuccessfulCreate();
}

TEST_F(ActionLogTest, NoRetryCreateIfAlreadyExists) {
    noRetryCreateIfAlreadyExists();
}
TEST_F(ChangeLogTest, NoRetryCreateIfAlreadyExists) {
    noRetryCreateIfAlreadyExists();
}

TEST_F(ActionLogTest, CreateFailure) {
    createFailure();
}
TEST_F(ChangeLogTest, CreateFailure) {
    createFailure();
}

}  // namespace
}  // namespace mongo
