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

#include "mongo/db/operation_context.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/sharding_test_fixture.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;

using ClusterGetLastErrorTest = ShardingTestFixture;

TEST_F(ClusterGetLastErrorTest,
       ClusterLastErrorInfoUpdatedIfInitializedAndReplyHasShardingMetadata) {
    auto client = operationContext()->getClient();

    // Ensure the clusterGLE on the Client has not yet been initialized.
    ASSERT(!ClusterLastErrorInfo::get(client));

    // Initialize the cluster last error info for the client with a new request.
    ClusterLastErrorInfo::get(client) = std::make_shared<ClusterLastErrorInfo>();
    ASSERT(ClusterLastErrorInfo::get(client));
    auto clusterGLE = ClusterLastErrorInfo::get(client);
    clusterGLE->newRequest();

    // Ensure the last error info is initially empty.
    ASSERT_EQUALS(0ul, clusterGLE->getPrevHostOpTimes().size());

    // Send a request over the ShardingTaskExecutor.
    HostAndPort host("fakeHost", 12345);
    auto future = launchAsync([&] {
        const RemoteCommandRequest request(host,
                                           "mydb",
                                           BSON("unusued"
                                                << "obj"),
                                           operationContext());
        executor()
            ->scheduleRemoteCommand(
                request, [=](const executor::TaskExecutor::RemoteCommandCallbackArgs) -> void {})
            .status_with_transitional_ignore();
    });

    // Make the reply contain ShardingMetadata.
    repl::OpTime opTime{Timestamp{10, 10}, 10};
    onCommandWithMetadata([&](const RemoteCommandRequest& request) {
        BSONObjBuilder metadataBob;
        rpc::ShardingMetadata(opTime, OID() /* ignored OID field */)
            .writeToMetadata(&metadataBob)
            .transitional_ignore();
        return RemoteCommandResponse(BSON("ok" << 1), metadataBob.obj(), Milliseconds(1));
    });

    future.timed_get(kFutureTimeout);

    // Ensure the last error info was updated with the contacted host and returned opTime.

    // Call newRequest() to emulate that the client then sent a getLastError() command. This is
    // required so ClusterLastErrorInfo moves its '_cur' list of hostOpTimes into its '_prev' list.
    clusterGLE->newRequest();

    ASSERT_EQUALS(1ul, clusterGLE->getPrevHostOpTimes().size());

    auto storedHostOpTimeIt = clusterGLE->getPrevHostOpTimes().begin();
    ASSERT_EQUALS(host.toString(), storedHostOpTimeIt->first.toString());
    ASSERT_EQUALS(opTime, storedHostOpTimeIt->second.opTime);
}

TEST_F(ClusterGetLastErrorTest, ClusterLastErrorInfoNotUpdatedIfNotInitialized) {
    auto client = operationContext()->getClient();

    // Ensure the clusterGLE on the Client has not been initialized.
    ASSERT(!ClusterLastErrorInfo::get(client));

    // Send a request over the ShardingTaskExecutor.
    HostAndPort host("fakeHost", 12345);
    auto future = launchAsync([&] {
        const RemoteCommandRequest request(host,
                                           "mydb",
                                           BSON("unusued"
                                                << "obj"),
                                           operationContext());
        executor()
            ->scheduleRemoteCommand(
                request, [=](const executor::TaskExecutor::RemoteCommandCallbackArgs) -> void {})
            .status_with_transitional_ignore();
    });

    // Make the reply contain ShardingMetadata.
    repl::OpTime opTime{Timestamp{10, 10}, 10};
    onCommandWithMetadata([&](const RemoteCommandRequest& request) {
        BSONObjBuilder metadataBob;
        rpc::ShardingMetadata(opTime, OID() /* ignored OID field */)
            .writeToMetadata(&metadataBob)
            .transitional_ignore();
        return RemoteCommandResponse(BSON("ok" << 1), metadataBob.obj(), Milliseconds(1));
    });

    future.timed_get(kFutureTimeout);

    // Ensure the clusterGLE on the Client has still not been initialized.
    ASSERT(!ClusterLastErrorInfo::get(client));
}

TEST_F(ClusterGetLastErrorTest, ClusterLastErrorInfoNotUpdatedIfReplyDoesntHaveShardingMetadata) {
    auto client = operationContext()->getClient();

    // Ensure the clusterGLE on the Client has not yet been initialized.
    ASSERT(!ClusterLastErrorInfo::get(client));

    // Initialize the cluster last error info for the client with a new request.
    ClusterLastErrorInfo::get(client) = std::make_shared<ClusterLastErrorInfo>();
    ASSERT(ClusterLastErrorInfo::get(client));
    auto clusterGLE = ClusterLastErrorInfo::get(client);
    clusterGLE->newRequest();

    // Ensure the last error info is initially empty.
    ASSERT_EQUALS(0ul, clusterGLE->getPrevHostOpTimes().size());

    // Send a request over the ShardingTaskExecutor.
    HostAndPort host("fakeHost", 12345);
    auto future = launchAsync([&] {
        const RemoteCommandRequest request(host,
                                           "mydb",
                                           BSON("unusued"
                                                << "obj"),
                                           operationContext());
        executor()
            ->scheduleRemoteCommand(
                request, [=](const executor::TaskExecutor::RemoteCommandCallbackArgs) -> void {})
            .status_with_transitional_ignore();
    });

    // Do not return ShardingMetadata in the reply.
    repl::OpTime opTime{Timestamp{10, 10}, 10};
    onCommandWithMetadata([&](const RemoteCommandRequest& request) {
        return RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(1));
    });

    future.timed_get(kFutureTimeout);

    // Ensure the last error info was not updated.

    // Call newRequest() to emulate that the client then sent a getLastError() command. This is
    // required so ClusterLastErrorInfo moves its '_cur' list of hostOpTimes into its '_prev' list.
    clusterGLE->newRequest();

    // Ensure the last error info is still empty.
    ASSERT_EQUALS(0ul, clusterGLE->getPrevHostOpTimes().size());
}

}  // namespace
}  // namespace mongo
