/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/catalog_cache/namespace_metadata_change_notifications.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
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
