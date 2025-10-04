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


#include "mongo/db/index/wildcard_key_generator.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <initializer_list>
#include <ostream>
#include <string>

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

KeyStringSet makeKeySet(std::initializer_list<BSONObj> init = {}, RecordId id = RecordId()) {
    KeyStringSet keys;
    Ordering ordering = Ordering::make(BSONObj());
    for (const auto& key : init) {
        key_string::HeapBuilder keyString(key_string::Version::kLatestVersion, key, ordering);
        if (!id.isNull()) {
            keyString.appendRecordId(id);
        }
        keys.insert(keyString.release());
    }
    return keys;
}

std::string dumpKeyset(const KeyStringSet& keyStrings) {
    std::stringstream ss;
    ss << "[ ";
    for (auto& keyString : keyStrings) {
        auto key = key_string::toBson(keyString, Ordering::make(BSONObj()));
        ss << key.toString() << " ";
    }
    ss << "]";

    return ss.str();
}

bool assertKeysetsEqual(const KeyStringSet& expectedKeys, const KeyStringSet& actualKeys) {
    if (expectedKeys.size() != actualKeys.size()) {
        LOGV2(20695,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    if (!std::equal(expectedKeys.begin(), expectedKeys.end(), actualKeys.begin())) {
        LOGV2(20696,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    return true;
}

struct WildcardKeyGeneratorTest : public unittest::Test {
    SharedBufferFragmentBuilder allocator{key_string::HeapBuilder::kHeapAllocatorDefaultBytes};
    KeyFormat rsKeyFormat = KeyFormat::Long;
};

// Full-document tests with no projection.
struct WildcardKeyGeneratorFullDocumentTest : public WildcardKeyGeneratorTest {};

TEST_F(WildcardKeyGeneratorFullDocumentTest, ExtractTopLevelKey) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: 1}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}")});
    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorFullDocumentTest, ExtractKeysFromNestedObject) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: {b: 'one', c: 2}}");

    auto expectedKeys =
        makeKeySet({fromjson("{'': 'a.b', '': 'one'}"), fromjson("{'': 'a.c', '': 2}")});

    auto expectedMultikeyPaths = makeKeySet();

    KeyStringSet outputKeys;
    KeyStringSet multikeyMetadataKeys;
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorFullDocumentTest, ShouldIndexEmptyObject) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: 1, b: {}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"), fromjson("{'': 'b', '': {}}")});
    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorFullDocumentTest, ShouldIndexNonNestedEmptyArrayAsUndefined) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{ a: [], b: {c: []}, d: [[], {e: []}]}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': undefined}"),
                                    fromjson("{'': 'b.c', '': undefined}"),
                                    fromjson("{'': 'd', '': []}"),
                                    fromjson("{'': 'd.e', '': undefined}")});
    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"),
                    fromjson("{'': 1, '': 'b.c'}"),
                    fromjson("{'': 1, '': 'd'}"),
                    fromjson("{'': 1, '': 'd.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorFullDocumentTest, ExtractMultikeyPath) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: [1, 2, {b: 'one', c: 2}, {d: 3}]}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.d', '': 3}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorFullDocumentTest, ExtractMultikeyPathsKeyFormatString) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                KeyFormat::String};
    auto inputDoc = fromjson("{a: [1, 2, {b: 'one', c: 2}, {d: 3}]}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.d', '': 3}")});

    auto expectedMultikeyPaths = makeKeySet(
        {fromjson("{'': 1, '': 'a'}")},
        record_id_helpers::reservedIdFor(
            record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::String));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorFullDocumentTest, ExtractMultikeyPathAndDedupKeys) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {d: 3}]}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.d', '': 3}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorFullDocumentTest, ExtractZeroElementMultikeyPath) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {d: 3}], e: []}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'e', '': undefined}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorFullDocumentTest, ExtractNestedMultikeyPaths) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    // Note: the 'e' array is nested within a subdocument in the enclosing 'a' array; it will
    // generate a separate multikey entry 'a.e' and index keys for each of its elements. The raw
    // array nested directly within the 'a' array will not, because the indexing system does not
    // descend nested arrays without an intervening path component.
    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]]}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.c', '': 'two'}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': 5}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorFullDocumentTest, ExtractMixedPathTypesAndAllSubpaths) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    // Tests a mix of multikey paths, various duplicate-key scenarios, and deeply-nested paths.
    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.c', '': 'two'}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': 5}"),
                                    fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}"),
                                    fromjson("{'': 'l', '': 'string'}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"),
                    fromjson("{'': 1, '': 'a.e'}"),
                    fromjson("{'': 1, '': 'g.h.j'}"),
                    fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Single-subtree implicit projection.
struct WildcardKeyGeneratorSingleSubtreeTest : public WildcardKeyGeneratorTest {};

TEST_F(WildcardKeyGeneratorSingleSubtreeTest, ExtractSubtreeWithSinglePathComponent) {
    WildcardKeyGenerator keyGen{fromjson("{'g.$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'g.h.j'}"), fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorSingleSubtreeTest, ExtractSubtreeWithMultiplePathComponents) {
    WildcardKeyGenerator keyGen{fromjson("{'g.h.$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'g.h.j'}"), fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorSingleSubtreeTest, ExtractMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'g.h.j.$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'g.h.j'}"), fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorSingleSubtreeTest, ExtractNestedMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'a.e.$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    // We project through the 'a' array to the nested 'e' array. Both 'a' and 'a.e' are added as
    // multikey paths.
    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': {}}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': 5}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Explicit inclusion tests.
struct WildcardKeyGeneratorInclusionTest : public WildcardKeyGeneratorTest {};

TEST_F(WildcardKeyGeneratorInclusionTest, InclusionProjectionSingleSubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{g: 1}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'g.h.j'}"), fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorInclusionTest, InclusionProjectionNestedSubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'g.h': 1}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'g.h.j'}"), fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorInclusionTest, InclusionProjectionMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'g.h.j': 1}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'g.h.j'}"), fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorInclusionTest, InclusionProjectionNestedMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'a.e': 1}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': {}}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': 5}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorInclusionTest, InclusionProjectionMultipleSubtrees) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'a.b': 1, 'a.c': 1, 'a.e': 1, 'g.h.i': 1}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.c', '': 'two'}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': 5}"),
                                    fromjson("{'': 'g.h.i', '': 9}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Explicit exclusion tests.
struct WildcardKeyGeneratorExclusionTest : public WildcardKeyGeneratorTest {};

TEST_F(WildcardKeyGeneratorExclusionTest, ExclusionProjectionSingleSubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{g: 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.c', '': 'two'}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': 5}"),
                                    fromjson("{'': 'l', '': 'string'}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorExclusionTest, ExclusionProjectionNestedSubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'g.h': 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.c', '': 'two'}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': 5}"),
                                    fromjson("{'': 'g', '': {}}"),
                                    fromjson("{'': 'l', '': 'string'}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorExclusionTest, ExclusionProjectionMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'g.h.j': 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.c', '': 'two'}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': 5}"),
                                    fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}"),
                                    fromjson("{'': 'l', '': 'string'}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorExclusionTest, ExclusionProjectionNestedMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'a.e': 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.c', '': 'two'}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}"),
                                    fromjson("{'': 'g.h.k', '': 12}"),
                                    fromjson("{'': 'l', '': 'string'}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"),
                    fromjson("{'': 1, '': 'g.h.j'}"),
                    fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorExclusionTest, ExclusionProjectionMultipleSubtrees) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'a.b': 0, 'a.c': 0, 'a.e': 0, 'g.h.i': 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a', '': {}}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}"),
                                    fromjson("{'': 'l', '': 'string'}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"),
                    fromjson("{'': 1, '': 'g.h.j'}"),
                    fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Test _id inclusion and exclusion behaviour.
struct WildcardKeyGeneratorIdTest : public WildcardKeyGeneratorTest {};

TEST_F(WildcardKeyGeneratorIdTest, ExcludeIdFieldIfProjectionIsEmpty) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 1}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, ExcludeIdFieldForSingleSubtreeKeyPattern) {
    WildcardKeyGenerator keyGen{fromjson("{'a.$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 1}"),
                                    fromjson("{'': 'a.e', '': 4}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, PermitIdFieldAsSingleSubtreeKeyPattern) {
    WildcardKeyGenerator keyGen{fromjson("{'_id.$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys =
        makeKeySet({fromjson("{'': '_id.id1', '': 1}"), fromjson("{'': '_id.id2', '': 2}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, PermitIdSubfieldAsSingleSubtreeKeyPattern) {
    WildcardKeyGenerator keyGen{fromjson("{'_id.id1.$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': '_id.id1', '': 1}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, ExcludeIdFieldByDefaultForInclusionProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{a: 1}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 1}"),
                                    fromjson("{'': 'a.e', '': 4}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, PermitIdSubfieldInclusionInExplicitProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'_id.id1': 1}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': '_id.id1', '': 1}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, ExcludeIdFieldByDefaultForExclusionProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{a: 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys =
        makeKeySet({fromjson("{'': 'g.h.i', '': 9}"), fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, PermitIdSubfieldExclusionInExplicitProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'_id.id1': 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': '_id.id2', '': 2}"),
                                    fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 1}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, IncludeIdFieldIfExplicitlySpecifiedInProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{_id: 1, a: 1}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': '_id.id1', '': 1}"),
                                    fromjson("{'': '_id.id2', '': 2}"),
                                    fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 1}"),
                                    fromjson("{'': 'a.e', '': 4}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, ExcludeIdFieldIfExplicitlySpecifiedInProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{_id: 0, a: 1}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 1}"),
                                    fromjson("{'': 'a.e', '': 4}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorIdTest, IncludeIdFieldIfExplicitlySpecifiedInExclusionProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{_id: 1, a: 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': '_id.id1', '': 1}"),
                                    fromjson("{'': '_id.id2', '': 2}"),
                                    fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Collation tests.
struct WildcardKeyGeneratorCollationTest : public WildcardKeyGeneratorTest {};

TEST_F(WildcardKeyGeneratorCollationTest, CollationMixedPathAndKeyTypes) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                &collator,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    // Verify that the collation is only applied to String values, but all types are indexed.
    auto dateVal = "{'$date': 1529453450288}"_sd;
    auto oidVal = "{'$oid': '520e6431b7fa4ea22d6b1872'}"_sd;
    auto tsVal = "{'$timestamp': {'t': 1, 'i': 100}}"_sd;
    auto undefVal = "{'$undefined': true}"_sd;

    auto inputDoc =
        fromjson("{a: [1, null, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: " + dateVal +
                 ", e: [4, " + oidVal + "]}, [6, 7, {f: 8}]], g: {h: {i: " + tsVal +
                 ", j: [10, {k: 11}, {k: [" + undefVal + "]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': null}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 'eno'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.c', '': 'owt'}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'a.d', '': " + dateVal + "}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': " + oidVal + "}"),
                                    fromjson("{'': 'g.h.i', '': " + tsVal + "}"),
                                    fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': " + undefVal + "}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}"),
                                    fromjson("{'': 'l', '': 'gnirts'}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"),
                    fromjson("{'': 1, '': 'a.e'}"),
                    fromjson("{'': 1, '': 'g.h.j'}"),
                    fromjson("{'': 1, '': 'g.h.j.k'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

struct WildcardKeyGeneratorDottedFieldsTest : public WildcardKeyGeneratorTest {};

TEST_F(WildcardKeyGeneratorDottedFieldsTest, DoNotIndexDottedFields) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                {},
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson(
        "{'a.b': 0, '.b': 1, 'b.': 2, a: {'.b': 3, 'b.': 4, 'b.c': 5, 'q': 6}, b: [{'d.e': 7}, {r: "
        "8}, [{'a.b': 9}]], c: 10}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a.q', '': 6}"),
                                    fromjson("{'': 'b.r', '': 8}"),
                                    fromjson("{'': 'b', '': [{'a.b': 9}]}"),
                                    fromjson("{'': 'c', '': 10}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'b'}")},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorDottedFieldsTest, DoNotIndexDottedFieldsWithSimilarSubpathInKey) {
    WildcardKeyGenerator keyGen{fromjson("{'a.b.$**': 1}"),
                                {},
                                {},
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};

    auto inputDoc = fromjson("{'a.b': 0}");

    auto expectedKeys = makeKeySet();

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

//
// Following unit tests are for compound wildcard indexes.
//
struct WildcardKeyGeneratorCompoundTest : public WildcardKeyGeneratorTest {};

TEST_F(WildcardKeyGeneratorCompoundTest, ExtractTopLevelKeyCompound) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1, a: 1}"),
                                fromjson("{a: 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: 1, b: 1}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'b', '': 1, '': 1}")});
    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorCompoundTest, ExtractKeysFromNestedObjectCompound) {
    WildcardKeyGenerator keyGen{fromjson("{c: 1, '$**': 1}"),
                                fromjson("{c: 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: {b: 'one', c: 2}}");

    auto expectedKeys = makeKeySet(
        {fromjson("{'': null, '': 'a.b', '': 'one'}"), fromjson("{'': null, '': 'a.c', '': 2}")});

    auto expectedMultikeyPaths = makeKeySet();

    KeyStringSet outputKeys;
    KeyStringSet multikeyMetadataKeys;
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorCompoundTest, MiddleWildcardComponentCompound) {
    WildcardKeyGenerator keyGen{fromjson("{a: 1, '$**': 1, c: 1}"),
                                fromjson("{a: 0, c: 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: 1, b: 2}");

    auto expectedKeys = makeKeySet({fromjson("{'': 1, '': 'b', '': 2, '': null}")});

    auto expectedMultikeyPaths = makeKeySet();

    KeyStringSet outputKeys;
    KeyStringSet multikeyMetadataKeys;
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorCompoundTest, IndexSubTreeCompound) {
    WildcardKeyGenerator keyGen{fromjson("{a: 1, 'sub.$**': 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: 1, sub: {a: 1, b: 2}}");

    auto expectedKeys = makeKeySet(
        {fromjson("{'': 1, '': 'sub.a', '': 1}"), fromjson("{'': 1, '': 'sub.b', '': 2}")});

    auto expectedMultikeyPaths = makeKeySet();

    KeyStringSet outputKeys;
    KeyStringSet multikeyMetadataKeys;
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorCompoundTest, CompoundWildcardIndexShouldBeSparse) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1, c: 1}"),
                                fromjson("{c: 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{}");

    // No key is generated because wildcard indexes are always sparse.
    auto expectedKeys = makeKeySet();
    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorCompoundTest, CanGenerateKeysForMultikeyFieldCompound) {
    WildcardKeyGenerator keyGen{fromjson("{a: 1, '$**': 1, c: 1}"),
                                fromjson("{a: 0, c: 0}"),
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{a: 1, b: [1, {c: [3]}]}");

    auto expectedKeys = makeKeySet({fromjson("{'': 1, '': 'b', '': 1, '': null}"),
                                    fromjson("{'': 1, '': 'b.c', '': 3, '': null}")});
    auto expectedMultikeyPaths =
        makeKeySet({BSON("" << MINKEY << "" << 1 << ""
                            << "b"
                            << "" << MINKEY),
                    BSON("" << MINKEY << "" << 1 << ""
                            << "b.c"
                            << "" << MINKEY)},
                   record_id_helpers::reservedIdFor(
                       record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, rsKeyFormat));

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST_F(WildcardKeyGeneratorCompoundTest, CannotCompoundWithMultikeyField) {
    WildcardKeyGenerator keyGen{fromjson("{'sub.$**': 1, arr: 1}"),
                                {},
                                nullptr,
                                key_string::Version::kLatestVersion,
                                Ordering::make(BSONObj()),
                                rsKeyFormat};
    auto inputDoc = fromjson("{sub: {a: 1}, arr: [1, 2]}");

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    ASSERT_THROWS_CODE(keyGen.generateKeys(allocator, inputDoc, &outputKeys, &multikeyMetadataKeys),
                       DBException,
                       7246301);
}

}  // namespace
}  // namespace mongo
