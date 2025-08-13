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

#include "mongo/db/local_catalog/shard_role_api/direct_shard_client_tracker.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class DirectShardClientTrackerTest : public ServiceContextTest {
protected:
    void checkStats(const DirectShardClientTracker& tracker, long long current, long long created) {
        BSONObjBuilder bob;
        tracker.appendStats(&bob);
        auto stats = bob.obj();

        ASSERT_EQ(stats.nFields(), 2);
        ASSERT_EQ(stats.getField(DirectShardClientTracker::kCurrentFieldName).Number(), current);
        ASSERT_EQ(stats.getField(DirectShardClientTracker::kCreatedFieldName).Number(), created);
    }
};

TEST_F(DirectShardClientTrackerTest, InitialStats) {
    auto& tracker = DirectShardClientTracker::get(getServiceContext());
    auto current = 0;
    auto created = 0;
    checkStats(tracker, current, created);
}

TEST_F(DirectShardClientTrackerTest, DestructedTrackedClient) {
    auto& tracker = DirectShardClientTracker::get(getServiceContext());
    auto current = 0;
    auto created = 0;

    {
        auto client = getServiceContext()->getService()->makeClient("DestructedTrackedClient");
        tracker.trackClient(client.get());

        current++;
        created++;
        checkStats(tracker, current, created);
    }

    current--;
    checkStats(tracker, current, created);
}

TEST_F(DirectShardClientTrackerTest, DestructedUntrackedClient) {
    auto& tracker = DirectShardClientTracker::get(getServiceContext());
    auto current = 0;
    auto created = 0;

    {
        auto client = getServiceContext()->getService()->makeClient("DestructedUntrackedClient");
        checkStats(tracker, current, created);
    }

    checkStats(tracker, current, created);
}

TEST_F(DirectShardClientTrackerTest, TrackMultipleClients) {
    auto& tracker = DirectShardClientTracker::get(getServiceContext());
    auto current = 0;
    auto created = 0;

    {
        auto client0 = getServiceContext()->getService()->makeClient("TrackMultipleClients");
        tracker.trackClient(client0.get());

        current++;
        created++;
        checkStats(tracker, current, created);
    }

    current--;
    checkStats(tracker, current, created);

    auto client1 = getServiceContext()->getService()->makeClient("TrackMultipleClients");
    tracker.trackClient(client1.get());

    current++;
    created++;
    checkStats(tracker, current, created);
}

}  // namespace
}  // namespace mongo
