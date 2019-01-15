
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

#include <string>
#include <utility>

#include "stitch_support/stitch_support.h"

#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"

namespace {

using mongo::fromjson;
using mongo::makeGuard;
using mongo::ScopeGuard;

class StitchSupportTest : public mongo::unittest::Test {
protected:
    void setUp() override {
        status = stitch_support_v1_status_create();
        ASSERT(status);

        lib = stitch_support_v1_init(status);
        ASSERT(lib);

        updateDetails = stitch_support_v1_update_details_create();
        ASSERT(updateDetails);
    }

    void tearDown() override {
        int code = stitch_support_v1_fini(lib, status);
        ASSERT_EQ(code, STITCH_SUPPORT_V1_SUCCESS);
        lib = nullptr;

        stitch_support_v1_status_destroy(status);
        status = nullptr;

        stitch_support_v1_update_details_destroy(updateDetails);
        updateDetails = nullptr;
    }

    auto checkMatch(const char* filterJSON,
                    std::vector<const char*> documentsJSON,
                    stitch_support_v1_collator* collator = nullptr) {
        auto matcher = stitch_support_v1_matcher_create(
            lib, fromjson(filterJSON).objdata(), collator, nullptr);
        ASSERT(matcher);
        bool isMatch = true;
        for (const auto documentJSON : documentsJSON) {
            stitch_support_v1_check_match(
                matcher, fromjson(documentJSON).objdata(), &isMatch, nullptr);
        }
        stitch_support_v1_matcher_destroy(matcher);
        return isMatch;
    }

    auto checkMatchStatus(const char* filterJSON,
                          const char* documentJSON,
                          stitch_support_v1_collator* collator = nullptr) {
        auto match_status = stitch_support_v1_status_create();
        auto matcher = stitch_support_v1_matcher_create(
            lib, fromjson(filterJSON).objdata(), collator, match_status);
        ASSERT(!matcher);

        ASSERT_EQ(STITCH_SUPPORT_V1_ERROR_EXCEPTION,
                  stitch_support_v1_status_get_error(match_status));
        // Make sure that we get a proper code back but don't worry about its exact value.
        ASSERT_NE(0, stitch_support_v1_status_get_code(match_status));
        std::string explanation(stitch_support_v1_status_get_explanation(match_status));
        stitch_support_v1_status_destroy(match_status);

        return explanation;
    }

    void checkUpdate(const char* expr,
                     const char* document,
                     mongo::BSONObj expectedResult,
                     const char* match = nullptr,
                     const char* arrayFilters = nullptr,
                     const char* collatorObj = nullptr) {
        stitch_support_v1_collator* collator = nullptr;
        if (collatorObj) {
            collator =
                stitch_support_v1_collator_create(lib, fromjson(collatorObj).objdata(), nullptr);
        }
        ON_BLOCK_EXIT([collator] { stitch_support_v1_collator_destroy(collator); });

        stitch_support_v1_matcher* matcher = nullptr;
        if (match) {
            matcher =
                stitch_support_v1_matcher_create(lib, fromjson(match).objdata(), collator, nullptr);
            ASSERT(matcher);
        }
        ON_BLOCK_EXIT([matcher] { stitch_support_v1_matcher_destroy(matcher); });

        stitch_support_v1_update* update = stitch_support_v1_update_create(
            lib,
            fromjson(expr).objdata(),
            arrayFilters ? fromjson(arrayFilters).objdata() : nullptr,
            matcher,
            collator,
            status);
        ASSERT(update);
        ON_BLOCK_EXIT([update] { stitch_support_v1_update_destroy(update); });

        char* updateResult = stitch_support_v1_update_apply(
            update, fromjson(document).objdata(), updateDetails, status);
        ASSERT_EQ(0, stitch_support_v1_status_get_code(status))
            << stitch_support_v1_status_get_error(status) << ":"
            << stitch_support_v1_status_get_explanation(status);
        ASSERT(updateResult);
        ON_BLOCK_EXIT([updateResult] { free(updateResult); });

        ASSERT_BSONOBJ_EQ(mongo::BSONObj(updateResult), expectedResult);
    }

    const std::string getModifiedPaths() {
        ASSERT(updateDetails);

        std::stringstream ss;
        ss << "[";
        size_t nPaths = stitch_support_v1_update_details_num_modified_paths(updateDetails);
        for (size_t pathIdx = 0; pathIdx < nPaths; ++pathIdx) {
            auto path = stitch_support_v1_update_details_path(updateDetails, pathIdx);
            ss << path;
            if (pathIdx != (nPaths - 1))
                ss << ", ";
        }
        ss << "]";
        return ss.str();
    }

    stitch_support_v1_status* status = nullptr;
    stitch_support_v1_lib* lib = nullptr;
    stitch_support_v1_update_details* updateDetails = nullptr;
};

TEST_F(StitchSupportTest, InitializationIsSuccessful) {
    ASSERT_EQ(STITCH_SUPPORT_V1_SUCCESS, stitch_support_v1_status_get_error(status));
    ASSERT(lib);
}

TEST_F(StitchSupportTest, DoubleInitializationFails) {
    auto lib2 = stitch_support_v1_init(status);

    ASSERT(!lib2);
    ASSERT_EQ(STITCH_SUPPORT_V1_ERROR_LIBRARY_ALREADY_INITIALIZED,
              stitch_support_v1_status_get_error(status));
}

TEST_F(StitchSupportTest, CheckMatchWorksWithDefaults) {
    ASSERT_TRUE(checkMatch("{a: 1}", {"{a: 1, b: 1}", "{a: [0, 1]}"}));
    ASSERT_TRUE(checkMatch(
        "{'a.b': 1}", {"{a: {b: 1}}", "{a: [{b: 1}]}", "{a: {b: [0, 1]}}", "{a: [{b: [0, 1]}]}"}));
    ASSERT_TRUE(checkMatch("{'a.0.b': 1}", {"{a: [{b: 1}]}", "{a: [{b: [0, 1]}]}"}));
    ASSERT_TRUE(checkMatch("{'a.1.b': 1}", {"{a: [{b: [0, 1]}, {b: [0, 1]}]}"}));
    ASSERT_TRUE(checkMatch("{a: {$size: 1}}", {"{a: [100]}"}));
    ASSERT_FALSE(checkMatch("{a: {$size: 1}}", {"{a: [[100], [101]]}"}));
    ASSERT_TRUE(checkMatch("{'a.b': {$size: 1}}", {"{a: [0, {b: [100]}]}"}));
    ASSERT_TRUE(checkMatch("{'a.1.0.b': 1}", {"{a: [123, [{b: [1]}, 456]]}"}));
    ASSERT_TRUE(checkMatch("{'a.1.b': 1}", {"{a: [123, [{b: [1]}, 456]]}"}));
    ASSERT_TRUE(checkMatch("{$expr: {$gt: ['$b', '$a']}}", {"{a: 123, b: 456}"}));
    ASSERT_TRUE(checkMatch("{a: {$regex: 'lib$'}}", {"{a: 'stitchlib'}"}));
}

TEST_F(StitchSupportTest, CheckMatchWorksWithStatus) {
    ASSERT_EQ("bad query: BadValue: unknown operator: $bogus",
              checkMatchStatus("{a: {$bogus: 1}}", "{a: 1}"));
    ASSERT_EQ("bad query: BadValue: $where is not allowed in this context",
              checkMatchStatus("{$where: 'this.a == 1'}", "{a: 1}"));
    ASSERT_EQ("bad query: BadValue: $text is not allowed in this context",
              checkMatchStatus("{$text: {$search: 'stitch'}}", "{a: 'stitch lib'}"));
    ASSERT_EQ(
        "bad query: BadValue: $geoNear, $near, and $nearSphere are not allowed in this context",
        checkMatchStatus(
            "{location: {$near: {$geometry: {type: 'Point', "
            "coordinates: [ -73.9667, 40.78 ] }, $minDistance: 10, $maxDistance: 500}}}",
            "{type: 'Point', 'coordinates': [100.0, 0.0]}"));

    // 'check_match' cannot actually fail so we do not test it with a status.
}

TEST_F(StitchSupportTest, CheckMatchWorksWithCollation) {
    auto collator = stitch_support_v1_collator_create(
        lib, fromjson("{locale: 'en', strength: 2}").objdata(), nullptr);
    ASSERT_TRUE(checkMatch("{a: 'word'}", {"{a: 'WORD', b: 'other'}"}, collator));
    stitch_support_v1_collator_destroy(collator);
}

TEST_F(StitchSupportTest, TestUpdateSingleElement) {
    checkUpdate("{$set: {a: 2}}", "{a: 1}", fromjson("{a: 2}"));
    ASSERT_EQ(getModifiedPaths(), "[a]");
}

TEST_F(StitchSupportTest, TestReplacementStyleUpdateReportsNoModifiedPaths) {
    // Replacement-style updates report no modified paths because this functionality is not
    // currently needed by Stitch.
    checkUpdate("{a: 2}", "{a: 1}", fromjson("{a: 2}"));
    ASSERT_EQ(getModifiedPaths(), "[]");
}

TEST_F(StitchSupportTest, TestUpdateArrayElement) {
    checkUpdate("{$set: {'a.0': 2}}", "{a: [1, 2]}", fromjson("{a: [2, 2]}"));
    ASSERT_EQ(getModifiedPaths(), "[a.0]");

    checkUpdate("{$set: {'a.0.b': 2}}", "{a: [{b: 1}]}", fromjson("{a: [{b: 2}]}"));
    ASSERT_EQ(getModifiedPaths(), "[a.0.b]");
}

TEST_F(StitchSupportTest, TestUpdateAddToArray) {
    checkUpdate("{$set: {'a.1.b': 2}}", "{a: [{b: 1}]}", fromjson("{a: [{b: 1}, {b: 2}]}"));
    ASSERT_EQ(getModifiedPaths(), "[a]");

    checkUpdate(
        "{$set: {'a.1.b': 2, c: 3}}", "{a: [{b: 1}]}", fromjson("{a: [{b: 1}, {b: 2}], c: 3}"));
    ASSERT_EQ(getModifiedPaths(), "[a, c]");
}

TEST_F(StitchSupportTest, TestUpdatePullFromArray) {
    checkUpdate("{$pull: {'a': 2}}", "{a: [3, 2, 1]}", fromjson("{a: [3, 1]}"));
    ASSERT_EQ(getModifiedPaths(), "[a]");
}

TEST_F(StitchSupportTest, TestPositionalUpdates) {
    checkUpdate("{$set: {'a.$': 3}}", "{a: [1, 2]}", fromjson("{a: [1, 3]}"), "{a: 2}");
    ASSERT_EQ(getModifiedPaths(), "[a.1]");

    checkUpdate("{$set: {'a.$.b': 3}}",
                "{a: [{b: 1}, {b: 2}]}",
                fromjson("{a: [{b: 1}, {b: 3}]}"),
                "{'a.b': 2}");
    ASSERT_EQ(getModifiedPaths(), "[a.1.b]");
}

TEST_F(StitchSupportTest, TestUpdatesWithArrayFilters) {
    checkUpdate(
        "{$set: {'a.$[i]': 3}}", "{a: [1, 2]}", fromjson("{a: [1, 3]}"), nullptr, "[{'i': 2}]");
    ASSERT_EQ(getModifiedPaths(), "[a.1]");

    checkUpdate("{$set: {'a.$[i].b': 3}}",
                "{a: [{b: 1}, {b: 2}]}",
                fromjson("{a: [{b: 1}, {b: 3}]}"),
                nullptr,
                "[{'i.b': 2}]");
    ASSERT_EQ(getModifiedPaths(), "[a.1.b]");
}

TEST_F(StitchSupportTest, TestUpdateRespectsTheCollation) {
    auto caseInsensitive = "{locale: 'en', strength: 2}";
    checkUpdate("{$addToSet: {a: 'santa'}}",
                "{a: ['Santa', 'Elf']}",
                fromjson("{a: ['Santa', 'Elf']}"),
                nullptr,
                nullptr,
                caseInsensitive);
    // $addToSet with existing element is considered a no-op, but the array is marked as modified.
    ASSERT_EQ(getModifiedPaths(), "[a]");

    checkUpdate("{$pull: {a: 'santa'}}",
                "{a: ['Santa', 'Elf']}",
                fromjson("{a: ['Elf']}"),
                nullptr,
                nullptr,
                caseInsensitive);
    ASSERT_EQ(getModifiedPaths(), "[a]");
}

}  // namespace

// Define main function as an entry to these tests.
//
// Note that we don't use the main() defined for most other unit tests so that we can avoid double
// calling runGlobalInitializers(), which is called both from the regular unit test main() and from
// the Stitch Support Library intializer function that gets tested here.
int main(const int argc, const char* const* const argv) {
    const auto result = ::mongo::unittest::Suite::run(std::vector<std::string>(), "", 1);

    // This is the standard exit path for Mongo processes. See the mongo::quickExit() declaration
    // for more information.
    mongo::quickExit(result);
}
