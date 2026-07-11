// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/routing_cache/namespace_metadata_change_notifications.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("foo.bar");

class NamespaceMetadataChangeNotificationsTest : public ServiceContextMongoDTest {
protected:
    NamespaceMetadataChangeNotificationsTest()
        : ServiceContextMongoDTest(Options{}.useMockTickSource(true)) {}
};

TEST_F(NamespaceMetadataChangeNotificationsTest, WaitForNotify) {
    NamespaceMetadataChangeNotifications notifications;

    auto scopedNotif = notifications.createNotification(kNss);

    {
        auto opCtx = getClient()->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(notifications.get(opCtx.get(), scopedNotif),
                           AssertionException,
                           ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss, {Timestamp(2, 1)});

    {
        auto opCtx = getClient()->makeOperationContext();
        notifications.get(opCtx.get(), scopedNotif);
    }
}

TEST_F(NamespaceMetadataChangeNotificationsTest, GiveUpWaitingForNotify) {
    NamespaceMetadataChangeNotifications notifications;

    {
        auto scopedNotif = notifications.createNotification(kNss);

        auto opCtx = getClient()->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(notifications.get(opCtx.get(), scopedNotif),
                           AssertionException,
                           ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss, {Timestamp(2, 1)});
}

TEST_F(NamespaceMetadataChangeNotificationsTest, MoveConstructionWaitForNotify) {
    NamespaceMetadataChangeNotifications notifications;

    auto scopedNotif = notifications.createNotification(kNss);
    auto movedScopedNotif = std::move(scopedNotif);

    {
        auto opCtx = getClient()->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(notifications.get(opCtx.get(), movedScopedNotif),
                           AssertionException,
                           ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss, {Timestamp(2, 1)});

    {
        auto opCtx = getClient()->makeOperationContext();
        ASSERT_EQ(notifications.get(opCtx.get(), movedScopedNotif), Timestamp(2, 1));
    }
}

TEST_F(NamespaceMetadataChangeNotificationsTest, NotifyTwice) {
    NamespaceMetadataChangeNotifications notifications;

    auto scopedNotif = notifications.createNotification(kNss);

    {
        auto opCtx = getClient()->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(notifications.get(opCtx.get(), scopedNotif),
                           AssertionException,
                           ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss, {Timestamp(2, 1)});
    notifications.notifyChange(kNss, {Timestamp(3, 1)});

    {
        auto opCtx = getClient()->makeOperationContext();
        ASSERT_EQUALS(notifications.get(opCtx.get(), scopedNotif), Timestamp(3, 1));
    }
}

TEST_F(NamespaceMetadataChangeNotificationsTest, NotifyAndThenWaitAgain) {
    NamespaceMetadataChangeNotifications notifications;

    auto scopedNotif = notifications.createNotification(kNss);

    {
        auto opCtx = getClient()->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(notifications.get(opCtx.get(), scopedNotif),
                           AssertionException,
                           ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss, {Timestamp(2, 1)});

    {
        auto opCtx = getClient()->makeOperationContext();
        ASSERT_EQUALS(notifications.get(opCtx.get(), scopedNotif), Timestamp(2, 1));
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(notifications.get(opCtx.get(), scopedNotif),
                           AssertionException,
                           ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss, {Timestamp(3, 1)});

    {
        auto opCtx = getClient()->makeOperationContext();
        ASSERT_EQUALS(notifications.get(opCtx.get(), scopedNotif), Timestamp(3, 1));
    }
}

TEST_F(NamespaceMetadataChangeNotificationsTest, TwoWaiters) {
    NamespaceMetadataChangeNotifications notifications;

    auto scopedNotif1 = notifications.createNotification(kNss);

    {
        auto opCtx = getClient()->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(
            scopedNotif1.get(opCtx.get()), AssertionException, ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss, {Timestamp(2, 1)});
    auto scopedNotif2 = notifications.createNotification(kNss);
    notifications.notifyChange(kNss, {Timestamp(3, 1)});

    {
        auto opCtx = getClient()->makeOperationContext();
        ASSERT_EQUALS(notifications.get(opCtx.get(), scopedNotif1), Timestamp(3, 1));
    }

    {
        auto opCtx = getClient()->makeOperationContext();
        scopedNotif2.get(opCtx.get());
        ASSERT_EQUALS(notifications.get(opCtx.get(), scopedNotif2), Timestamp(3, 1));
    }
}

}  // namespace
}  // namespace mongo
