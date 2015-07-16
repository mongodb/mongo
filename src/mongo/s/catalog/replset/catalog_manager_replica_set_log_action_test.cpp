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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_actionlog.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using unittest::assertGet;

static const stdx::chrono::seconds kFutureTimeout{5};

class LogActionTest : public CatalogManagerReplSetTestFixture {
public:
    void expectActionLogCreate(const BSONObj& response) {
        onCommand([&response](const RemoteCommandRequest& request) {
            ASSERT_EQUALS("config", request.dbname);
            BSONObj expectedCreateCmd = BSON("create" << ActionLogType::ConfigNS << "capped" << true
                                                      << "size" << 1024 * 1024 * 2);
            ASSERT_EQUALS(expectedCreateCmd, request.cmdObj);

            return response;
        });
    }

    void expectActionLogInsert(const ActionLogType& expectedActionLog) {
        onCommand([&expectedActionLog](const RemoteCommandRequest& request) {
            ASSERT_EQUALS("config", request.dbname);

            BatchedInsertRequest actualBatchedInsert;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));
            ASSERT_EQUALS(ActionLogType::ConfigNS, actualBatchedInsert.getNS().ns());
            auto inserts = actualBatchedInsert.getDocuments();
            ASSERT_EQUALS(1U, inserts.size());
            BSONObj insert = inserts.front();

            auto actualActionLogRes = ActionLogType::fromBSON(insert);
            ASSERT_OK(actualActionLogRes.getStatus());
            const ActionLogType& actualActionLog = actualActionLogRes.getValue();

            ASSERT_EQUALS(expectedActionLog.toBSON(), actualActionLog.toBSON());

            BatchedCommandResponse response;
            response.setOk(true);

            return response.toBSON();
        });
    }
};

TEST_F(LogActionTest, LogActionNoRetryAfterSuccessfulCreate) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ActionLogType expectedActionLog;
    expectedActionLog.setServer("server1");
    expectedActionLog.setTime(network()->now());
    expectedActionLog.setWhat("moved a chunk");
    expectedActionLog.setDetails(boost::none, 0, 1, 1);

    auto future =
        launchAsync([this, &expectedActionLog] { catalogManager()->logAction(expectedActionLog); });

    expectActionLogCreate(BSON("ok" << 1));
    expectActionLogInsert(expectedActionLog);

    // Now wait for the logAction call to return
    future.timed_get(kFutureTimeout);

    // Now log another action and confirm that we don't re-attempt to create the collection
    future =
        launchAsync([this, &expectedActionLog] { catalogManager()->logAction(expectedActionLog); });

    expectActionLogInsert(expectedActionLog);

    // Now wait for the logAction call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(LogActionTest, LogActionNoRetryCreateIfAlreadyExists) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ActionLogType expectedActionLog;
    expectedActionLog.setServer("server1");
    expectedActionLog.setTime(network()->now());
    expectedActionLog.setWhat("moved a chunk");
    expectedActionLog.setDetails(boost::none, 0, 1, 1);

    auto future =
        launchAsync([this, &expectedActionLog] { catalogManager()->logAction(expectedActionLog); });

    BSONObjBuilder createResponseBuilder;
    Command::appendCommandStatus(createResponseBuilder,
                                 Status(ErrorCodes::NamespaceExists, "coll already exists"));
    expectActionLogCreate(createResponseBuilder.obj());
    expectActionLogInsert(expectedActionLog);

    // Now wait for the logAction call to return
    future.timed_get(kFutureTimeout);

    // Now log another action and confirm that we don't re-attempt to create the collection
    future =
        launchAsync([this, &expectedActionLog] { catalogManager()->logAction(expectedActionLog); });

    expectActionLogInsert(expectedActionLog);

    // Now wait for the logAction call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(LogActionTest, LogActionCreateFailure) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ActionLogType expectedActionLog;
    expectedActionLog.setServer("server1");
    expectedActionLog.setTime(network()->now());
    expectedActionLog.setWhat("moved a chunk");
    expectedActionLog.setDetails(boost::none, 0, 1, 1);

    auto future =
        launchAsync([this, &expectedActionLog] { catalogManager()->logAction(expectedActionLog); });

    BSONObjBuilder createResponseBuilder;
    Command::appendCommandStatus(createResponseBuilder,
                                 Status(ErrorCodes::HostUnreachable, "socket error"));
    expectActionLogCreate(createResponseBuilder.obj());
    // If creating the collection fails we won't perform the insert

    // Now wait for the logAction call to return
    future.timed_get(kFutureTimeout);

    // Now log another action and confirm that we *do* attempt to re-create the collection
    future =
        launchAsync([this, &expectedActionLog] { catalogManager()->logAction(expectedActionLog); });

    expectActionLogCreate(BSON("ok" << 1));
    expectActionLogInsert(expectedActionLog);

    // Now wait for the logAction call to return
    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
