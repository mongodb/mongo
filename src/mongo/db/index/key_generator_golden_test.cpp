/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/2d_key_generator.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index/s2_key_generator.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <algorithm>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

/**
 * Golden data regression tests for index key generation.
 *
 * These tests pin the output of every index key generator (btree, 2d, 2dsphere, hashed, wildcard,
 * text) against checked-in golden files.
 */
namespace mongo {
namespace {

using unittest::GoldenTestConfig;
using unittest::GoldenTestContext;

GoldenTestConfig goldenTestConfig{"src/mongo/db/index/expected_output"};

/**
 * Converts a KeyStringSet to a sorted vector of BSON objects for deterministic, human-readable
 * golden output.
 */
std::vector<BSONObj> keySetToBsonVector(const KeyStringSet& keyStrings, Ordering ordering) {
    std::vector<BSONObj> result;
    for (auto& ks : keyStrings) {
        result.push_back(key_string::toBson(ks, ordering));
    }
    // The KeyStringSet is already sorted by key_string ordering, but we want deterministic output;
    // sorting by BSON woCompare ensures a canonical text representation.
    std::sort(result.begin(), result.end(), SimpleBSONObjComparator::kInstance.makeLessThan());
    return result;
}

/**
 * Prints a labeled key generation test case to the golden output stream.
 */
void printTestCase(std::ostream& os,
                   const std::string& label,
                   const BSONObj& inputDoc,
                   const BSONObj& keyPattern,
                   const KeyStringSet& keys,
                   Ordering ordering) {
    os << "--- " << label << " ---\n";
    os << "Input: " << inputDoc.jsonString(JsonStringFormat::LegacyStrict) << "\n";
    os << "KeyPattern: " << keyPattern.jsonString(JsonStringFormat::LegacyStrict) << "\n";

    auto bsonKeys = keySetToBsonVector(keys, ordering);
    os << "Keys (" << bsonKeys.size() << "):\n";
    for (auto& key : bsonKeys) {
        os << "  " << key.jsonString(JsonStringFormat::LegacyStrict) << "\n";
    }
    os << "\n";
}

// ---------------------------------------------------------------------------
// Btree Keys
// ---------------------------------------------------------------------------
TEST(KeyGeneratorGoldenTest, BtreeKeys) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    auto runBtreeTest = [&](const std::string& label,
                            const BSONObj& keyPattern,
                            const BSONObj& inputDoc,
                            bool sparse = false) {
        std::vector<const char*> fieldNames;
        std::vector<BSONElement> fixed;
        for (auto it = keyPattern.begin(); it != keyPattern.end(); ++it) {
            fieldNames.push_back((*it).fieldName());
            fixed.push_back(BSONElement());
        }

        auto keyGen = std::make_unique<BtreeKeyGenerator>(fieldNames,
                                                          fixed,
                                                          sparse,
                                                          key_string::Version::kLatestVersion,
                                                          Ordering::make(BSONObj()));

        SharedBufferFragmentBuilder allocator(BufBuilder::kDefaultInitSizeBytes);
        KeyStringSet keys;
        MultikeyPaths multikeyPaths;
        keyGen->getKeys(allocator, inputDoc, false, &keys, &multikeyPaths);

        printTestCase(os, label, inputDoc, keyPattern, keys, Ordering::make(BSONObj()));
    };

    // Compound index (3-field with descending; not directly tested elsewhere).
    runBtreeTest("CompoundIndex", fromjson("{a: 1, b: -1, c: 1}"), fromjson("{a: 1, b: 2, c: 3}"));

    // Nested object as index key value.
    runBtreeTest("NestedObject", fromjson("{a: 1}"), fromjson("{a: {x: 1, y: 2}}"));

    // Sparse index with missing field (should produce no keys).
    runBtreeTest("SparseMissing",
                 fromjson("{a: 1}"),
                 fromjson("{b: 1}"),
                 /* sparse= */ true);

    // Array of objects with top-level array and deeply dotted path.
    runBtreeTest("ArrayOfObjectsDotted",
                 fromjson("{'a.b.c': 1}"),
                 fromjson("{a: [{b: {c: 10}}, {b: {c: 20}}]}"));

    // Array of arrays (nested arrays).
    runBtreeTest("ArrayOfArrays", fromjson("{a: 1}"), fromjson("{a: [[1, 2], [3, 4]]}"));

    // --- Compound edge cases ---

    // Compound with both fields missing.
    runBtreeTest("CompoundBothMissing", fromjson("{a: 1, b: 1}"), fromjson("{c: 1}"));

    // Compound with deeply dotted paths.
    runBtreeTest("CompoundDeeplyDotted",
                 fromjson("{'a.b.c': 1, 'd.e.f': 1}"),
                 fromjson("{a: {b: {c: 1}}, d: {e: {f: 2}}}"));

    // --- Special BSON types ---

    // Undefined value (from BSON builder since fromjson can't express it).
    {
        BSONObjBuilder bob;
        bob.appendUndefined("a");
        runBtreeTest("UndefinedValue", fromjson("{a: 1}"), bob.obj());
    }

    // MinKey / MaxKey.
    {
        BSONObjBuilder bob;
        bob.appendMinKey("a");
        runBtreeTest("MinKeyValue", fromjson("{a: 1}"), bob.obj());
    }
    {
        BSONObjBuilder bob;
        bob.appendMaxKey("a");
        runBtreeTest("MaxKeyValue", fromjson("{a: 1}"), bob.obj());
    }

    // Regex value.
    runBtreeTest("RegexValue", fromjson("{a: 1}"), BSON("a" << BSONRegEx("^abc", "i")));

    // Date value.
    runBtreeTest("DateValue",
                 fromjson("{a: 1}"),
                 BSON("a" << Date_t::fromMillisSinceEpoch(1234567890000LL)));

    // Sparse compound where one field present and one missing.
    runBtreeTest("SparseCompoundPartialMissing",
                 fromjson("{a: 1, b: 1}"),
                 fromjson("{a: 5}"),
                 /* sparse= */ true);
}

// ---------------------------------------------------------------------------
// 2d Key Generator
// ---------------------------------------------------------------------------
TEST(KeyGeneratorGoldenTest, TwoDKeys) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    auto run2DTest =
        [&](const std::string& label, const BSONObj& infoObj, const BSONObj& inputDoc) {
            TwoDIndexingParams params;
            index2d::parse2dParams(infoObj, &params);

            SharedBufferFragmentBuilder allocator(BufBuilder::kDefaultInitSizeBytes);
            KeyStringSet keys;
            index2d::get2DKeys(allocator,
                               inputDoc,
                               params,
                               &keys,
                               key_string::Version::kLatestVersion,
                               Ordering::make(BSONObj()));

            BSONObj keyPattern = infoObj["key"].Obj();
            printTestCase(os, label, inputDoc, keyPattern, keys, Ordering::make(BSONObj()));
        };

    // Simple point.
    run2DTest("SimplePoint", fromjson("{key: {a: '2d'}}"), fromjson("{a: [0, 0]}"));

    // Embedded object location.
    run2DTest("EmbeddedObject", fromjson("{key: {a: '2d'}}"), fromjson("{a: {x: 5, y: 10}}"));

    // Negative coordinates.
    run2DTest("NegativeCoords", fromjson("{key: {loc: '2d'}}"), fromjson("{loc: [-73.97, 40.77]}"));

    // Dotted path trailing field.
    run2DTest("DottedTrailingField",
              fromjson("{key: {a: '2d', 'b.c': 1}}"),
              fromjson("{a: [0, 0], b: {c: 42}}"));

    // Large coordinates.
    run2DTest("LargeCoords", fromjson("{key: {a: '2d'}}"), fromjson("{a: [179.5, 89.5]}"));

    // --- Boundary and special value edge cases ---

    // Near-boundary coordinates (close to default min/max -180/180).
    run2DTest("NearMinBoundary", fromjson("{key: {a: '2d'}}"), fromjson("{a: [-179.99, -89.99]}"));

    // Fractional coordinates with many decimal places.
    run2DTest("HighPrecisionCoords",
              fromjson("{key: {a: '2d'}}"),
              fromjson("{a: [1.123456789, 2.987654321]}"));

    // Nested location field via dotted path.
    run2DTest("DottedGeoPath",
              fromjson("{key: {'loc.position': '2d'}}"),
              fromjson("{loc: {position: [10, 20]}}"));

    // Multiple trailing fields.
    run2DTest("MultipleTrailingFields",
              fromjson("{key: {a: '2d', b: 1, c: 1}}"),
              fromjson("{a: [1, 2], b: 'hello', c: 42}"));

    // Missing location field (should produce no keys).
    run2DTest("MissingLocationField", fromjson("{key: {a: '2d'}}"), fromjson("{b: [1, 2]}"));

    // Null trailing field.
    run2DTest(
        "NullTrailingField", fromjson("{key: {a: '2d', b: 1}}"), fromjson("{a: [5, 10], b: null}"));

    // Missing trailing field.
    run2DTest("MissingTrailingField", fromjson("{key: {a: '2d', b: 1}}"), fromjson("{a: [5, 10]}"));
}

// ---------------------------------------------------------------------------
// 2dsphere Key Generator
// ---------------------------------------------------------------------------
TEST(KeyGeneratorGoldenTest, S2Keys) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    auto runS2Test = [&](const std::string& label,
                         const BSONObj& keyPattern,
                         const BSONObj& infoObj,
                         const BSONObj& inputDoc) {
        S2IndexingParams params;
        const CollatorInterface* collator = nullptr;
        index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

        SharedBufferFragmentBuilder allocator(BufBuilder::kDefaultInitSizeBytes);
        KeyStringSet keys;
        MultikeyPaths multikeyPaths;
        index2dsphere::getS2Keys(allocator,
                                 inputDoc,
                                 keyPattern,
                                 params,
                                 &keys,
                                 &multikeyPaths,
                                 key_string::Version::kLatestVersion,
                                 SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                                 Ordering::make(BSONObj()));

        printTestCase(os, label, inputDoc, keyPattern, keys, Ordering::make(BSONObj()));
    };

    // --- Version 3 (default) tests ---
    BSONObj v3InfoObj = fromjson("{key: {a: '2dsphere'}, '2dsphereIndexVersion': 3}");
    BSONObj v3KeyPattern = fromjson("{a: '2dsphere'}");

    // Point.
    runS2Test("V3_Point",
              v3KeyPattern,
              v3InfoObj,
              fromjson("{a: {type: 'Point', coordinates: [40.0, -73.0]}}"));

    // MultiPoint.
    runS2Test("V3_MultiPoint",
              v3KeyPattern,
              v3InfoObj,
              fromjson("{a: {type: 'MultiPoint', coordinates: [[0, 0], [1, 0], [1, 1]]}}"));

    // Compound with non-geo field.
    BSONObj compoundInfoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    BSONObj compoundKeyPattern = fromjson("{a: '2dsphere', b: 1}");

    runS2Test("V3_CompoundNonGeo",
              compoundKeyPattern,
              compoundInfoObj,
              fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 42}"));

    // Compound with non-geo field before geo.
    BSONObj compoundBeforeInfoObj =
        fromjson("{key: {b: 1, a: '2dsphere'}, '2dsphereIndexVersion': 3}");
    BSONObj compoundBeforeKeyPattern = fromjson("{b: 1, a: '2dsphere'}");

    runS2Test("V3_CompoundNonGeoBefore",
              compoundBeforeKeyPattern,
              compoundBeforeInfoObj,
              fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 'hello'}"));

    // Missing geo field (should produce no keys for v3).
    runS2Test("V3_MissingGeoField", v3KeyPattern, v3InfoObj, fromjson("{b: 1}"));

    // Array of geo objects.
    BSONObj arrayInfoObj = fromjson("{key: {'a.b.geo': '2dsphere'}, '2dsphereIndexVersion': 3}");
    BSONObj arrayKeyPattern = fromjson("{'a.b.geo': '2dsphere'}");

    runS2Test("V3_ArrayOfGeoObjects",
              arrayKeyPattern,
              arrayInfoObj,
              fromjson("{a: {b: [{geo: {type: 'Point', coordinates: [0, 0]}}, "
                       "{geo: {type: 'Point', coordinates: [3, 3]}}]}}"));

    // --- Version 2 tests ---
    BSONObj v2InfoObj = fromjson("{key: {a: '2dsphere'}, '2dsphereIndexVersion': 2}");

    runS2Test("V2_Point",
              v3KeyPattern,
              v2InfoObj,
              fromjson("{a: {type: 'Point', coordinates: [40.0, -73.0]}}"));

    // --- Legacy coordinate pairs ---
    // 2dsphere indexes (v3+) accept legacy coordinate pairs [lon, lat] as well as GeoJSON.
    runS2Test("V3_LegacyCoordPair", v3KeyPattern, v3InfoObj, fromjson("{a: [40.0, -73.0]}"));

    // Legacy embedded coordinate object {lon: N, lat: N}.
    runS2Test("V3_LegacyEmbeddedCoordObject",
              v3KeyPattern,
              v3InfoObj,
              fromjson("{a: {x: 40.0, y: -73.0}}"));

    // --- Null, undefined, empty-array, empty-object geo field ---
    // For V2+ these should produce no keys since there's no valid geo field.
    runS2Test("V3_NullGeoField", v3KeyPattern, v3InfoObj, fromjson("{a: null}"));

    runS2Test("V3_EmptyArrayGeoField", v3KeyPattern, v3InfoObj, fromjson("{a: []}"));

    runS2Test("V3_EmptyObjectGeoField", v3KeyPattern, v3InfoObj, fromjson("{a: {}}"));

    // --- Compound with array in non-geo field (multikey) ---
    runS2Test("V3_CompoundArrayNonGeo",
              compoundKeyPattern,
              compoundInfoObj,
              fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: [1, 2, 3]}"));

    // --- Compound with missing non-geo field ---
    runS2Test("V3_CompoundMissingNonGeo",
              compoundKeyPattern,
              compoundInfoObj,
              fromjson("{a: {type: 'Point', coordinates: [0, 0]}}"));

    // --- Compound with empty array non-geo field (leading) ---
    BSONObj compoundLeadArrayInfoObj =
        fromjson("{key: {c: 1, a: '2dsphere'}, '2dsphereIndexVersion': 3}");
    BSONObj compoundLeadArrayKeyPattern = fromjson("{c: 1, a: '2dsphere'}");

    runS2Test("V3_CompoundEmptyArrayLeading",
              compoundLeadArrayKeyPattern,
              compoundLeadArrayInfoObj,
              fromjson("{c: [], a: {type: 'Point', coordinates: [0, 0]}}"));

    // --- Multiple geo fields in compound ---
    BSONObj multiGeoInfoObj =
        fromjson("{key: {a: '2dsphere', b: '2dsphere'}, '2dsphereIndexVersion': 3}");
    BSONObj multiGeoKeyPattern = fromjson("{a: '2dsphere', b: '2dsphere'}");

    runS2Test("V3_MultipleGeoFields",
              multiGeoKeyPattern,
              multiGeoInfoObj,
              fromjson("{a: {type: 'Point', coordinates: [0, 0]}, "
                       "b: {type: 'Point', coordinates: [1, 1]}}"));

    // --- V2 legacy coordinate pair (v2 also supports legacy) ---
    runS2Test("V2_LegacyCoordPair", v3KeyPattern, v2InfoObj, fromjson("{a: [40.0, -73.0]}"));

    // ===========================================================================================
    // V4 changed the parsing order for object-type geometry elements: GeoJSON is attempted first,
    // and legacy point parsing is used only as a fallback. Pre-V4, if the first BSON field of the
    // geo element was numeric, the legacy point parser took priority even when valid GeoJSON was
    // present. This section pins V4 behavior and explicitly tests the parsing-priority divergence
    // that motivated bumping the index version.
    // ===========================================================================================
    BSONObj v4InfoObj = fromjson("{key: {a: '2dsphere'}, '2dsphereIndexVersion': 4}");
    BSONObj v4KeyPattern = fromjson("{a: '2dsphere'}");

    // --- V4 basic GeoJSON (should match V3 for pure GeoJSON documents) ---
    runS2Test("V4_Point",
              v4KeyPattern,
              v4InfoObj,
              fromjson("{a: {type: 'Point', coordinates: [40.0, -73.0]}}"));

    // V4 legacy coordinate pair (arrays always use legacy parser regardless of version).
    runS2Test("V4_LegacyCoordPair", v4KeyPattern, v4InfoObj, fromjson("{a: [40.0, -73.0]}"));

    // V4 legacy embedded coordinate object (GeoJSON fails, falls back to legacy).
    runS2Test("V4_LegacyEmbeddedCoordObject",
              v4KeyPattern,
              v4InfoObj,
              fromjson("{a: {x: 40.0, y: -73.0}}"));

    // --- Ambiguous documents: both legacy-looking fields and valid GeoJSON ---
    BSONObj ambiguousDoc =
        fromjson("{a: {x: 40.0, y: -70.0, type: 'Point', coordinates: [-70.0, 40.0]}}");

    runS2Test("V3_AmbiguousLegacyAndGeoJSON", v3KeyPattern, v3InfoObj, ambiguousDoc);

    runS2Test("V4_AmbiguousLegacyAndGeoJSON", v4KeyPattern, v4InfoObj, ambiguousDoc);

    // --- V4 compound with ambiguous geo field ---
    BSONObj v4CompoundInfoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 4}");
    BSONObj v4CompoundKeyPattern = fromjson("{a: '2dsphere', b: 1}");

    runS2Test("V4_CompoundAmbiguous",
              v4CompoundKeyPattern,
              v4CompoundInfoObj,
              fromjson("{a: {x: 40.0, y: -70.0, type: 'Point', coordinates: [-70.0, 40.0]},"
                       " b: 'test'}"));
}

// ---------------------------------------------------------------------------
// 2dsphere Key Generator (non-golden)
// ---------------------------------------------------------------------------
// S2 cell coverings for non-point geometries (lines, polygons, etc.) are non-deterministic across
// CPU architectures due to floating-point differences in the S2 library's covering algorithm. These
// tests validate structural properties (non-empty result, key count within expected range) rather
// than exact cell IDs to avoid platform-dependent golden test failures.
TEST(KeyGeneratorGoldenTest, S2KeysCoverings) {
    auto runS2CoveringTest = [](const std::string& label,
                                const BSONObj& keyPattern,
                                const BSONObj& infoObj,
                                const BSONObj& inputDoc,
                                size_t minExpectedKeys,
                                size_t maxExpectedKeys) {
        S2IndexingParams params;
        const CollatorInterface* collator = nullptr;
        index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

        SharedBufferFragmentBuilder allocator(BufBuilder::kDefaultInitSizeBytes);
        KeyStringSet keys;
        MultikeyPaths multikeyPaths;
        index2dsphere::getS2Keys(allocator,
                                 inputDoc,
                                 keyPattern,
                                 params,
                                 &keys,
                                 &multikeyPaths,
                                 key_string::Version::kLatestVersion,
                                 SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                                 Ordering::make(BSONObj()));

        ASSERT_GTE(keys.size(), minExpectedKeys)
            << label << ": expected at least " << minExpectedKeys << " keys, got " << keys.size();
        ASSERT_LTE(keys.size(), maxExpectedKeys)
            << label << ": expected at most " << maxExpectedKeys << " keys, got " << keys.size();
    };

    // --- V3 covering tests ---
    BSONObj v3InfoObj = fromjson("{key: {a: '2dsphere'}, '2dsphereIndexVersion': 3}");
    BSONObj v3KeyPattern = fromjson("{a: '2dsphere'}");

    runS2CoveringTest("V3_LineString",
                      v3KeyPattern,
                      v3InfoObj,
                      fromjson("{a: {type: 'LineString', coordinates: [[0, 0], [1, 1]]}}"),
                      15,
                      25);

    runS2CoveringTest("V3_Polygon",
                      v3KeyPattern,
                      v3InfoObj,
                      fromjson("{a: {type: 'Polygon', coordinates: [[[0, 0], [0, 1], "
                               "[1, 1], [1, 0], [0, 0]]]}}"),
                      12,
                      22);

    runS2CoveringTest("V3_MultiLineString",
                      v3KeyPattern,
                      v3InfoObj,
                      fromjson("{a: {type: 'MultiLineString', coordinates: "
                               "[[[0, 0], [1, 1]], [[2, 2], [3, 3]]]}}"),
                      14,
                      24);

    runS2CoveringTest("V3_MultiPolygon",
                      v3KeyPattern,
                      v3InfoObj,
                      fromjson("{a: {type: 'MultiPolygon', coordinates: "
                               "[[[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]], "
                               "[[[2, 2], [2, 3], [3, 3], [3, 2], [2, 2]]]]}}"),
                      6,
                      16);

    runS2CoveringTest("V3_GeometryCollection",
                      v3KeyPattern,
                      v3InfoObj,
                      fromjson("{a: {type: 'GeometryCollection', geometries: ["
                               "{type: 'Point', coordinates: [0, 0]}, "
                               "{type: 'LineString', coordinates: [[0, 0], [1, 1]]}]}}"),
                      15,
                      25);

    runS2CoveringTest("V3_PolygonWithHole",
                      v3KeyPattern,
                      v3InfoObj,
                      fromjson("{a: {type: 'Polygon', coordinates: ["
                               "[[0, 0], [0, 10], [10, 10], [10, 0], [0, 0]], "
                               "[[2, 2], [2, 8], [8, 8], [8, 2], [2, 2]]]}}"),
                      9,
                      19);

    // --- V2 covering test ---
    BSONObj v2InfoObj = fromjson("{key: {a: '2dsphere'}, '2dsphereIndexVersion': 2}");

    runS2CoveringTest("V2_Polygon",
                      v3KeyPattern,
                      v2InfoObj,
                      fromjson("{a: {type: 'Polygon', coordinates: [[[0, 0], [0, 1], "
                               "[1, 1], [1, 0], [0, 0]]]}}"),
                      20,
                      35);
}

// ---------------------------------------------------------------------------
// Hashed Key Generator
// ---------------------------------------------------------------------------
TEST(KeyGeneratorGoldenTest, HashedKeys) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    const int kHashVersion = 0;

    auto runHashTest = [&](const std::string& label,
                           const BSONObj& keyPattern,
                           const BSONObj& inputDoc,
                           bool sparse = false) {
        SharedBufferFragmentBuilder allocator(BufBuilder::kDefaultInitSizeBytes);
        KeyStringSet keys;
        ExpressionKeysPrivate::getHashKeys(allocator,
                                           inputDoc,
                                           keyPattern,
                                           kHashVersion,
                                           sparse,
                                           nullptr,  // no collator
                                           &keys,
                                           key_string::Version::kLatestVersion,
                                           Ordering::make(BSONObj()),
                                           false);  // ignoreArraysAlongPath

        printTestCase(os, label, inputDoc, keyPattern, keys, Ordering::make(BSONObj()));
    };

    // Simple integer (no collator; existing tests only cover integer hashing with collator).
    runHashTest("SimpleInteger", fromjson("{a: 'hashed'}"), fromjson("{a: 5}"));

    // Nested object.
    runHashTest("NestedObject", fromjson("{a: 'hashed'}"), fromjson("{a: {b: 'string'}}"));

    // Null value.
    runHashTest("NullValue", fromjson("{a: 'hashed'}"), fromjson("{a: null}"));

    // Missing field.
    runHashTest("MissingField", fromjson("{a: 'hashed'}"), fromjson("{b: 1}"));

    // Compound with hashed field.
    runHashTest("CompoundHashed",
                fromjson("{a: 'hashed', b: 1, c: 1}"),
                fromjson("{a: 'test', b: 5, c: 10}"));

    // Compound with hashed field not first.
    runHashTest("CompoundHashedNotFirst",
                fromjson("{'a.c': 1, a: 'hashed'}"),
                fromjson("{a: {b: 'abc', c: 'def'}}"));

    // --- Numeric type equivalence edge cases ---
    // double 5.0 should hash to the same value as int 5 (confirmed by SimpleInteger above).
    runHashTest("DoubleFive", fromjson("{a: 'hashed'}"), BSON("a" << 5.0));

    // Negative zero vs positive zero.
    runHashTest("NegativeZero", fromjson("{a: 'hashed'}"), BSON("a" << -0.0));

    runHashTest("PositiveZero", fromjson("{a: 'hashed'}"), BSON("a" << 0.0));

    // --- Special BSON types ---
    // MinKey.
    {
        BSONObjBuilder bob;
        bob.appendMinKey("a");
        runHashTest("MinKey", fromjson("{a: 'hashed'}"), bob.obj());
    }

    // Undefined.
    {
        BSONObjBuilder bob;
        bob.appendUndefined("a");
        runHashTest("UndefinedValue", fromjson("{a: 'hashed'}"), bob.obj());
    }

    // --- Dotted path for hashed field ---
    runHashTest("DottedPathHashed", fromjson("{'a.b': 'hashed'}"), fromjson("{a: {b: 'value'}}"));

    // Dotted path with missing intermediate.
    runHashTest("DottedPathMissing1", fromjson("{'a.b': 'hashed'}"), fromjson("{a: {c: 'value'}}"));
    runHashTest("DottedPathMissing2", fromjson("{'a.b': 'hashed'}"), fromjson("{a: 1}"));
}

// ---------------------------------------------------------------------------
// Wildcard Key Generator
// ---------------------------------------------------------------------------
TEST(KeyGeneratorGoldenTest, WildcardKeys) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    auto runWildcardTest = [&](const std::string& label,
                               const BSONObj& keyPattern,
                               const BSONObj& pathProjection,
                               const BSONObj& inputDoc) {
        WildcardKeyGenerator keyGen(keyPattern,
                                    pathProjection,
                                    nullptr,  // no collator
                                    key_string::Version::kLatestVersion,
                                    Ordering::make(BSONObj()),
                                    KeyFormat::Long);

        SharedBufferFragmentBuilder allocator(BufBuilder::kDefaultInitSizeBytes);
        KeyStringSet keys;
        KeyStringSet multikeyPaths;
        keyGen.generateKeys(allocator, inputDoc, &keys, &multikeyPaths);

        printTestCase(os, label, inputDoc, keyPattern, keys, Ordering::make(BSONObj()));

        // Also print multikey metadata keys.
        auto mkKeys = keySetToBsonVector(multikeyPaths, Ordering::make(BSONObj()));
        os << "MultikeyPaths (" << mkKeys.size() << "):\n";
        for (auto& mk : mkKeys) {
            os << "  " << mk.jsonString(JsonStringFormat::LegacyStrict) << "\n";
        }
        os << "\n";
    };

    BSONObj allPaths = fromjson("{\"$**\": 1}");
    BSONObj emptyProj = BSONObj();

    // Simple document.
    runWildcardTest("SimpleDocument", allPaths, emptyProj, fromjson("{a: 1, b: 'hello', c: true}"));

    // Nested document.
    runWildcardTest("NestedDocument", allPaths, emptyProj, fromjson("{a: {b: 1, c: 2}}"));

    // Array value.
    runWildcardTest("ArrayValue", allPaths, emptyProj, fromjson("{a: [1, 2, 3]}"));

    // Empty document.
    runWildcardTest("EmptyDocument", allPaths, emptyProj, fromjson("{}"));

    // Null field.
    runWildcardTest("NullField", allPaths, emptyProj, fromjson("{a: null}"));

    // Empty object field.
    runWildcardTest("EmptyObjectField", allPaths, emptyProj, fromjson("{a: {}}"));

    // Empty array field.
    runWildcardTest("EmptyArrayField", allPaths, emptyProj, fromjson("{a: []}"));

    // Array of objects.
    runWildcardTest("ArrayOfObjects", allPaths, emptyProj, fromjson("{a: [{b: 1}, {b: 2}]}"));

    // Multiple levels of nesting.
    runWildcardTest("DeepNesting", allPaths, emptyProj, fromjson("{a: {b: {c: {d: 42}}}}"));

    // Document with _id.
    runWildcardTest("WithId", allPaths, emptyProj, fromjson("{_id: 1, a: 2, b: 3}"));

    // Single field wildcard.
    BSONObj singleFieldPattern = fromjson("{\"a.$**\": 1}");
    runWildcardTest(
        "SingleFieldWildcard", singleFieldPattern, emptyProj, fromjson("{a: {x: 1, y: 2}, b: 3}"));

    // --- Nested array edge cases ---

    // Array of arrays.
    runWildcardTest("ArrayOfArrays", allPaths, emptyProj, fromjson("{a: [[1, 2], [3, 4]]}"));

    // Deeply nested arrays.
    runWildcardTest(
        "DeeplyNestedArrays", allPaths, emptyProj, fromjson("{a: [{b: [1, 2]}, {b: [3, 4]}]}"));

    // --- Numeric field names ---
    runWildcardTest("NumericFieldNames",
                    allPaths,
                    emptyProj,
                    fromjson("{\"0\": 'zero', \"1\": 'one', \"2\": 'two'}"));

    // --- Multiple levels with arrays at different depths ---
    runWildcardTest(
        "MultiLevelArrays", allPaths, emptyProj, fromjson("{a: {b: [{c: [1, 2]}, {c: [3]}]}}"));

    // --- Single field wildcard with nested target ---
    BSONObj nestedFieldPattern = fromjson("{\"a.b.$**\": 1}");
    runWildcardTest("NestedFieldWildcard",
                    nestedFieldPattern,
                    emptyProj,
                    fromjson("{a: {b: {x: 1, y: 2}, c: 3}, d: 4}"));

    // --- Inclusion projection ---
    runWildcardTest("InclusionProjection",
                    allPaths,
                    fromjson("{a: 1, b: 1}"),
                    fromjson("{a: 1, b: 2, c: 3, d: 4}"));

    // --- Exclusion projection ---
    runWildcardTest("ExclusionProjection",
                    allPaths,
                    fromjson("{a: 0, b: 0}"),
                    fromjson("{a: 1, b: 2, c: 3, d: 4}"));

    // --- Object with only _id (wildcard skips _id) ---
    runWildcardTest("OnlyId", allPaths, emptyProj, fromjson("{_id: 'onlyid'}"));

    // --- Undefined value ---
    {
        BSONObjBuilder bob;
        bob.appendUndefined("a");
        runWildcardTest("UndefinedField", allPaths, emptyProj, bob.obj());
    }

    // --- MinKey ---
    {
        BSONObjBuilder bob;
        bob.appendMinKey("a");
        runWildcardTest("MinKeyField", allPaths, emptyProj, bob.obj());
    }
}

// ---------------------------------------------------------------------------
// Text (FTS) Key Generator
// ---------------------------------------------------------------------------

TEST(KeyGeneratorGoldenTest, TextKeys) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    auto runTextTest =
        [&](const std::string& label, const BSONObj& specObj, const BSONObj& inputDoc) {
            auto fixedSpec = unittest::assertGet(fts::FTSSpec::fixSpec(specObj));
            fts::FTSSpec spec(fixedSpec);

            SharedBufferFragmentBuilder allocator(BufBuilder::kDefaultInitSizeBytes);
            KeyStringSet keys;
            ExpressionKeysPrivate::getFTSKeys(allocator,
                                              inputDoc,
                                              spec,
                                              &keys,
                                              key_string::Version::kLatestVersion,
                                              Ordering::make(BSONObj()));

            os << "--- " << label << " ---\n";
            os << "Input: " << inputDoc.jsonString(JsonStringFormat::LegacyStrict) << "\n";
            os << "Spec: " << specObj.jsonString(JsonStringFormat::LegacyStrict) << "\n";

            auto bsonKeys = keySetToBsonVector(keys, Ordering::make(BSONObj()));
            os << "Keys (" << bsonKeys.size() << "):\n";
            for (auto& key : bsonKeys) {
                os << "  " << key.jsonString(JsonStringFormat::LegacyStrict) << "\n";
            }
            os << "\n";
        };

    // Simple single word.
    runTextTest("SimpleWord", BSON("key" << BSON("data" << "text")), BSON("data" << "cat"));

    // Multiple words.
    runTextTest("MultipleWords", BSON("key" << BSON("data" << "text")), BSON("data" << "cat sat"));

    // Compound text with trailing field.
    runTextTest("CompoundTrailing",
                BSON("key" << BSON("data" << "text" << "x" << 1)),
                BSON("data" << "cat" << "x" << 5));

    // Compound text with leading field.
    runTextTest("CompoundLeading",
                BSON("key" << BSON("x" << 1 << "data" << "text")),
                BSON("data" << "cat" << "x" << 5));

    // Multiple text fields.
    runTextTest("MultipleTextFields",
                BSON("key" << BSON("a" << "text" << "b" << "text")),
                BSON("a" << "dog" << "b" << "cat"));

    // Missing text field.
    runTextTest("MissingTextField", BSON("key" << BSON("data" << "text")), BSON("other" << "cat"));

    // Empty string.
    runTextTest("EmptyString", BSON("key" << BSON("data" << "text")), BSON("data" << ""));

    // Long text (multiple stemmed words).
    runTextTest("LongText",
                BSON("key" << BSON("data" << "text")),
                BSON("data" << "the quick brown fox jumped over the lazy dog"));

    // Weighted fields.
    runTextTest("WeightedFields",
                BSON("key" << BSON("title" << "text" << "body" << "text") << "weights"
                           << BSON("title" << 10 << "body" << 1)),
                BSON("title" << "important" << "body" << "details here"));

    // --- Wildcard text ($**) ---
    runTextTest("WildcardText",
                BSON("key" << BSON("$**" << "text")),
                BSON("a" << "hello world" << "b" << "foo bar"));

    // --- Nested text field via dotted path ---
    runTextTest("DottedPathText",
                BSON("key" << BSON("a.b" << "text")),
                BSON("a" << BSON("b" << "nested text here")));

    // --- Repeated words (stemming should collapse) ---
    runTextTest("RepeatedWords",
                BSON("key" << BSON("data" << "text")),
                BSON("data" << "run running runs runner"));

    // --- Punctuation and special characters ---
    runTextTest("PunctuationText",
                BSON("key" << BSON("data" << "text")),
                BSON("data" << "hello, world! this is... a test."));

    // --- Stop words only ---
    runTextTest(
        "StopWordsOnly", BSON("key" << BSON("data" << "text")), BSON("data" << "the and is at"));

    // --- Numbers in text ---
    runTextTest("NumbersInText",
                BSON("key" << BSON("data" << "text")),
                BSON("data" << "test123 456 abc789"));

    // --- Null text field ---
    runTextTest("NullTextField", BSON("key" << BSON("data" << "text")), BSON("data" << BSONNULL));

    // --- Multiple text fields with one missing ---
    runTextTest("MultipleTextFieldsOneMissing",
                BSON("key" << BSON("a" << "text" << "b" << "text")),
                BSON("a" << "cat dog"));

    // --- Text with array of strings (multikey) ---
    runTextTest("ArrayOfStrings",
                BSON("key" << BSON("data" << "text")),
                BSON("data" << BSON_ARRAY("hello world" << "foo bar")));
}

}  // namespace
}  // namespace mongo
