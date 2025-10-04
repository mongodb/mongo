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

#include "mongo/db/auth/user_acquisition_stats.h"

#include "mongo/db/auth/ldap_cumulative_operation_stats.h"
#include "mongo/db/auth/ldap_operation_stats.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

class UserAcquisitionStatsTest : public unittest::Test {
public:
    void assertUserCacheStats(const UserCacheAccessStats& stats,
                              std::uint64_t expectedStartedCacheAccessAttempts,
                              std::uint64_t expectedCompletedCacheAccessAttempts,
                              Microseconds expectedOngoingCacheAccessStartTime,
                              Microseconds expectedTotalCompletedCacheAccessTime) {
        ASSERT_EQ(stats._startedCacheAccessAttempts, expectedStartedCacheAccessAttempts);
        ASSERT_EQ(stats._completedCacheAccessAttempts, expectedCompletedCacheAccessAttempts);
        ASSERT_EQ(stats._ongoingCacheAccessStartTime, expectedOngoingCacheAccessStartTime);
        ASSERT_EQ(stats._totalCompletedCacheAccessTime, expectedTotalCompletedCacheAccessTime);
    }

    void assertLdapBindOrSearchStats(const LDAPOperationStats& stats,
                                     std::uint64_t expectedNumOps,
                                     Microseconds expectedStartTime,
                                     Microseconds expectedTotalTime,
                                     UserAcquisitionOpType type) {
        ASSERT(type == kBind || type == kSearch);
        if (type == kBind) {
            assertLdapOpStats(
                stats._bindStats, expectedNumOps, expectedStartTime, expectedTotalTime);
        } else {
            assertLdapOpStats(
                stats._searchStats, expectedNumOps, expectedStartTime, expectedTotalTime);
        }
    }

    void assertLdapOpStats(const LDAPOperationStats::Stats& stats,
                           std::uint64_t expectedNumOps,
                           Microseconds expectedStartTime,
                           Microseconds expectedTotalTime) {
        ASSERT_EQ(stats.numOps, expectedNumOps);
        ASSERT_EQ(stats.startTime, expectedStartTime);
        ASSERT_EQ(stats.totalCompletedOpTime, expectedTotalTime);
    }

    void assertLdapReferrals(const LDAPOperationStats& stats,
                             std::uint64_t expectedNumSuccessfulReferrals,
                             std::uint64_t expectedNumFailedReferrals) {
        ASSERT_EQ(stats._numSuccessfulReferrals, expectedNumSuccessfulReferrals);
        ASSERT_EQ(stats._numFailedReferrals, expectedNumFailedReferrals);
    }

protected:
    TickSourceMock<Seconds> mockTickSource;
};

TEST_F(UserAcquisitionStatsTest, userCacheAccessStats) {
    auto userAcquisitionStats = std::make_unique<UserAcquisitionStats>();
    mockTickSource.advance(Seconds(1));

    ASSERT_FALSE(userAcquisitionStats->shouldReportLDAPOperationStats());
    ASSERT_FALSE(userAcquisitionStats->shouldReportUserCacheAccessStats());

    UserAcquisitionStatsHandle handle(userAcquisitionStats.get(), &mockTickSource, kCache);
    auto ongoingUserCacheAccessStats = userAcquisitionStats->getUserCacheAccessStatsSnapshot();

    mockTickSource.advance(Seconds(1));
    handle.recordTimerEnd();

    auto completedUserCacheAccessStats = userAcquisitionStats->getUserCacheAccessStatsSnapshot();
    assertUserCacheStats(ongoingUserCacheAccessStats, 1, 0, Microseconds(1000000), Microseconds(0));
    assertUserCacheStats(
        completedUserCacheAccessStats, 1, 1, Microseconds(0), Microseconds(1000000));

    ASSERT_FALSE(userAcquisitionStats->shouldReportLDAPOperationStats());
    ASSERT_TRUE(userAcquisitionStats->shouldReportUserCacheAccessStats());

    StringBuilder sb;
    userAcquisitionStats->userCacheAcquisitionStatsToString(&sb, &mockTickSource);
    ASSERT_EQ(sb.str(),
              "{ startedUserCacheAcquisitionAttempts: 1, completedUserCacheAcquisitionAttempts: 1, "
              "userCacheWaitTimeMicros: 1000000 }");

    BSONObjBuilder bob;
    userAcquisitionStats->reportUserCacheAcquisitionStats(&bob, &mockTickSource);
    ASSERT_BSONOBJ_EQ(bob.obj(),
                      BSON("startedUserCacheAcquisitionAttempts"
                           << 1 << "completedUserCacheAcquisitionAttempts" << 1
                           << "userCacheWaitTimeMicros" << 1000000));
}

TEST_F(UserAcquisitionStatsTest, ldapOperationBindStats) {
    auto userAcquisitionStats = std::make_unique<UserAcquisitionStats>();
    mockTickSource.advance(Seconds(1));

    ASSERT_FALSE(userAcquisitionStats->shouldReportLDAPOperationStats());
    ASSERT_FALSE(userAcquisitionStats->shouldReportUserCacheAccessStats());

    UserAcquisitionStatsHandle firstHandle(userAcquisitionStats.get(), &mockTickSource, kBind);
    UserAcquisitionStatsHandle secondHandle(userAcquisitionStats.get(), &mockTickSource, kBind);
    auto ongoingLdapOperationStats = userAcquisitionStats->getLdapOperationStatsSnapshot();

    mockTickSource.advance(Seconds(1));
    firstHandle.recordTimerEnd();

    auto completedLdapOperationStats = userAcquisitionStats->getLdapOperationStatsSnapshot();
    assertLdapBindOrSearchStats(
        ongoingLdapOperationStats, 2, Microseconds(1000000), Microseconds(0), kBind);
    assertLdapBindOrSearchStats(
        ongoingLdapOperationStats, 0, Microseconds(0), Microseconds(0), kSearch);
    assertLdapBindOrSearchStats(
        completedLdapOperationStats, 2, Microseconds(0), Microseconds(1000000), kBind);
    assertLdapBindOrSearchStats(
        completedLdapOperationStats, 0, Microseconds(0), Microseconds(0), kSearch);

    mockTickSource.advance(Seconds(1));
    secondHandle.recordTimerEnd();

    // The second concurrent operation's time will be ignored since the first operation already
    // finished. This is a slight inaccuracy but is accepted due to the unlikelihood of these
    // types of scenarios and the complexity of properly accounting for it.
    completedLdapOperationStats = userAcquisitionStats->getLdapOperationStatsSnapshot();
    assertLdapBindOrSearchStats(
        completedLdapOperationStats, 2, Microseconds(0), Microseconds(1000000), kBind);

    ASSERT_TRUE(userAcquisitionStats->shouldReportLDAPOperationStats());
    ASSERT_FALSE(userAcquisitionStats->shouldReportUserCacheAccessStats());

    StringBuilder sb;
    userAcquisitionStats->ldapOperationStatsToString(&sb, &mockTickSource);
    ASSERT_EQ(sb.str(),
              "{ LDAPNumberOfSuccessfulReferrals: 0, LDAPNumberOfFailedReferrals: 0, "
              "LDAPNumberOfReferrals: 0, bindStats: { numOp: 2, opDurationMicros: 1000000 }, "
              "searchStats: { numOp: 0, opDurationMicros: 0 } }");

    BSONObjBuilder bob;
    userAcquisitionStats->reportLdapOperationStats(&bob, &mockTickSource);
    ASSERT_BSONOBJ_EQ(bob.obj(),
                      BSON("LDAPNumberOfSuccessfulReferrals"
                           << 0 << "LDAPNumberOfFailedReferrals" << 0 << "LDAPNumberOfReferrals"
                           << 0 << "bindStats"
                           << BSON("numOp" << 2 << "opDurationMicros" << 1000000) << "searchStats"
                           << BSON("numOp" << 0 << "opDurationMicros" << 0)));
}

TEST_F(UserAcquisitionStatsTest, ldapOperationSearchStats) {
    auto userAcquisitionStats = std::make_unique<UserAcquisitionStats>();
    mockTickSource.advance(Seconds(1));

    ASSERT_FALSE(userAcquisitionStats->shouldReportLDAPOperationStats());
    ASSERT_FALSE(userAcquisitionStats->shouldReportUserCacheAccessStats());

    UserAcquisitionStatsHandle firstHandle(userAcquisitionStats.get(), &mockTickSource, kSearch);
    UserAcquisitionStatsHandle secondHandle(userAcquisitionStats.get(), &mockTickSource, kSearch);
    auto ongoingLdapOperationStats = userAcquisitionStats->getLdapOperationStatsSnapshot();

    mockTickSource.advance(Seconds(1));
    firstHandle.recordTimerEnd();

    auto completedLdapOperationStats = userAcquisitionStats->getLdapOperationStatsSnapshot();
    assertLdapBindOrSearchStats(
        ongoingLdapOperationStats, 2, Microseconds(1000000), Microseconds(0), kSearch);
    assertLdapBindOrSearchStats(
        ongoingLdapOperationStats, 0, Microseconds(0), Microseconds(0), kBind);
    assertLdapBindOrSearchStats(
        completedLdapOperationStats, 2, Microseconds(0), Microseconds(1000000), kSearch);
    assertLdapBindOrSearchStats(
        completedLdapOperationStats, 0, Microseconds(0), Microseconds(0), kBind);

    mockTickSource.advance(Seconds(1));
    secondHandle.recordTimerEnd();

    // The second concurrent operation's time will be ignored since the first operation already
    // finished.
    completedLdapOperationStats = userAcquisitionStats->getLdapOperationStatsSnapshot();
    assertLdapBindOrSearchStats(
        completedLdapOperationStats, 2, Microseconds(0), Microseconds(1000000), kSearch);

    ASSERT_TRUE(userAcquisitionStats->shouldReportLDAPOperationStats());
    ASSERT_FALSE(userAcquisitionStats->shouldReportUserCacheAccessStats());

    StringBuilder sb;
    userAcquisitionStats->ldapOperationStatsToString(&sb, &mockTickSource);
    ASSERT_EQ(sb.str(),
              "{ LDAPNumberOfSuccessfulReferrals: 0, LDAPNumberOfFailedReferrals: 0, "
              "LDAPNumberOfReferrals: 0, bindStats: { numOp: 0, opDurationMicros: 0 }, "
              "searchStats: { numOp: 2, opDurationMicros: 1000000 } }");

    BSONObjBuilder bob;
    userAcquisitionStats->reportLdapOperationStats(&bob, &mockTickSource);
    ASSERT_BSONOBJ_EQ(bob.obj(),
                      BSON("LDAPNumberOfSuccessfulReferrals"
                           << 0 << "LDAPNumberOfFailedReferrals" << 0 << "LDAPNumberOfReferrals"
                           << 0 << "bindStats" << BSON("numOp" << 0 << "opDurationMicros" << 0)
                           << "searchStats"
                           << BSON("numOp" << 2 << "opDurationMicros" << 1000000)));
}

TEST_F(UserAcquisitionStatsTest, ldapOperationReferralStats) {
    auto userAcquisitionStats = std::make_unique<UserAcquisitionStats>();

    ASSERT_FALSE(userAcquisitionStats->shouldReportLDAPOperationStats());
    ASSERT_FALSE(userAcquisitionStats->shouldReportUserCacheAccessStats());

    {
        UserAcquisitionStatsHandle successHandle(
            userAcquisitionStats.get(), &mockTickSource, kSuccessfulReferral);
        UserAcquisitionStatsHandle failHandle(
            userAcquisitionStats.get(), &mockTickSource, kFailedReferral);
    }

    auto ldapOperationStats = userAcquisitionStats->getLdapOperationStatsSnapshot();
    assertLdapReferrals(ldapOperationStats, 1, 1);

    ASSERT_TRUE(userAcquisitionStats->shouldReportLDAPOperationStats());
    ASSERT_FALSE(userAcquisitionStats->shouldReportUserCacheAccessStats());

    StringBuilder sb;
    userAcquisitionStats->ldapOperationStatsToString(&sb, &mockTickSource);
    ASSERT_EQ(sb.str(),
              "{ LDAPNumberOfSuccessfulReferrals: 1, LDAPNumberOfFailedReferrals: 1, "
              "LDAPNumberOfReferrals: 2, bindStats: { numOp: 0, opDurationMicros: 0 }, "
              "searchStats: { numOp: 0, opDurationMicros: 0 } }");

    BSONObjBuilder bob;
    userAcquisitionStats->reportLdapOperationStats(&bob, &mockTickSource);
    ASSERT_BSONOBJ_EQ(bob.obj(),
                      BSON("LDAPNumberOfSuccessfulReferrals"
                           << 1 << "LDAPNumberOfFailedReferrals" << 1 << "LDAPNumberOfReferrals"
                           << 2 << "bindStats" << BSON("numOp" << 0 << "opDurationMicros" << 0)
                           << "searchStats" << BSON("numOp" << 0 << "opDurationMicros" << 0)));
}

}  // namespace mongo
