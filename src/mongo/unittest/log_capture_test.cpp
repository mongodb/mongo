/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
