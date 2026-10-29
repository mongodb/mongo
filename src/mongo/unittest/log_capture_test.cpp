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

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::IsEmpty;

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
    static constexpr int logId = 10903001;
    LogCaptureGuard logs;
    LOGV2(logId, "Test", "a"_attr = 1);
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << logId)), 1);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("msg" << "Test")), 1);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << logId << "msg" << "Test")), 1);

    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << logId + 1)), 0);
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

TEST(LogCaptureTest, IndicesOfBSONContainingSubset1Element) {
    for (size_t multiplicity = 0; multiplicity < 4; ++multiplicity) {
        SCOPED_TRACE(fmt::format("multiplicity={}", multiplicity));

        LogCaptureGuard logs;
        std::vector<size_t> expected(multiplicity);
        for (size_t i = 0; i < multiplicity; ++i) {
            LOGV2(13370711, "Xyzzy");
            expected[i] = i;
        }
        logs.stop();

        ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("msg" << "Xyzzy")),
                    ElementsAreArray(expected));
    }
}

TEST(LogCaptureTest, IndicesOfBSONContainingSubsetFindCommonFields) {
    static constexpr int logId = 13370712;
    LogCaptureGuard logs;
    LOGV2(logId, "Test", "a"_attr = 1);
    logs.stop();
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("id" << logId)), ElementsAre(0));
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("msg" << "Test")), ElementsAre(0));
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("id" << logId << "msg" << "Test")),
                ElementsAre(0));
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("id" << logId + 1)), IsEmpty());
}

TEST(LogCaptureTest, IndicesOfBSONContainingSubsetFindAttrInt) {
    LogCaptureGuard logs;
    LOGV2(13370713, "Test", "a"_attr = 1);
    logs.stop();
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("attr" << BSON("a" << 1))), ElementsAre(0));
}

TEST(LogCaptureTest, IndicesOfBSONContainingSubsetFindAttrObj) {
    LogCaptureGuard logs;
    LOGV2(13370714, "Test", "obj"_attr = BSON("f1" << 1 << "f2" << "hi"));
    logs.stop();
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(
                    BSON("attr" << BSON("obj" << BSON("f1" << 1 << "f2" << "hi")))),
                ElementsAre(0));
}

TEST(LogCaptureTest, IndicesOfBSONContainingSubsetIgnoresExtraneousFields) {
    LogCaptureGuard logs;
    LOGV2(13370715, "Test", "a"_attr = 1, "b"_attr = 2);
    logs.stop();
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("attr" << BSON("a" << 1))), ElementsAre(0));
}

TEST(LogCaptureTest, IndicesOfBSONContainingSubsetAcceptsSubsets) {
    LogCaptureGuard logs;
    LOGV2(13370716, "Test", "obj"_attr = BSON("f" << 1 << "g" << 1));
    logs.stop();
    auto hasAttrObj = [&](BSONObj sub) {
        return logs.indicesOfBSONContainingSubset(BSON("attr" << BSON("obj" << sub)));
    };
    ASSERT_THAT(hasAttrObj(BSONObj{}), ElementsAre(0));
    ASSERT_THAT(hasAttrObj(BSON("f" << 1)), ElementsAre(0));
    ASSERT_THAT(hasAttrObj(BSON("f" << 1 << "g" << 1)), ElementsAre(0));
    ASSERT_THAT(hasAttrObj(BSON("f" << 1 << "g" << 1 << "h" << 1)), IsEmpty());
}

TEST(LogCaptureTest, IndicesOfBSONContainingSubsetNotRecursive) {
    LogCaptureGuard logs;
    LOGV2(13370717, "Test", "a"_attr = 1);
    logs.stop();
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("attr" << BSON("a" << 1))), ElementsAre(0));
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("a" << 1)), IsEmpty())
        << "Do not match a deep node";
}

TEST(LogCaptureTest, IndicesOfBSONContainingSubsetUndefinedActsAsWildcard) {
    LogCaptureGuard logs;
    LOGV2(13370718, "Test", "a"_attr = 1);
    logs.stop();
    ASSERT_EQ(logs.indicesOfBSONContainingSubset(BSON("id" << BSONUndefined)).size(), 1u);
    ASSERT_EQ(logs.indicesOfBSONContainingSubset(BSON("msg" << BSONUndefined)).size(), 1u);
    ASSERT_EQ(logs.indicesOfBSONContainingSubset(BSON("attr" << BSONUndefined)).size(), 1u);
    ASSERT_EQ(logs.indicesOfBSONContainingSubset(BSON("attr" << BSON("a" << BSONUndefined))).size(),
              1);
}

TEST(LogCaptureTest, IndicesOfBSONContainingSubsetReturnsCorrectIndices) {
    LogCaptureGuard logs;
    LOGV2(13370719, "First");
    LOGV2(13370720, "Second", "a"_attr = 1);
    LOGV2(13370721, "Third");
    LOGV2(13370722, "Fourth", "a"_attr = 1);
    logs.stop();

    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("attr" << BSON("a" << 1))),
                ElementsAre(1, 3));
    ASSERT_THAT(logs.indicesOfBSONContainingSubset(BSON("msg" << "First")), ElementsAre(0));
}

TEST(LogCaptureTest, IndicesOfTextContaining1Element) {
    for (size_t multiplicity = 0; multiplicity < 4; ++multiplicity) {
        SCOPED_TRACE(fmt::format("multiplicity={}", multiplicity));

        LogCaptureGuard logs;
        std::vector<size_t> expected(multiplicity);
        for (size_t i = 0; i < multiplicity; ++i) {
            LOGV2(13370723, "Xyzzy");
            expected[i] = i;
        }
        logs.stop();

        ASSERT_THAT(logs.indicesOfTextContaining("Xyzzy"), ElementsAreArray(expected));
    }
}

TEST(LogCaptureTest, IndicesOfTextContainingNoMatch) {
    LogCaptureGuard logs;
    LOGV2(13370724, "Test");
    logs.stop();
    ASSERT_THAT(logs.indicesOfTextContaining("NotFound"), IsEmpty());
}

TEST(LogCaptureTest, IndicesOfTextContainingReturnsCorrectIndices) {
    LogCaptureGuard logs;
    LOGV2(13370725, "First");
    LOGV2(13370726, "Second match");
    LOGV2(13370727, "Third");
    LOGV2(13370728, "Fourth match");
    logs.stop();

    ASSERT_THAT(logs.indicesOfTextContaining("match"), ElementsAre(1, 3));
}

}  // namespace
}  // namespace mongo::unittest
