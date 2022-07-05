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

#include <algorithm>
#include <string>
#include <utility>

#include "stitch_support/stitch_support.h"

#include "mongo/base/initializer.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"

namespace {

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

    /**
     * toBSONForAPI is a custom converter from json which satisfies the unusual uint8_t type that
     * stitch_support_v1 uses for BSON. The intermediate BSONObj is also returned since its lifetime
     * governs the lifetime of the uint8_t*.
     */
    auto toBSONForAPI(const char* json) {
        auto bson = mongo::fromjson(json);
        return std::make_pair(static_cast<const uint8_t*>(static_cast<const void*>(bson.objdata())),
                              bson);
    }

    /**
     * fromBSONForAPI is a custom converter to json which satisfies the unusual uint8_t type that
     * stitch_support_v1 uses for BSON.
     */
    auto fromBSONForAPI(const uint8_t* bson) {
        return mongo::tojson(
            mongo::BSONObj(static_cast<const char*>(static_cast<const void*>(bson))),
            mongo::JsonStringFormat::LegacyStrict);
    }

    auto checkMatch(const char* filterJSON,
                    std::vector<const char*> documentsJSON,
                    stitch_support_v1_collator* collator = nullptr) {
        auto matcher = stitch_support_v1_matcher_create(
            lib, toBSONForAPI(filterJSON).first, collator, nullptr);
        ASSERT(matcher);
        ON_BLOCK_EXIT([matcher] { stitch_support_v1_matcher_destroy(matcher); });
        return std::all_of(
            documentsJSON.begin(), documentsJSON.end(), [=](const char* documentJSON) {
                bool isMatch;
                stitch_support_v1_check_match(
                    matcher, toBSONForAPI(documentJSON).first, &isMatch, nullptr);
                return isMatch;
            });
    }

    auto checkMatchStatus(const char* filterJSON,
                          const char* documentJSON,
                          stitch_support_v1_collator* collator = nullptr) {
        auto matchStatus = stitch_support_v1_status_create();
        ON_BLOCK_EXIT([matchStatus] { stitch_support_v1_status_destroy(matchStatus); });
        auto matcher = stitch_support_v1_matcher_create(
            lib, toBSONForAPI(filterJSON).first, collator, matchStatus);
        if (matcher) {
            stitch_support_v1_matcher_destroy(matcher);
            FAIL("Expected stich_support_v1_matcher_create to fail");
        }

        ASSERT_EQ(STITCH_SUPPORT_V1_ERROR_EXCEPTION,
                  stitch_support_v1_status_get_error(matchStatus));
        // Make sure that we get a proper code back but don't worry about its exact value.
        ASSERT_NE(0, stitch_support_v1_status_get_code(matchStatus));
        std::string explanation(stitch_support_v1_status_get_explanation(matchStatus));

        return explanation;
    }

    auto checkProjection(const char* specJSON,
                         std::vector<const char*> documentsJSON,
                         const char* filterJSON = nullptr,
                         stitch_support_v1_collator* collator = nullptr,
                         bool denyProjectionCollator = false) {
        stitch_support_v1_matcher* matcher = nullptr;
        if (filterJSON) {
            matcher = stitch_support_v1_matcher_create(
                lib, toBSONForAPI(filterJSON).first, collator, nullptr);
            ASSERT(matcher);
        }
        ON_BLOCK_EXIT([matcher] { stitch_support_v1_matcher_destroy(matcher); });

        auto projection =
            stitch_support_v1_projection_create(lib,
                                                toBSONForAPI(specJSON).first,
                                                matcher,
                                                denyProjectionCollator ? nullptr : collator,
                                                nullptr);
        ASSERT(projection);
        ON_BLOCK_EXIT([projection] { stitch_support_v1_projection_destroy(projection); });

        std::vector<std::string> results;
        std::transform(documentsJSON.begin(),
                       documentsJSON.end(),
                       std::back_inserter(results),
                       [=](const char* documentJSON) {
                           auto bson = stitch_support_v1_projection_apply(
                               projection, toBSONForAPI(documentJSON).first, nullptr);
                           auto result = fromBSONForAPI(bson);
                           stitch_support_v1_bson_free(bson);
                           return result;
                       });

        return std::make_pair(results, stitch_support_v1_projection_requires_match(projection));
    }

    auto checkProjectionStatus(const char* specJSON,
                               const char* documentJSON,
                               const char* filterJSON = nullptr,
                               stitch_support_v1_collator* collator = nullptr) {
        auto projectionStatus = stitch_support_v1_status_create();
        ON_BLOCK_EXIT([projectionStatus] { stitch_support_v1_status_destroy(projectionStatus); });

        stitch_support_v1_matcher* matcher = nullptr;
        if (filterJSON) {
            matcher = stitch_support_v1_matcher_create(
                lib, toBSONForAPI(filterJSON).first, collator, nullptr);
            ASSERT(matcher);
        }
        ON_BLOCK_EXIT([matcher] { stitch_support_v1_matcher_destroy(matcher); });

        auto projection = stitch_support_v1_projection_create(
            lib, toBSONForAPI(specJSON).first, matcher, collator, projectionStatus);
        if (projection) {
            stitch_support_v1_projection_destroy(projection);
            FAIL("Expected stich_support_v1_projection_create to fail");
        }

        ASSERT_EQ(STITCH_SUPPORT_V1_ERROR_EXCEPTION,
                  stitch_support_v1_status_get_error(projectionStatus));
        // Make sure that we get a proper code back but don't worry about its exact value.
        ASSERT_NE(0, stitch_support_v1_status_get_code(projectionStatus));

        return std::string(stitch_support_v1_status_get_explanation(projectionStatus));
    }

    auto checkUpdate(const char* expr,
                     const char* document,
                     const char* match = nullptr,
                     const char* arrayFilters = nullptr,
                     const char* collatorObj = nullptr) {
        stitch_support_v1_collator* collator = nullptr;
        if (collatorObj) {
            collator =
                stitch_support_v1_collator_create(lib, toBSONForAPI(collatorObj).first, nullptr);
        }
        ON_BLOCK_EXIT([collator] { stitch_support_v1_collator_destroy(collator); });

        stitch_support_v1_matcher* matcher = nullptr;
        if (match) {
            matcher =
                stitch_support_v1_matcher_create(lib, toBSONForAPI(match).first, collator, nullptr);
            ASSERT(matcher);
        }
        ON_BLOCK_EXIT([matcher] { stitch_support_v1_matcher_destroy(matcher); });

        stitch_support_v1_update* update = stitch_support_v1_update_create(
            lib,
            toBSONForAPI(expr).first,
            arrayFilters ? toBSONForAPI(arrayFilters).first : nullptr,
            matcher,
            collator,
            status);
        ASSERT(update);
        ON_BLOCK_EXIT([update] { stitch_support_v1_update_destroy(update); });

        // We assume that callers of this function will provide a 'match' argument if and only if
        // the update requires it.
        ASSERT_EQ(match != nullptr, stitch_support_v1_update_requires_match(update));

        auto updateResult = stitch_support_v1_update_apply(
            update, toBSONForAPI(document).first, updateDetails, status);
        ASSERT_EQ(0, stitch_support_v1_status_get_code(status))
            << stitch_support_v1_status_get_error(status) << ":"
            << stitch_support_v1_status_get_explanation(status);
        ASSERT(updateResult);
        ON_BLOCK_EXIT([updateResult] { stitch_support_v1_bson_free(updateResult); });

        return std::string(fromBSONForAPI(updateResult));
    }

    auto checkUpdateStatus(const char* expr,
                           const char* document,
                           const char* match = nullptr,
                           const char* arrayFilters = nullptr,
                           const char* collatorObj = nullptr) {
        auto updateStatus = stitch_support_v1_status_create();
        ON_BLOCK_EXIT([updateStatus] { stitch_support_v1_status_destroy(updateStatus); });

        stitch_support_v1_collator* collator = nullptr;
        if (collatorObj) {
            collator =
                stitch_support_v1_collator_create(lib, toBSONForAPI(collatorObj).first, nullptr);
        }
        ON_BLOCK_EXIT([collator] { stitch_support_v1_collator_destroy(collator); });

        stitch_support_v1_matcher* matcher = nullptr;
        if (match) {
            matcher =
                stitch_support_v1_matcher_create(lib, toBSONForAPI(match).first, collator, nullptr);
            ASSERT(matcher);
        }
        ON_BLOCK_EXIT([matcher] { stitch_support_v1_matcher_destroy(matcher); });

        stitch_support_v1_update* update = stitch_support_v1_update_create(
            lib,
            toBSONForAPI(expr).first,
            arrayFilters ? toBSONForAPI(arrayFilters).first : nullptr,
            matcher,
            collator,
            updateStatus);

        if (!update) {
            ASSERT_EQ(STITCH_SUPPORT_V1_ERROR_EXCEPTION,
                      stitch_support_v1_status_get_error(updateStatus));
            // Make sure that we get a proper code back but don't worry about its exact value.
            ASSERT_NE(0, stitch_support_v1_status_get_code(updateStatus));
        } else {
            ON_BLOCK_EXIT([update] { stitch_support_v1_update_destroy(update); });
            auto updateResult = stitch_support_v1_update_apply(
                update, toBSONForAPI(document).first, updateDetails, updateStatus);
            ASSERT_NE(0, stitch_support_v1_status_get_code(updateStatus));
            ASSERT(!updateResult);
        }
        return std::string(stitch_support_v1_status_get_explanation(updateStatus));
    }

    std::string getModifiedPaths() {
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

    auto checkUpsert(const char* expr, const char* match = nullptr) {
        stitch_support_v1_matcher* matcher = nullptr;
        if (match) {
            matcher =
                stitch_support_v1_matcher_create(lib, toBSONForAPI(match).first, nullptr, nullptr);
            ASSERT(matcher);
        }
        ON_BLOCK_EXIT([matcher] { stitch_support_v1_matcher_destroy(matcher); });

        stitch_support_v1_update* update = stitch_support_v1_update_create(
            lib, toBSONForAPI(expr).first, nullptr, matcher, nullptr, status);
        ON_BLOCK_EXIT([update] { stitch_support_v1_update_destroy(update); });

        uint8_t* upsertResult = stitch_support_v1_update_upsert(update, status);
        ASSERT(upsertResult);
        ON_BLOCK_EXIT([upsertResult] { stitch_support_v1_bson_free(upsertResult); });

        return std::string(fromBSONForAPI(upsertResult));
    }

    auto checkUpsertStatus(const char* expr, const char* match) {
        auto updateStatus = stitch_support_v1_status_create();
        ON_BLOCK_EXIT([updateStatus] { stitch_support_v1_status_destroy(updateStatus); });

        stitch_support_v1_matcher* matcher =
            stitch_support_v1_matcher_create(lib, toBSONForAPI(match).first, nullptr, nullptr);
        ASSERT(matcher);
        ON_BLOCK_EXIT([matcher] { stitch_support_v1_matcher_destroy(matcher); });

        stitch_support_v1_update* update = stitch_support_v1_update_create(
            lib, toBSONForAPI(expr).first, nullptr, matcher, nullptr, updateStatus);

        if (!update) {
            ASSERT_EQ(STITCH_SUPPORT_V1_ERROR_EXCEPTION,
                      stitch_support_v1_status_get_error(updateStatus));
            // Make sure that we get a proper code back but don't worry about its exact value.
            ASSERT_NE(0, stitch_support_v1_status_get_code(updateStatus));
        } else {
            ON_BLOCK_EXIT([update] { stitch_support_v1_update_destroy(update); });
            auto upsertResult = stitch_support_v1_update_upsert(update, updateStatus);
            ASSERT_NE(0, stitch_support_v1_status_get_code(updateStatus));
            ASSERT(!upsertResult);
        }
        return std::string(stitch_support_v1_status_get_explanation(updateStatus));
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
    ASSERT_EQ("unknown operator: $bogus", checkMatchStatus("{a: {$bogus: 1}}", "{a: 1}"));
    ASSERT_EQ("$where is not allowed in this context",
              checkMatchStatus("{$where: 'this.a == 1'}", "{a: 1}"));
    ASSERT_EQ("$text is not allowed in this context",
              checkMatchStatus("{$text: {$search: 'stitch'}}", "{a: 'stitch lib'}"));
    ASSERT_EQ("$geoNear, $near, and $nearSphere are not allowed in this context",
              checkMatchStatus(
                  "{location: {$near: {$geometry: {type: 'Point', "
                  "coordinates: [ -73.9667, 40.78 ] }, $minDistance: 10, $maxDistance: 500}}}",
                  "{type: 'Point', 'coordinates': [100.0, 0.0]}"));

    // 'check_match' cannot actually fail so we do not test it with a status.
}

TEST_F(StitchSupportTest, CheckMatchWorksWithCollation) {
    auto collator = stitch_support_v1_collator_create(
        lib, toBSONForAPI("{locale: 'en', strength: 2}").first, nullptr);
    ON_BLOCK_EXIT([collator] { stitch_support_v1_collator_destroy(collator); });
    ASSERT_TRUE(checkMatch("{a: 'word'}", {"{a: 'WORD', b: 'other'}"}, collator));
}

TEST_F(StitchSupportTest, CheckProjectionWorksWithDefaults) {
    auto [results, needsMatch] =
        checkProjection("{a: 1}", {"{_id: 1, a: 100, b: 200}", "{_id: 1, a: 200, b: 300}"});
    ASSERT_FALSE(needsMatch);
    ASSERT_EQ("{ \"_id\" : 1, \"a\" : 100 }", results[0]);
    ASSERT_EQ("{ \"_id\" : 1, \"a\" : 200 }", results[1]);

    std::tie(results, needsMatch) =
        checkProjection("{'a.$': 1}",
                        {"{_id: 1, a: [{b: 2, c: 100}, {b: 1, c: 200}]}",
                         "{_id: 1, a: [{b: 1, c: 100, d: 45}, {b: 2, c: 200}]}"},
                        "{'a.b': 1}");
    ASSERT_TRUE(needsMatch);
    ASSERT_EQ("{ \"_id\" : 1, \"a\" : [ { \"b\" : 1, \"c\" : 200 } ] }", results[0]);
    ASSERT_EQ("{ \"_id\" : 1, \"a\" : [ { \"b\" : 1, \"c\" : 100, \"d\" : 45 } ] }", results[1]);

    std::tie(results, needsMatch) =
        checkProjection("{a: {$elemMatch: {b: 2}}}", {"{a: [{b: 1, c: 1}, {b: 2, c: 2}]}"});
    ASSERT_FALSE(needsMatch);
    ASSERT_EQ("{ \"a\" : [ { \"b\" : 2, \"c\" : 2 } ] }", results[0]);

    std::tie(results, needsMatch) = checkProjection("{a: {$slice: [1, 2]}}", {"{a: [1, 2, 3, 4]}"});
    ASSERT_FALSE(needsMatch);
    ASSERT_EQ("{ \"a\" : [ 2, 3 ] }", results[0]);
}

TEST_F(StitchSupportTest, CheckProjectionProducesExpectedStatus) {
    ASSERT_EQ("Projections with a positional operator require a matcher",
              checkProjectionStatus("{'a.$': 1}", "{_id: 1, a: [{b: 2, c: 100}, {b: 1, c: 200}]}"));
    ASSERT_EQ("$textScore, $sortKey, $recordId and $geoNear are not allowed in this context",
              checkProjectionStatus("{a: {$meta: 'textScore'}}", "{_id: 1, a: 100, b: 200}"));

    ASSERT_EQ("Cannot do inclusion on field c in exclusion projection",
              checkProjectionStatus("{a: 0, c: 1}", "{_id: 1, a: {b: 200}}"));
}

TEST_F(StitchSupportTest, CheckProjectionCollatesRespectfully) {
    auto collator = stitch_support_v1_collator_create(
        lib, toBSONForAPI("{locale: 'en', strength: 2}").first, nullptr);
    ON_BLOCK_EXIT([collator] { stitch_support_v1_collator_destroy(collator); });

    auto [results, needsMatch] =
        checkProjection("{a: {$elemMatch: {$eq: 'MiXedcAse'}}}",
                        {"{_id: 1, a: ['lowercase', 'mixEdCaSe', 'UPPERCASE']}"},
                        nullptr,
                        collator);
    ASSERT_FALSE(needsMatch);
    ASSERT_EQ("{ \"_id\" : 1, \"a\" : [ \"mixEdCaSe\" ] }", results[0]);

    // Ignore a matcher's collator.
    std::tie(results, needsMatch) =
        checkProjection("{a: {$elemMatch: {$eq: 'MiXedcAse'}}}",
                        {"{_id: 1, a: ['lowercase', 'mixEdCaSe', 'UPPERCASE']}"},
                        "{_id: 1}",
                        collator,
                        true);
    ASSERT_FALSE(needsMatch);
    ASSERT_EQ("{ \"_id\" : 1 }", results[0]);
}

TEST_F(StitchSupportTest, TestUpdateSingleElement) {
    ASSERT_EQ("{ \"a\" : 2 }", checkUpdate("{$set: {a: 2}}", "{a: 1}"));
    ASSERT_EQ("[a]", getModifiedPaths());
}

TEST_F(StitchSupportTest, TestReplacementStyleUpdateReportsNoModifiedPaths) {
    // Replacement-style updates report no modified paths because this functionality is not
    // currently needed by Stitch.
    ASSERT_EQ("{ \"a\" : 2 }", checkUpdate("{a: 2}", "{a: 1}"));
    ASSERT_EQ("[]", getModifiedPaths());
}

TEST_F(StitchSupportTest, TestReplacementStyleUpdatePreservesId) {
    ASSERT_EQ("{ \"_id\" : 123, \"b\" : 789 }", checkUpdate("{b: 789}", "{_id: 123, a: 456}"));
}

TEST_F(StitchSupportTest, TestReplacementZeroTimestamp) {
    auto result =
        mongo::fromjson(checkUpdate("{b: Timestamp(0, 0)}", "{_id: 123, a: 456}").c_str());
    auto elemB = result["b"];
    ASSERT_TRUE(elemB.ok());
    ASSERT_EQUALS(elemB.type(), mongo::BSONType::bsonTimestamp);
    auto ts = elemB.timestamp();
    ASSERT_NOT_EQUALS(0U, ts.getSecs());
    ASSERT_NOT_EQUALS(0U, ts.getInc());
}

TEST_F(StitchSupportTest, TestUpdateCurrentDateTimestamp) {
    auto result = mongo::fromjson(
        checkUpdate("{$currentDate: {b: {$type: 'timestamp'}}}", "{_id: 123, a: 456}").c_str());
    auto elemB = result["b"];
    ASSERT_TRUE(elemB.ok());
    ASSERT_EQUALS(elemB.type(), mongo::BSONType::bsonTimestamp);
    auto ts = elemB.timestamp();
    ASSERT_NOT_EQUALS(0U, ts.getSecs());
    ASSERT_NOT_EQUALS(0U, ts.getInc());
}

TEST_F(StitchSupportTest, TestUpdatePipelineClusterTime) {
    auto result = mongo::fromjson(
        checkUpdate("[{$set: {b: '$$CLUSTER_TIME'}}]", "{_id: 123, a: 456}").c_str());
    auto elemB = result["b"];
    ASSERT_FALSE(elemB.ok());
}

TEST_F(StitchSupportTest, TestUpdateArrayElement) {
    ASSERT_EQ("{ \"a\" : [ 2, 2 ] }", checkUpdate("{$set: {'a.0': 2}}", "{a: [1, 2]}"));
    ASSERT_EQ("[a.0]", getModifiedPaths());

    ASSERT_EQ("{ \"a\" : [ { \"b\" : 2 } ] }",
              checkUpdate("{$set: {'a.0.b': 2}}", "{a: [{b: 1}]}"));
    ASSERT_EQ("[a.0.b]", getModifiedPaths());
}

TEST_F(StitchSupportTest, TestUpdateAddToArray) {
    ASSERT_EQ("{ \"a\" : [ { \"b\" : 1 }, { \"b\" : 2 } ] }",
              checkUpdate("{$set: {'a.1.b': 2}}", "{a: [{b: 1}]}"));
    ASSERT_EQ("[a]", getModifiedPaths());

    ASSERT_EQ("{ \"a\" : [ { \"b\" : 1 }, { \"b\" : 2 } ], \"c\" : 3 }",
              checkUpdate("{$set: {'a.1.b': 2, c: 3}}", "{a: [{b: 1}]}"));
    ASSERT_EQ("[a, c]", getModifiedPaths());
}

TEST_F(StitchSupportTest, TestUpdatePullFromArray) {
    ASSERT_EQ("{ \"a\" : [ 3, 1 ] }", checkUpdate("{$pull: {'a': 2}}", "{a: [3, 2, 1]}"));
    ASSERT_EQ("[a]", getModifiedPaths());
}

TEST_F(StitchSupportTest, TestPositionalUpdates) {
    ASSERT_EQ("{ \"a\" : [ 1, 3 ] }", checkUpdate("{$set: {'a.$': 3}}", "{a: [1, 2]}", "{a: 2}"));
    ASSERT_EQ("[a.1]", getModifiedPaths());

    ASSERT_EQ("{ \"a\" : [ { \"b\" : 1 }, { \"b\" : 3 } ] }",
              checkUpdate("{$set: {'a.$.b': 3}}", "{a: [{b: 1}, {b: 2}]}", "{'a.b': 2}"));
    ASSERT_EQ("[a.1.b]", getModifiedPaths());
}

TEST_F(StitchSupportTest, TestUpdatesWithArrayFilters) {
    ASSERT_EQ("{ \"a\" : [ 1, 3 ] }",
              checkUpdate("{$set: {'a.$[i]': 3}}", "{a: [1, 2]}", nullptr, "[{'i': 2}]"));
    ASSERT_EQ("[a.1]", getModifiedPaths());

    ASSERT_EQ(
        "{ \"a\" : [ { \"b\" : 1 }, { \"b\" : 3 } ] }",
        checkUpdate("{$set: {'a.$[i].b': 3}}", "{a: [{b: 1}, {b: 2}]}", nullptr, "[{'i.b': 2}]"));
    ASSERT_EQ("[a.1.b]", getModifiedPaths());
}

TEST_F(StitchSupportTest, TestUpdateRespectsTheCollation) {
    auto caseInsensitive = "{locale: 'en', strength: 2}";
    ASSERT_EQ("{ \"a\" : [ \"Santa\", \"Elf\" ] }",
              checkUpdate("{$addToSet: {a: 'santa'}}",
                          "{a: ['Santa', 'Elf']}",
                          nullptr,
                          nullptr,
                          caseInsensitive));
    // $addToSet with existing element is considered a no-op, but the array is marked as modified.
    ASSERT_EQ("[a]", getModifiedPaths());

    ASSERT_EQ(
        "{ \"a\" : [ \"Elf\" ] }",
        checkUpdate(
            "{$pull: {a: 'santa'}}", "{a: ['Santa', 'Elf']}", nullptr, nullptr, caseInsensitive));
    ASSERT_EQ("[a]", getModifiedPaths());
}

TEST_F(StitchSupportTest, TestUpdateWithSetOnInsert) {
    ASSERT_EQ("{ \"a\" : 1 }", checkUpdate("{$setOnInsert: {a: 2}}", "{a: 1}"));
    ASSERT_EQ("[a]", getModifiedPaths());
}

TEST_F(StitchSupportTest, TestUpdateProducesProperStatus) {
    ASSERT_EQ(
        "Unknown modifier: $bogus. Expected a valid update modifier or pipeline-style update "
        "specified as an array",
        checkUpdateStatus("{$bogus: {a: 2}}", "{a: 1}"));
    ASSERT_EQ("Updating the path 'a' would create a conflict at 'a'",
              checkUpdateStatus("{$set: {a: 2, a: 3}}", "{a: 1}"));
    ASSERT_EQ("No array filter found for identifier 'i' in path 'a.$[i]'",
              checkUpdateStatus("{$set: {'a.$[i]': 3}}", "{a: [1, 2]}"));
    ASSERT_EQ("No array filter found for identifier 'i' in path 'a.$[i]'",
              checkUpdateStatus("{$set: {'a.$[i]': 3}}", "{a: [1, 2]}", nullptr, "[{'j': 2}]"));
    ASSERT_EQ("Update created a conflict at 'a.0'",
              checkUpdateStatus(
                  "{$set: {'a.$[i]': 2, 'a.$[j]': 3}}", "{a: [0]}", nullptr, " [{i: 0}, {j:0}]"));
}

TEST_F(StitchSupportTest, TestUpsert) {
    ASSERT_EQ("{ \"_id\" : 1, \"a\" : 1 }", checkUpsert("{$set: {a: 1}}", "{_id: 1}"));
    ASSERT_EQ("{ \"a\" : 2 }", checkUpsert("{$set: {a: 2}}", "{a: 1}"));
    ASSERT_EQ("{ \"_id\" : 1, \"a\" : 1 }", checkUpsert("{$setOnInsert: {a: 1}}", "{_id: 1}"));
    ASSERT_EQ("{ \"_id\" : 1, \"b\" : 1 }", checkUpsert("{$inc: {b: 1}}", "{_id: 1, a: {$gt: 2}}"));
    // Document replace overrides matcher.
    ASSERT_EQ("{}", checkUpsert("{}", "{a: 1}"));
}

TEST_F(StitchSupportTest, TestUpsertWithoutMatcher) {
    ASSERT_EQ("{ \"a\" : 1 }", checkUpsert("{a: 1}"));
    ASSERT_EQ("{ \"a\" : [ { \"b\" : 2 }, false ] }", checkUpsert("{a: [{b: 2}, false]}"));
    ASSERT_EQ("{}", checkUpsert("{}"));
}

TEST_F(StitchSupportTest, TestUpsertEmptyMatcher) {
    ASSERT_EQ("{ \"a\" : 1 }", checkUpsert("{$set: {a: 1}}", "{}"));
    ASSERT_EQ("{ \"a\" : 1 }", checkUpsert("{$setOnInsert: {a: 1}}", "{}"));
    ASSERT_EQ("{ \"b\" : 1 }", checkUpsert("{$inc: {b: 1}}", "{}"));
    ASSERT_EQ("{ \"a\" : 1 }", checkUpsert("{a: 1}", "{}"));
    ASSERT_EQ("{ \"a\" : [ { \"b\" : 2 }, false ] }", checkUpsert("{a: [{b: 2}, false]}", "{}"));
}

TEST_F(StitchSupportTest, TestUpsertWithReplacementUpdate) {
    ASSERT_EQ("{ \"_id\" : 1, \"a\" : 2 }", checkUpsert("{a: 2}", "{_id: 1}"));
    ASSERT_EQ("{ \"_id\" : 1, \"a\" : 2 }", checkUpsert("{a: 2}", "{$and: [{_id: 1}]}"));

    // Upsert with replacement update ues the '_id' field from the query but not any other fields.
    ASSERT_EQ("{ \"_id\" : 1, \"b\" : 4 }", checkUpsert("{b: 4}", "{_id: 1, a: 2, b: 3}"));
}

TEST_F(StitchSupportTest, TestUpsertProducesProperStatus) {
    ASSERT_EQ("Cannot apply array updates to non-array element a: 1",
              checkUpsertStatus("{$set: {'a.$[].b': 1}}", "{a: 1}"));
}

}  // namespace

// Define main function as an entry to these tests.
//
// Note that we don't use the main() defined for most other unit tests so that we can avoid double
// calling runGlobalInitializers(), which is called both from the regular unit test main() and from
// the Stitch Support Library intializer function that gets tested here.
int main(const int argc, const char* const* const argv) {
    // See comment by the same code block in mongo_embedded_test.cpp
    auto ret = mongo::runGlobalInitializers(std::vector<std::string>{argv, argv + argc});
    if (!ret.isOK()) {
        std::cerr << "Global initilization failed";
        return static_cast<int>(mongo::ExitCode::fail);
    }

    ret = mongo::runGlobalDeinitializers();
    if (!ret.isOK()) {
        std::cerr << "Global deinitilization failed";
        return static_cast<int>(mongo::ExitCode::fail);
    }

    const auto result = ::mongo::unittest::Suite::run(std::vector<std::string>(), "", "", 1);

    // This is the standard exit path for Mongo processes. See the mongo::quickExit() declaration
    // for more information.
    mongo::quickExit(result);
}
