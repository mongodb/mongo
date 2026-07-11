// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/direct_shard_client_tracker.h"

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
