/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <chrono>
#include <future>
#include <vector>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/text.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::TaskExecutor;
using std::async;
using unittest::assertGet;

static const std::chrono::seconds kFutureTimeout{5};

class LogChangeTest : public CatalogManagerReplSetTestFixture {
public:
    void expectChangeLogCreate(const BSONObj& response) {
        onCommand([&response](const RemoteCommandRequest& request) {
            ASSERT_EQUALS("config", request.dbname);
            BSONObj expectedCreateCmd = BSON("create" << ChangelogType::ConfigNS << "capped" << true
                                                      << "size" << 1024 * 1024 * 10);
            ASSERT_EQUALS(expectedCreateCmd, request.cmdObj);

            return response;
        });
    }

    void expectChangeLogInsert(const ChangelogType& expectedChangeLog) {
        onCommand([&expectedChangeLog](const RemoteCommandRequest& request) {
            ASSERT_EQUALS("config", request.dbname);

            BatchedInsertRequest actualBatchedInsert;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedInsert.parseBSON(request.cmdObj, &errmsg));
            ASSERT_EQUALS(ChangelogType::ConfigNS, actualBatchedInsert.getCollName());
            auto inserts = actualBatchedInsert.getDocuments();
            ASSERT_EQUALS(1U, inserts.size());
            BSONObj insert = inserts.front();

            ChangelogType actualChangeLog;
            ASSERT_TRUE(actualChangeLog.parseBSON(insert, &errmsg));

            ASSERT_EQUALS(expectedChangeLog.getClientAddr(), actualChangeLog.getClientAddr());
            ASSERT_EQUALS(expectedChangeLog.getDetails(), actualChangeLog.getDetails());
            ASSERT_EQUALS(expectedChangeLog.getNS(), actualChangeLog.getNS());
            ASSERT_EQUALS(expectedChangeLog.getServer(), actualChangeLog.getServer());
            ASSERT_EQUALS(expectedChangeLog.getTime(), actualChangeLog.getTime());
            ASSERT_EQUALS(expectedChangeLog.getWhat(), actualChangeLog.getWhat());

            // Handle changeID specially because there's no way to know what OID was generated
            std::string changeID = actualChangeLog.getChangeID();
            size_t firstDash = changeID.find("-");
            size_t lastDash = changeID.rfind("-");
            std::string serverPiece = changeID.substr(0, firstDash);
            std::string timePiece = changeID.substr(firstDash + 1, lastDash - firstDash - 1);
            std::string oidPiece = changeID.substr(lastDash + 1);

            ASSERT_EQUALS(serverPiece, expectedChangeLog.getServer());
            ASSERT_EQUALS(timePiece, expectedChangeLog.getTime().toString());

            OID generatedOID;
            // Just make sure this doesn't throws and assume the OID is valid
            generatedOID.init(oidPiece);

            BatchedCommandResponse response;
            response.setOk(true);

            return response.toBSON();
        });
    }
};

TEST_F(LogChangeTest, LogChangeNoRetryAfterSuccessfulCreate) {
    RemoteCommandTargeterMock* targeter =
        RemoteCommandTargeterMock::get(shardRegistry()->getShard("config")->getTargeter());
    targeter->setFindHostReturnValue(HostAndPort("TestHost1"));

    ChangelogType expectedChangeLog;
    expectedChangeLog.setServer(network()->getHostName());
    expectedChangeLog.setClientAddr("client");
    expectedChangeLog.setTime(network()->now());
    expectedChangeLog.setNS("foo.bar");
    expectedChangeLog.setWhat("moved a chunk");
    expectedChangeLog.setDetails(BSON("min" << 3 << "max" << 4));

    auto future = async(std::launch::async,
                        [this, &expectedChangeLog] {
                            catalogManager()->logChange(expectedChangeLog.getClientAddr(),
                                                        expectedChangeLog.getWhat(),
                                                        expectedChangeLog.getNS(),
                                                        expectedChangeLog.getDetails());
                        });

    expectChangeLogCreate(BSON("ok" << 1));
    expectChangeLogInsert(expectedChangeLog);

    // Now wait for the logChange call to return
    future.wait_for(kFutureTimeout);

    // Now log another change and confirm that we don't re-attempt to create the collection
    future = async(std::launch::async,
                   [this, &expectedChangeLog] {
                       catalogManager()->logChange(expectedChangeLog.getClientAddr(),
                                                   expectedChangeLog.getWhat(),
                                                   expectedChangeLog.getNS(),
                                                   expectedChangeLog.getDetails());
                   });

    expectChangeLogInsert(expectedChangeLog);

    // Now wait for the logChange call to return
    future.wait_for(kFutureTimeout);
}

TEST_F(LogChangeTest, LogActionNoRetryCreateIfAlreadyExists) {
    RemoteCommandTargeterMock* targeter =
        RemoteCommandTargeterMock::get(shardRegistry()->getShard("config")->getTargeter());
    targeter->setFindHostReturnValue(HostAndPort("TestHost1"));

    ChangelogType expectedChangeLog;
    expectedChangeLog.setServer(network()->getHostName());
    expectedChangeLog.setClientAddr("client");
    expectedChangeLog.setTime(network()->now());
    expectedChangeLog.setNS("foo.bar");
    expectedChangeLog.setWhat("moved a chunk");
    expectedChangeLog.setDetails(BSON("min" << 3 << "max" << 4));

    auto future = async(std::launch::async,
                        [this, &expectedChangeLog] {
                            catalogManager()->logChange(expectedChangeLog.getClientAddr(),
                                                        expectedChangeLog.getWhat(),
                                                        expectedChangeLog.getNS(),
                                                        expectedChangeLog.getDetails());
                        });

    BSONObjBuilder createResponseBuilder;
    Command::appendCommandStatus(createResponseBuilder,
                                 Status(ErrorCodes::NamespaceExists, "coll already exists"));
    expectChangeLogCreate(createResponseBuilder.obj());
    expectChangeLogInsert(expectedChangeLog);

    // Now wait for the logAction call to return
    future.wait_for(kFutureTimeout);

    // Now log another change and confirm that we don't re-attempt to create the collection
    future = async(std::launch::async,
                   [this, &expectedChangeLog] {
                       catalogManager()->logChange(expectedChangeLog.getClientAddr(),
                                                   expectedChangeLog.getWhat(),
                                                   expectedChangeLog.getNS(),
                                                   expectedChangeLog.getDetails());
                   });

    expectChangeLogInsert(expectedChangeLog);

    // Now wait for the logChange call to return
    future.wait_for(kFutureTimeout);
}

TEST_F(LogChangeTest, LogActionCreateFailure) {
    RemoteCommandTargeterMock* targeter =
        RemoteCommandTargeterMock::get(shardRegistry()->getShard("config")->getTargeter());
    targeter->setFindHostReturnValue(HostAndPort("TestHost1"));

    ChangelogType expectedChangeLog;
    expectedChangeLog.setServer(network()->getHostName());
    expectedChangeLog.setClientAddr("client");
    expectedChangeLog.setTime(network()->now());
    expectedChangeLog.setNS("foo.bar");
    expectedChangeLog.setWhat("moved a chunk");
    expectedChangeLog.setDetails(BSON("min" << 3 << "max" << 4));

    auto future = async(std::launch::async,
                        [this, &expectedChangeLog] {
                            catalogManager()->logChange(expectedChangeLog.getClientAddr(),
                                                        expectedChangeLog.getWhat(),
                                                        expectedChangeLog.getNS(),
                                                        expectedChangeLog.getDetails());
                        });

    BSONObjBuilder createResponseBuilder;
    Command::appendCommandStatus(createResponseBuilder,
                                 Status(ErrorCodes::HostUnreachable, "socket error"));
    expectChangeLogCreate(createResponseBuilder.obj());

    // Now wait for the logAction call to return
    future.wait_for(kFutureTimeout);

    // Now log another change and confirm that we *do* attempt to create the collection
    future = async(std::launch::async,
                   [this, &expectedChangeLog] {
                       catalogManager()->logChange(expectedChangeLog.getClientAddr(),
                                                   expectedChangeLog.getWhat(),
                                                   expectedChangeLog.getNS(),
                                                   expectedChangeLog.getDetails());
                   });

    expectChangeLogCreate(BSON("ok" << 1));
    expectChangeLogInsert(expectedChangeLog);

    // Now wait for the logChange call to return
    future.wait_for(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
