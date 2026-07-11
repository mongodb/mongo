// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/log_capture.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/logv2/bson_formatter.h"
#include "mongo/logv2/domain_filter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/synchronized_value.h"

#include <algorithm>

#include <boost/log/core/core.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::unittest {
namespace {

TEST(LogCaptureTest, CountBSONContainingSubset1Element) {
    for (int multiplicity = 0; multiplicity < 4; ++multiplicity) {
        LogCaptureGuard logs;
        for (int i = 0; i < multiplicity; ++i)
            LOGV2(10903000, "Xyzzy");
        logs.stop();
        ASSERT_EQ(logs.countBSONContainingSubset(BSON("msg" << "Xyzzy")), multiplicity);
    }
}

TEST(LogCaptureTest, CountBSONContainingSubsetFindCommonFields) {
    LogCaptureGuard logs;
    LOGV2(10903001, "Test", "a"_attr = 1);
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 10903001)), 1);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("msg" << "Test")), 1);

    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 10903099)), 0);
}


TEST(LogCaptureTest, CountBSONContainingSubsetFindAttrInt) {
    LogCaptureGuard logs;
    LOGV2(10903002, "Test", "a"_attr = 1);
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("attr" << BSON("a" << 1))), 1);
}

TEST(LogCaptureTest, CountBSONContainingSubsetFindAttrObj) {
    LogCaptureGuard logs;
    LOGV2(10903003, "Test", "obj"_attr = BSON("f1" << 1 << "f2" << "hi"));
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(
                  BSON("attr" << BSON("obj" << BSON("f1" << 1 << "f2" << "hi")))),
              1);
}

TEST(LogCaptureTest, CountBSONContainingSubsetIgnoresExtraneousFields) {
    LogCaptureGuard logs;
    LOGV2(10903004, "Test", "a"_attr = 1, "b"_attr = 2);
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("attr" << BSON("a" << 1))), 1);
}

TEST(LogCaptureTest, CountBSONContainingSubsetAcceptsSubsets) {
    LogCaptureGuard logs;
    LOGV2(10903005, "Test", "obj"_attr = BSON("f" << 1 << "g" << 1));
    logs.stop();
    auto hasAttrObj = [&](BSONObj sub) {
        return logs.countBSONContainingSubset(BSON("attr" << BSON("obj" << sub)));
    };
    ASSERT_EQ(hasAttrObj(BSONObj{}), 1);
    ASSERT_EQ(hasAttrObj(BSON("f" << 1)), 1);
    ASSERT_EQ(hasAttrObj(BSON("f" << 1 << "g" << 1)), 1);
    ASSERT_EQ(hasAttrObj(BSON("f" << 1 << "g" << 1 << "h" << 1)), 0);
}

TEST(LogCaptureTest, CountBSONContainingSubsetNotRecursive) {
    LogCaptureGuard logs;
    LOGV2(10903006, "Test", "a"_attr = 1);
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("attr" << BSON("a" << 1))), 1);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("a" << 1)), 0) << "Do not match a deep node";
}

TEST(LogCaptureTest, CountBSONContainingSubsetUndefinedActsAsWildcard) {
    LogCaptureGuard logs;
    LOGV2(10903007, "Test", "a"_attr = 1);
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << BSONUndefined)), 1);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("msg" << BSONUndefined)), 1);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("attr" << BSONUndefined)), 1);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("attr" << BSON("a" << BSONUndefined))), 1);
}

}  // namespace
}  // namespace mongo::unittest
