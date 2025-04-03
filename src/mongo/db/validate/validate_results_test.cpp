/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/validate/validate_results.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

// Helper to convert a BSON array into a vector.
std::vector<BSONElement> getElements(const BSONObj& obj) {
    std::vector<BSONElement> ret;
    obj.elems(ret);
    return ret;
}

// Helper to ensure that there is an element present in a container. In gmock this would be
// something like: EXPECT_THAT(container, Contains(BSONString(Eq(element))))
bool contains(const auto& container, const auto& element) {
    return std::any_of(container.begin(), container.end(), [&element](const BSONElement& elem) {
        return elem.str() == element;
    });
};

}  // namespace

TEST(ValidateResultsTest, ErrorWarningFieldsPropagatedCorrectly) {
    ValidateResults vr;
    vr.addError("top-level error");
    vr.addWarning("top-level warning");
    vr.getIndexValidateResult("index").addError("index-specific error");
    vr.getIndexValidateResult("index").addWarning("index-specific warning");

    BSONObjBuilder bob;
    vr.appendToResultObj(&bob, /*debugging=*/false);
    auto obj = bob.done();

    std::vector<BSONElement> errs = getElements(obj.getObjectField("errors"));
    std::vector<BSONElement> warns = getElements(obj.getObjectField("warnings"));
    std::vector<BSONElement> index_errs = getElements(
        obj.getObjectField("indexDetails").getObjectField("index").getObjectField("errors"));
    std::vector<BSONElement> index_warns = getElements(
        obj.getObjectField("indexDetails").getObjectField("index").getObjectField("warnings"));
    ASSERT_TRUE(contains(errs, "top-level error"));
    ASSERT_TRUE(contains(warns, "top-level warning"));
    ASSERT_TRUE(contains(index_errs, "index-specific error"));
    ASSERT_TRUE(contains(index_warns, "index-specific warning"));
}

// See SERVER-89857, issues found for a specific index should not be duplicated en-masse into the
// top-level fields, but we do need to put *something* in the top-level field to inform the user to
// check indexDetails.
TEST(ValidateResultsTest, IndexIssuesGenerateSyntheticCollectionIssue) {
    ValidateResults vr;
    vr.getIndexValidateResult("foo_idx").addError("foo error");
    vr.getIndexValidateResult("foo_idx").addWarning("foo warning");
    vr.getIndexValidateResult("bar_idx").addError("bar error 1");
    vr.getIndexValidateResult("bar_idx").addError("bar error 2");
    ASSERT_TRUE(vr.getErrors().empty());
    ASSERT_TRUE(vr.getWarnings().empty());

    BSONObjBuilder bob;
    vr.appendToResultObj(&bob, /*debugging=*/false);
    auto obj = bob.done();

    std::vector<BSONElement> errs = getElements(obj.getObjectField("errors"));
    std::vector<BSONElement> warns = getElements(obj.getObjectField("warnings"));
    ASSERT_EQ(errs.size(), 2);   // One foo and *one* for bar.
    ASSERT_EQ(warns.size(), 1);  // One for foo only.

    // Don't double-report index issues in the top-level fields.
    ASSERT_FALSE(contains(errs, "foo error"));
    ASSERT_FALSE(contains(errs, "bar error 1"));
    ASSERT_FALSE(contains(warns, "foo warning"));
}

TEST(ValidateResultsTest, ErrorsAndWarningsAutomaticallyDeduplicated) {
    ValidateResults vr;

    ASSERT_TRUE(vr.addError("e1"));
    ASSERT_TRUE(vr.addError("e2"));
    ASSERT_FALSE(vr.addError("e1"));
    ASSERT_EQ(vr.getErrors().size(), 2);

    ASSERT_TRUE(vr.addWarning("w1"));
    ASSERT_TRUE(vr.addWarning("w2"));
    ASSERT_FALSE(vr.addWarning("w2"));
    ASSERT_EQ(vr.getWarnings().size(), 2);
}

TEST(ValidateResultsTest, MissingAndExtraEntriesSizeLimited) {
    ValidateResults vr;

    for (int i = 0; i < 10; i++) {
        // Each string is (200 - i)KB in size, so the *larger* i will be kept.
        auto obj = BSON("i" << i << "s" << std::string((200 - i) * 1024, 'a'));
        vr.addMissingIndexEntry(obj);
        vr.addExtraIndexEntry(obj);
    }

    // We can't fit all 10 elements in.
    ASSERT_NE(10, vr.getMissingIndexEntries().size());
    ASSERT_NE(10, vr.getExtraIndexEntries().size());

    // Since "i==0" is the largest, they will definitely have been evicted.
    auto isNotZero = [](const BSONObj& obj) {
        const auto& i = obj.getField("i");
        return i.isNumber() && i.numberInt() != 0;
    };
    ASSERT_TRUE(std::all_of(
        vr.getMissingIndexEntries().begin(), vr.getMissingIndexEntries().end(), isNotZero));
    ASSERT_TRUE(
        std::all_of(vr.getExtraIndexEntries().begin(), vr.getExtraIndexEntries().end(), isNotZero));
}

TEST(ValidateResultsTest, MissingAndExtraEntriesKeepsAtLeastOne) {
    ValidateResults vr;

    // Definitely larger than the limit.
    auto obj = BSON("x" << std::string(2 * 1024 * 1024, 'a'));
    vr.addMissingIndexEntry(obj);
    vr.addExtraIndexEntry(obj);

    ASSERT_EQ(1, vr.getMissingIndexEntries().size());
    ASSERT_EQ(1, vr.getExtraIndexEntries().size());

    // Much smaller.
    auto obj2 = BSON("x" << "a");
    vr.addMissingIndexEntry(obj2);
    vr.addExtraIndexEntry(obj2);

    ASSERT_EQ(1, vr.getMissingIndexEntries().size());
    ASSERT_EQ(1, vr.getExtraIndexEntries().size());

    // Prefers to keep the smaller entries.
    ASSERT_BSONOBJ_EQ(obj2, vr.getMissingIndexEntries().front());
    ASSERT_BSONOBJ_EQ(obj2, vr.getExtraIndexEntries().front());
}

TEST(ValidateResultsTest, MissingAndExtraEntriesCreateErrorsWhenSizeExceeded) {
    ValidateResults vr;
    auto obj = BSON("x" << std::string(2 * 1024 * 1024, 'a'));

    // First addition, no evictions.
    vr.addMissingIndexEntry(obj);
    vr.addExtraIndexEntry(obj);
    ASSERT_TRUE(vr.getErrors().empty());

    // Now we evict something.
    vr.addMissingIndexEntry(obj);
    ASSERT_EQ(1, vr.getErrors().size());
    ASSERT_TRUE(vr.getErrors().contains(
        "Not all missing index entry inconsistencies are listed due to size limitations."));

    // Multiple evictions -> still 1 error.
    vr.addMissingIndexEntry(obj);
    ASSERT_EQ(1, vr.getErrors().size());

    // But 1 for each missing/extra
    vr.addExtraIndexEntry(obj);
    ASSERT_EQ(2, vr.getErrors().size());
    ASSERT_TRUE(vr.getErrors().contains(
        "Not all missing index entry inconsistencies are listed due to size limitations."));
    ASSERT_TRUE(vr.getErrors().contains(
        "Not all extra index entry inconsistencies are listed due to size limitations."));
}

TEST(ValidateResultsTest, TestingSizeOfIndexDetailsEntries) {
    ValidateResults vr;

    IndexValidateResults& ivr = vr.getIndexValidateResult("index1");
    std::string largeString(200 * 1024, 'a');

    // We allocated 2MB for all warnings, and 2MB for all errors - these both surpass that amount.
    for (int i = 0; i < 25; i++) {
        ivr.addWarning(largeString);
        ivr.addError(largeString);
    }

    BSONObjBuilder bob;
    vr.appendToResultObj(&bob, /*debugging=*/false);
    auto obj = bob.done();

    // Check that we have not surpasssed 2MB, should be within 11 iterations.
    std::vector<BSONElement> index_warns = getElements(
        obj.getObjectField("indexDetails").getObjectField("index1").getObjectField("warnings"));
    ASSERT_LESS_THAN(index_warns.size(), 11);
    std::vector<BSONElement> index_errors = getElements(
        obj.getObjectField("indexDetails").getObjectField("index1").getObjectField("errors"));
    ASSERT_LESS_THAN(index_warns.size(), 11);
}

TEST(ValidateResultsTest, SpecAppearsInOutput) {
    ValidateResults vr;

    IndexValidateResults& ivr = vr.getIndexValidateResult("index1");
    ivr.setSpec(BSON("k" << "v"));

    BSONObjBuilder bob;
    vr.appendToResultObj(&bob, /*debugging=*/false);
    auto obj = bob.done();

    ASSERT_TRUE(obj.getObjectField("indexDetails").getObjectField("index1").hasField("spec"));
    ASSERT_TRUE(obj.getObjectField("indexDetails")
                    .getObjectField("index1")
                    .getObjectField("spec")
                    .hasField("k"));
    ASSERT_EQ(obj.getObjectField("indexDetails")
                  .getObjectField("index1")
                  .getObjectField("spec")
                  .getStringField("k"),
              "v");
}

}  // namespace mongo
