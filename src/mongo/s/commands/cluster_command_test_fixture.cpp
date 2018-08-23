/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/s/commands/cluster_command_test_fixture.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/util/log.h"

namespace mongo {

void ClusterCommandTestFixture::setUp() {
    CatalogCacheTestFixture::setUp();
    CatalogCacheTestFixture::setupNShards(numShards);

    // Set up a logical clock with an initial time.
    auto logicalClock = stdx::make_unique<LogicalClock>(getServiceContext());
    logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
    LogicalClock::set(getServiceContext(), std::move(logicalClock));

    auto keysCollectionClient = stdx::make_unique<KeysCollectionClientSharded>(
        Grid::get(operationContext())->catalogClient());

    auto keyManager = std::make_shared<KeysCollectionManager>(
        "dummy", std::move(keysCollectionClient), Seconds(KeysRotationIntervalSec));

    auto validator = stdx::make_unique<LogicalTimeValidator>(keyManager);
    LogicalTimeValidator::set(getServiceContext(), std::move(validator));

    LogicalSessionCache::set(getServiceContext(), stdx::make_unique<LogicalSessionCacheNoop>());
}

void ClusterCommandTestFixture::expectReturnsError(ErrorCodes::Error code) {
    onCommandForPoolExecutor([code](const executor::RemoteCommandRequest& request) {
        BSONObjBuilder resBob;
        CommandHelpers::appendCommandStatusNoThrow(resBob, Status(code, "dummy error"));
        return resBob.obj();
    });
}

DbResponse ClusterCommandTestFixture::runCommand(BSONObj cmd) {
    // Create a new client/operation context per command
    auto client = getServiceContext()->makeClient("ClusterCmdClient");
    auto opCtx = client->makeOperationContext();

    const auto opMsgRequest = OpMsgRequest::fromDBAndBody(kNss.db(), cmd);

    return Strategy::clientCommand(opCtx.get(), opMsgRequest.serialize());
}

void ClusterCommandTestFixture::runCommandSuccessful(BSONObj cmd, bool isTargeted) {
    auto future = launchAsync([&] {
        // Shouldn't throw.
        runCommand(cmd);
    });

    size_t numMocks = isTargeted ? 1 : numShards;
    for (size_t i = 0; i < numMocks; i++) {
        expectReturnsSuccess(i % numShards);
    }

    future.timed_get(kFutureTimeout);
}

void ClusterCommandTestFixture::runCommandOneError(BSONObj cmd,
                                                   ErrorCodes::Error code,
                                                   bool isTargeted) {
    auto future = launchAsync([&] {
        // Shouldn't throw.
        runCommand(cmd);
    });

    size_t numMocks = isTargeted ? 1 : numShards;
    for (size_t i = 0; i < numMocks; i++) {
        expectReturnsError(code);
    }
    for (size_t i = 0; i < numMocks; i++) {
        expectReturnsSuccess(i % numShards);
    }

    future.timed_get(kFutureTimeout);
}

void ClusterCommandTestFixture::runCommandInspectRequests(BSONObj cmd,
                                                          InspectionCallback cb,
                                                          bool isTargeted) {
    auto future = launchAsync([&] { runCommand(cmd); });

    size_t numMocks = isTargeted ? 1 : numShards;
    for (size_t i = 0; i < numMocks; i++) {
        expectInspectRequest(i % numShards, cb);
    }

    future.timed_get(kFutureTimeout);
}

void ClusterCommandTestFixture::runCommandMaxErrors(BSONObj cmd,
                                                    ErrorCodes::Error code,
                                                    bool isTargeted) {
    auto future = launchAsync([&] { runCommand(cmd); });

    size_t numRetries =
        isTargeted ? kMaxNumStaleVersionRetries : kMaxNumStaleVersionRetries * numShards;
    for (size_t i = 0; i < numRetries; i++) {
        expectReturnsError(code);
    }

    future.timed_get(kFutureTimeout);
}

}  // namespace mongo
