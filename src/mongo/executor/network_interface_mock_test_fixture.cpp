// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/network_interface_mock_test_fixture.h"

namespace mongo {
namespace executor {

// Intentionally not done in setUp in case there are methods that need to be called prior to
// starting the network.
void NetworkInterfaceMockTest::startNetwork() {
    net().startup();
    executor().startup();
}

void NetworkInterfaceMockTest::setUp() {
    _tearDownCalled = false;
}

void NetworkInterfaceMockTest::tearDown() {
    // We're calling tearDown() manually in some tests so
    // we can check post-conditions.
    if (_tearDownCalled) {
        return;
    }
    _tearDownCalled = true;

    net().exitNetwork();
    executor().shutdown();
    // Wake up sleeping executor threads so they clean up.
    net().signalWorkAvailable();
    executor().join();
    net().shutdown();
}
}  // namespace executor
}  // namespace mongo
