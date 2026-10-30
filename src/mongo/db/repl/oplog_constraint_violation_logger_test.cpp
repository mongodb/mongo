// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_constraint_violation_logger.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

constexpr int kViolationLogId = 7149000;
constexpr int kCappedViolationLogId = 10836100;

int countLogsWithId(const unittest::LogCaptureGuard& logs, int id) {
    return logs.countBSONContainingSubset(BSON("id" << id));
}

TEST(OplogConstraintViolationLoggerTest, NonCappedViolationUsesWarningLogId) {
    OplogConstraintViolationLogger logger;
    unittest::LogCaptureGuard logs;

    logger.logViolationIfReady(OplogConstraintViolationEnum::kDeleteWasEmpty,
                               BSON("op" << "d"),
                               /*status=*/boost::none,
                               /*isCappedCollection=*/false);

    ASSERT_EQ(1, countLogsWithId(logs, kViolationLogId));
    ASSERT_EQ(0, countLogsWithId(logs, kCappedViolationLogId));
    ASSERT_EQ(1,
              logs.countBSONContainingSubset(
                  BSON("id" << kViolationLogId << "attr" << BSON("isCappedCollection" << false))));
}

TEST(OplogConstraintViolationLoggerTest, CappedViolationUsesSeparateLogId) {
    OplogConstraintViolationLogger logger;
    unittest::LogCaptureGuard logs;

    logger.logViolationIfReady(OplogConstraintViolationEnum::kDeleteWasEmpty,
                               BSON("op" << "d"),
                               /*status=*/boost::none,
                               /*isCappedCollection=*/true,
                               /*cappedViolationIsTolerated=*/true);

    ASSERT_EQ(0, countLogsWithId(logs, kViolationLogId));
    ASSERT_EQ(1,
              logs.countBSONContainingSubset(BSON("id" << kCappedViolationLogId << "attr"
                                                       << BSON("isCappedCollection" << true))));
}

TEST(OplogConstraintViolationLoggerTest, NonToleratedCappedViolationUsesWarningLogId) {
    OplogConstraintViolationLogger logger;
    unittest::LogCaptureGuard logs;

    // A violation on a capped collection that will stop replication must not be reported as
    // harmless.
    logger.logViolationIfReady(OplogConstraintViolationEnum::kInsertOnExistingDoc,
                               BSON("op" << "i"),
                               /*status=*/boost::none,
                               /*isCappedCollection=*/true,
                               /*cappedViolationIsTolerated=*/false);

    ASSERT_EQ(0, countLogsWithId(logs, kCappedViolationLogId));
    ASSERT_EQ(1,
              logs.countBSONContainingSubset(
                  BSON("id" << kViolationLogId << "attr" << BSON("isCappedCollection" << true))));
}

TEST(OplogConstraintViolationLoggerTest, CappedViolationsDoNotSuppressNonCappedViolations) {
    OplogConstraintViolationLogger logger;
    unittest::LogCaptureGuard logs;

    logger.logViolationIfReady(OplogConstraintViolationEnum::kDeleteWasEmpty,
                               BSON("op" << "d"),
                               /*status=*/boost::none,
                               /*isCappedCollection=*/true,
                               /*cappedViolationIsTolerated=*/true);
    logger.logViolationIfReady(OplogConstraintViolationEnum::kDeleteWasEmpty,
                               BSON("op" << "d"),
                               /*status=*/boost::none,
                               /*isCappedCollection=*/false);

    ASSERT_EQ(1, countLogsWithId(logs, kCappedViolationLogId));
    ASSERT_EQ(1, countLogsWithId(logs, kViolationLogId));
}

TEST(OplogConstraintViolationLoggerTest, RepeatedViolationsAreRateLimitedPerCappedness) {
    OplogConstraintViolationLogger logger;
    unittest::LogCaptureGuard logs;

    for (int i = 0; i < 3; ++i) {
        for (bool isCapped : {true, false}) {
            logger.logViolationIfReady(OplogConstraintViolationEnum::kDeleteWasEmpty,
                                       BSON("op" << "d"),
                                       /*status=*/boost::none,
                                       isCapped,
                                       /*cappedViolationIsTolerated=*/isCapped);
        }
    }

    ASSERT_EQ(1, countLogsWithId(logs, kCappedViolationLogId));
    ASSERT_EQ(1, countLogsWithId(logs, kViolationLogId));
}

TEST(OplogConstraintViolationLoggerTest, CappedUpdateWasEmptyHasMessage) {
    ASSERT_EQ(kUpdateWasEmptyMsg, toString(OplogConstraintViolationEnum::kUpdateWasEmpty));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
