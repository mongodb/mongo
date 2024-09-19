/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <memory>

#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

namespace mongo::transport {
namespace {

class TransportLayerManagerTest : public unittest::Test {
public:
    class TransportLayerMockWithConnect : public TransportLayerMock {
    public:
        StatusWith<std::shared_ptr<Session>> connect(
            HostAndPort peer,
            ConnectSSLMode sslMode,
            Milliseconds timeout,
            const boost::optional<TransientSSLParams>& transientSSLParams = boost::none) override {
            return createSession();
        }

        bool isStarted() const {
            return _started;
        }

        Status start() override {
            uassert(7402201,
                    "cannot start a transport layer more than once",
                    !std::exchange(_started, true));
            return Status::OK();
        }

    private:
        bool _started = false;
    };
};

TEST_F(TransportLayerManagerTest, StartAndShutdown) {
    std::vector<TransportLayerMockWithConnect*> layerPtrs;
    std::vector<std::unique_ptr<TransportLayer>> layers;
    for (int i = 0; i < 5; i++) {
        auto layer = std::make_unique<TransportLayerMockWithConnect>();
        layerPtrs.push_back(layer.get());
        layers.push_back(std::move(layer));
    }

    TransportLayerManagerImpl manager(std::move(layers), layerPtrs[0]);
    ASSERT_OK(manager.setup());
    ASSERT_OK(manager.start());
    for (auto layer : layerPtrs) {
        ASSERT_TRUE(layer->isStarted());
    }

    manager.shutdown();
    for (auto layer : layerPtrs) {
        ASSERT_TRUE(layer->inShutdown());
    }
}

TEST_F(TransportLayerManagerTest, ConnectEgressLayer) {
    std::vector<std::unique_ptr<TransportLayer>> layers;

    auto egress = std::make_unique<TransportLayerMockWithConnect>();
    auto egressPtr = egress.get();
    layers.push_back(std::move(egress));
    layers.push_back(std::make_unique<TransportLayerMock>());

    TransportLayerManagerImpl manager(std::move(layers), egressPtr);
    uassertStatusOK(manager.setup());
    uassertStatusOK(manager.start());
    auto swSession = manager.getEgressLayer()->connect(
        HostAndPort("localhost:1234"), ConnectSSLMode::kDisableSSL, Milliseconds(100), boost::none);
    ASSERT_OK(swSession);
    ASSERT_TRUE(egressPtr->owns(swSession.getValue()->id()));
}

}  // namespace
}  // namespace mongo::transport
