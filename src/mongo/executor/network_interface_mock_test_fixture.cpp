/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

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