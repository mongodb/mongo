// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/repl/initial_sync/base_cloner.h"
#include "mongo/db/repl/initial_sync/repl_sync_shared_data.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

class ClonerTestFixture : public ServiceContextMongoDTest {
public:
    ClonerTestFixture() : _storageInterface{} {}

    static BSONObj createCountResponse(int documentCount);

    // Since the DBClient handles the cursor iterating, we assume that works for the purposes of the
    // cloner unit test and just use a single batch for all mock responses.
    static BSONObj createCursorResponse(std::string_view nss, const BSONArray& docs);

protected:
    void setUp() override;

    void tearDown() override;

    StorageInterfaceMock _storageInterface;
    HostAndPort _source;
    std::unique_ptr<ThreadPool> _dbWorkThreadPool;
    std::unique_ptr<MockRemoteDBServer> _mockServer;
    std::unique_ptr<DBClientConnection> _mockClient;
    std::unique_ptr<ReplSyncSharedData> _sharedData;
    ClockSourceMock _clock;
    ServiceContext::UniqueClient _oldClient;

private:
    [[MONGO_MOD_FILE_PRIVATE]] unittest::MinimumLoggedSeverityGuard _verboseGuard{
        logv2::LogComponent::kReplicationInitialSync, logv2::LogSeverity::Debug(1)};
};

}  // namespace repl
}  // namespace mongo
