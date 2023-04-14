/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

class ClonerTestFixture : public ServiceContextMongoDTest {
public:
    ClonerTestFixture() : _storageInterface{} {}

    static BSONObj createCountResponse(int documentCount);

    // Since the DBClient handles the cursor iterating, we assume that works for the purposes of the
    // cloner unit test and just use a single batch for all mock responses.
    static BSONObj createCursorResponse(StringData nss, const BSONArray& docs);

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
    unittest::MinimumLoggedSeverityGuard _verboseGuard{logv2::LogComponent::kReplicationInitialSync,
                                                       logv2::LogSeverity::Debug(1)};
};

}  // namespace repl
}  // namespace mongo
