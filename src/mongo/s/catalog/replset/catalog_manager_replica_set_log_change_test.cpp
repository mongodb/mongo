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

#include <vector>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/text.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::TaskExecutor;
using stdx::async;
using unittest::assertGet;

static const stdx::chrono::seconds kFutureTimeout{5};

using LogChangeTest = CatalogManagerReplSetTestFixture;

TEST_F(LogChangeTest, NoRetryAfterSuccessfulCreate) {
    const HostAndPort configHost{"TestHost1"};
    configTargeter()->setFindHostReturnValue(configHost);

    auto future = launchAsync([this] {
        catalogManager()->logChange(
            "client", "moved a chunk", "foo.bar", BSON("min" << 3 << "max" << 4));
    });

    expectChangeLogCreate(configHost, BSON("ok" << 1));
    expectChangeLogInsert(configHost,
                          "client",
                          network()->now(),
                          "moved a chunk",
                          "foo.bar",
                          BSON("min" << 3 << "max" << 4));

    // Now wait for the logChange call to return
    future.timed_get(kFutureTimeout);

    // Now log another change and confirm that we don't re-attempt to create the collection
    future = launchAsync([this] {
        catalogManager()->logChange(
            "client", "moved a second chunk", "foo.bar", BSON("min" << 4 << "max" << 5));
    });

    expectChangeLogInsert(configHost,
                          "client",
                          network()->now(),
                          "moved a second chunk",
                          "foo.bar",
                          BSON("min" << 4 << "max" << 5));

    // Now wait for the logChange call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(LogChangeTest, NoRetryCreateIfAlreadyExists) {
    const HostAndPort configHost{"TestHost1"};
    configTargeter()->setFindHostReturnValue(configHost);

    auto future = launchAsync([this] {
        catalogManager()->logChange(
            "client", "moved a chunk", "foo.bar", BSON("min" << 3 << "max" << 4));
    });

    BSONObjBuilder createResponseBuilder;
    Command::appendCommandStatus(createResponseBuilder,
                                 Status(ErrorCodes::NamespaceExists, "coll already exists"));
    expectChangeLogCreate(configHost, createResponseBuilder.obj());
    expectChangeLogInsert(configHost,
                          "client",
                          network()->now(),
                          "moved a chunk",
                          "foo.bar",
                          BSON("min" << 3 << "max" << 4));

    // Now wait for the logAction call to return
    future.timed_get(kFutureTimeout);

    // Now log another change and confirm that we don't re-attempt to create the collection
    future = launchAsync([this] {
        catalogManager()->logChange(
            "client", "moved a second chunk", "foo.bar", BSON("min" << 4 << "max" << 5));
    });

    expectChangeLogInsert(configHost,
                          "client",
                          network()->now(),
                          "moved a second chunk",
                          "foo.bar",
                          BSON("min" << 4 << "max" << 5));

    // Now wait for the logChange call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(LogChangeTest, CreateFailure) {
    const HostAndPort configHost{"TestHost1"};
    configTargeter()->setFindHostReturnValue(configHost);

    auto future = launchAsync([this] {
        catalogManager()->logChange(
            "client", "moved a chunk", "foo.bar", BSON("min" << 3 << "max" << 4));
    });

    BSONObjBuilder createResponseBuilder;
    Command::appendCommandStatus(createResponseBuilder,
                                 Status(ErrorCodes::HostUnreachable, "socket error"));
    expectChangeLogCreate(configHost, createResponseBuilder.obj());

    // Now wait for the logAction call to return
    future.timed_get(kFutureTimeout);

    // Now log another change and confirm that we *do* attempt to create the collection
    future = launchAsync([this] {
        catalogManager()->logChange(
            "client", "moved a second chunk", "foo.bar", BSON("min" << 4 << "max" << 5));
    });

    expectChangeLogCreate(configHost, BSON("ok" << 1));
    expectChangeLogInsert(configHost,
                          "client",
                          network()->now(),
                          "moved a second chunk",
                          "foo.bar",
                          BSON("min" << 4 << "max" << 5));

    // Now wait for the logChange call to return
    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
