/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/backpressure_connection_metrics.h"
#include "mongo/transport/transport_layer_integration_test_fixture.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::transport {
namespace {

using executor::NetworkConnectionHook;

class AugmentHelloRequestHook final : public NetworkConnectionHook {
public:
    explicit AugmentHelloRequestHook(BSONObj helloFields) : _helloFields(std::move(helloFields)) {}

    BSONObj augmentHelloRequest(const HostAndPort&, BSONObj cmdObj) override {
        if (_helloFields.isEmpty()) {
            return cmdObj;
        }
        BSONObjBuilder bob(cmdObj);
        bob.appendElements(_helloFields);
        return bob.obj();
    }

    Status validateHost(const HostAndPort&,
                        const BSONObj&,
                        const executor::RemoteCommandResponse&) override {
        return Status::OK();
    }

    StatusWith<boost::optional<executor::RemoteCommandRequest>> makeRequest(
        const HostAndPort&) override {
        return {boost::none};
    }

    Status handleReply(const HostAndPort&, executor::RemoteCommandResponse&&) override {
        return Status::OK();
    }

private:
    BSONObj _helloFields;
};

int64_t counterValue(const BSONObj& counters, std::string_view label) {
    if (!counters.hasField(label)) {
        return 0;
    }
    return counters[label].safeNumberLong();
}

struct BackpressureServerStatusSnapshot {
    BSONObj activeCount;
    BSONObj totalCount;
};

BackpressureServerStatusSnapshot fetchBackpressureStats(
    const std::shared_ptr<AsyncDBClient>& client) {
    auto resp = AsyncClientIntegrationTestFixture::assertOK(
        client
            ->runCommandRequest(AsyncClientIntegrationTestFixture::makeTestRequest(
                DatabaseName::kAdmin, BSON("serverStatus" << 1)))
            .get());
    auto connections = resp["connections"].Obj();
    ASSERT(connections.hasField("backpressureVersions"));
    auto backpressureVersions = connections["backpressureVersions"].Obj();
    // Nested .Obj() views share the parent buffer; own them before resp goes out of scope.
    return BackpressureServerStatusSnapshot{
        backpressureVersions.hasField("activeCount")
            ? backpressureVersions["activeCount"].Obj().getOwned()
            : BSONObj{},
        backpressureVersions.hasField("totalCount")
            ? backpressureVersions["totalCount"].Obj().getOwned()
            : BSONObj{},
    };
}

class BackpressureConnectionMetricsIntegrationTest : public AsyncClientIntegrationTestFixture {
public:
    void setUp() override {
        AsyncClientIntegrationTestFixture::setUp();

        auto sc = getGlobalServiceContext();
        auto tl = getTransportLayer(sc);
        _reactor = tl->getReactor(TransportLayer::kNewReactor);
        _reactorThread = stdx::thread([&] {
            _reactor->run();
            _reactor->drain();
        });
        _adminClient = makeClient();
    }

    void tearDown() override {
        _testClients.clear();
        _reactor->stop();
        _reactorThread.join();
    }

    TransportLayer* getTransportLayer(ServiceContext* svc) const override {
        auto tl = svc->getTransportLayerManager()->getTransportLayer(TransportProtocol::MongoRPC);
        invariant(tl);
        return tl;
    }

    std::shared_ptr<AsyncDBClient> connectWithHelloFields(BSONObj helloFields) {
        auto svcCtx = getServiceContext();
        auto metrics = std::make_shared<ConnectionMetrics>(svcCtx->getFastClockSource());
        auto client = AsyncDBClient::connect(getServer(),
                                             kGlobalSSLMode,
                                             svcCtx,
                                             getTransportLayer(svcCtx),
                                             _reactor,
                                             Milliseconds::max(),
                                             metrics)
                          .get();
        AugmentHelloRequestHook hook(std::move(helloFields));
        client->initWireVersion(__FILE__, &hook).get();
        _testClients.push_back(client);
        return client;
    }

    int64_t currentCount(std::string_view label) const {
        return counterValue(fetchBackpressureStats(_adminClient).activeCount, label);
    }

    int64_t totalCount(std::string_view label) const {
        return counterValue(fetchBackpressureStats(_adminClient).totalCount, label);
    }

    void disconnectTestClients() {
        _testClients.clear();
    }

    /** Server session cleanup is asynchronous after client disconnect. */
    void waitFor(std::function<bool()> predicate) const {
        ASSERT_TRUE([&] {
            for (int i = 0; i < 100; ++i) {
                if (predicate()) {
                    return true;
                }
                sleepmillis(100);
            }
            return false;
        }());
    }

private:
    std::vector<std::shared_ptr<AsyncDBClient>> _testClients;
    std::shared_ptr<AsyncDBClient> _adminClient;
};

TEST_F(BackpressureConnectionMetricsIntegrationTest,
       ServerStatusReflectsInjectedBackpressureVersions) {
    // "1"/"2"/"Other" are unused by fixture clients, so baselines stay stable.
    const auto beforeVersionOneCurrent = currentCount("1");
    const auto beforeVersionOneTotal = totalCount("1");
    const auto beforeVersionTwoCurrent = currentCount("2");
    const auto beforeVersionTwoTotal = totalCount("2");
    const auto beforeOtherCurrent = currentCount(kBackpressureOtherVersionLabel);
    const auto beforeOtherTotal = totalCount(kBackpressureOtherVersionLabel);

    connectWithHelloFields(BSON("backpressure" << true));
    connectWithHelloFields(BSON("backpressure" << 2));
    connectWithHelloFields(BSON("backpressure" << "invalid"));
    connectWithHelloFields(BSON("backpressure" << (kMaxExplicitBackpressureVersion + 5)));

    ASSERT_EQ(currentCount("1"), beforeVersionOneCurrent + 1);
    ASSERT_EQ(totalCount("1"), beforeVersionOneTotal + 1);
    ASSERT_EQ(currentCount("2"), beforeVersionTwoCurrent + 1);
    ASSERT_EQ(totalCount("2"), beforeVersionTwoTotal + 1);
    ASSERT_EQ(currentCount(kBackpressureOtherVersionLabel), beforeOtherCurrent + 2);
    ASSERT_EQ(totalCount(kBackpressureOtherVersionLabel), beforeOtherTotal + 2);

    disconnectTestClients();

    // Active counts drop only after the server tears down each Session decoration.
    waitFor([&] {
        return currentCount("1") == beforeVersionOneCurrent &&
            currentCount("2") == beforeVersionTwoCurrent &&
            currentCount(kBackpressureOtherVersionLabel) == beforeOtherCurrent;
    });

    ASSERT_EQ(currentCount("1"), beforeVersionOneCurrent);
    ASSERT_EQ(totalCount("1"), beforeVersionOneTotal + 1);
    ASSERT_EQ(currentCount("2"), beforeVersionTwoCurrent);
    ASSERT_EQ(totalCount("2"), beforeVersionTwoTotal + 1);
    ASSERT_EQ(currentCount(kBackpressureOtherVersionLabel), beforeOtherCurrent);
    ASSERT_EQ(totalCount(kBackpressureOtherVersionLabel), beforeOtherTotal + 2);

    // Absent backpressure → NoBackpressure. Do not assert activeCount here: that bucket is
    // shared with fixture/resmoke clients that connect and disconnect asynchronously, so
    // absolute active deltas are inherently flaky. totalCreated is monotonic.
    const auto noBackpressureTotal = totalCount(kNoBackpressureVersionLabel);
    connectWithHelloFields(BSONObj{});
    waitFor([&] { return totalCount(kNoBackpressureVersionLabel) >= noBackpressureTotal + 1; });
    // Empty hello must not land in a versioned / Other bucket.
    ASSERT_EQ(currentCount("1"), beforeVersionOneCurrent);
    ASSERT_EQ(currentCount("2"), beforeVersionTwoCurrent);
    ASSERT_EQ(currentCount(kBackpressureOtherVersionLabel), beforeOtherCurrent);
}

}  // namespace
}  // namespace mongo::transport
