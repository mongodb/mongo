// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/cloner_test_fixture.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/util/concurrency/thread_pool.h"

#include <functional>
#include <string_view>
#include <utility>

namespace mongo {
namespace repl {

/* static */
BSONObj ClonerTestFixture::createCountResponse(int documentCount) {
    return BSON("n" << documentCount << "ok" << 1);
}

/* static */
BSONObj ClonerTestFixture::createCursorResponse(std::string_view nss, const BSONArray& docs) {
    return BSON("cursor" << BSON("id" << CursorId(0) << "ns" << nss << "firstBatch" << docs) << "ok"
                         << 1);
}

void ClonerTestFixture::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    // Release the current client and start a new client.
    _oldClient = Client::releaseCurrent();
    Client::initThread("ClonerTest", getGlobalServiceContext()->getService());

    _dbWorkThreadPool = ThreadPool::make({
        .minThreads = 1,
        .maxThreads = 1,
        .onCreateThread =
            [](std::string_view threadName) {
                Client::initThread(threadName, getGlobalServiceContext()->getService());
            },
    });

    _dbWorkThreadPool->startup();
    _source = HostAndPort{"local:1234"};
    _mockServer = std::make_unique<MockRemoteDBServer>(_source.toString());
    const bool autoReconnect = true;
    _mockClient = std::unique_ptr<DBClientConnection>(
        new MockDBClientConnection(_mockServer.get(), autoReconnect));
}

void ClonerTestFixture::tearDown() {
    _dbWorkThreadPool.reset();

    // Release the current client and restore to its old client.
    Client::releaseCurrent();
    Client::setCurrent(std::move(_oldClient));

    // Tear down mongod.
    ServiceContextMongoDTest::tearDown();
}

}  // namespace repl
}  // namespace mongo
