/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ClientTest : public unittest::Test, public ScopedGlobalServiceContextForTest {
public:
    constexpr static auto kClientName1 = "foo";
    constexpr static auto kClientName2 = "bar";

    auto makeClient(std::string desc = "ClientTest") {
        return getService()->makeClient(std::move(desc));
    }
};

TEST_F(ClientTest, UuidsAreDifferent) {
    // This test trivially asserts that the uuid for two Client instances are different. This is not
    // intended to test the efficacy of our uuid generation. Instead, this is to make sure that we
    // are not default constructing or reusing the same UUID for all Client instances.
    auto client1 = makeClient(kClientName1);
    auto client2 = makeClient(kClientName2);

    ASSERT_NE(client1->getUUID(), client2->getUUID());
}

TEST_F(ClientTest, ReportState) {
    std::string desc = "ReportStateTestClient";
    auto client = makeClient(desc);

    BSONObjBuilder bob;
    client->reportState(bob);

    auto stateObj = bob.done();
    ASSERT_EQ(stateObj.getStringField("desc"), desc);
}

TEST_F(ClientTest, InitThread) {
    ASSERT_FALSE(haveClient());
    Client::initThread("ThreadName", getService());
    ASSERT_TRUE(haveClient());
    Client::releaseCurrent();
}

TEST_F(ClientTest, SetAndReleaseCurrent) {
    auto clientUniq = makeClient();
    auto clientPtr = clientUniq.get();

    ASSERT_FALSE(haveClient());
    Client::setCurrent(std::move(clientUniq));
    ASSERT_TRUE(haveClient());
    ASSERT_EQ(Client::getCurrent(), clientPtr);

    clientUniq = Client::releaseCurrent();
    ASSERT_FALSE(haveClient());
    ASSERT_EQ(clientUniq.get(), clientPtr);
}

DEATH_TEST_REGEX_F(ClientTest, OverwriteThreadsClient, "Invariant failure.*Client1") {
    auto client1 = makeClient("Client1");
    auto client2 = makeClient("Client2");

    Client::setCurrent(std::move(client1));
    Client::setCurrent(std::move(client2));
}

}  // namespace
}  // namespace mongo
