
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

namespace {

using mongo::fromjson;

class StitchSupportTest : public mongo::unittest::Test {
protected:
    void setUp() override {
        status = stitch_support_v1_status_create();
        ASSERT(status);

        lib = stitch_support_v1_init(status);
        ASSERT(lib);
    }

    void tearDown() override {
        int code = stitch_support_v1_fini(lib, status);
        ASSERT_EQ(code, STITCH_SUPPORT_V1_SUCCESS);
        lib = nullptr;

        stitch_support_v1_status_destroy(status);
        status = nullptr;
    }

    stitch_support_v1_status* status = nullptr;
    stitch_support_v1_lib* lib = nullptr;

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
