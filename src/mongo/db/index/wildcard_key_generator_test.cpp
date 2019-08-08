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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

KeyStringSet makeKeySet(std::initializer_list<BSONObj> init = {}, RecordId id = RecordId()) {
    KeyStringSet keys;
    Ordering ordering = Ordering::make(BSONObj());
    for (const auto& key : init) {
        KeyString::HeapBuilder keyString(KeyString::Version::kLatestVersion, key, ordering);
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
        auto key = KeyString::toBson(keyString, Ordering::make(BSONObj()));
        ss << key.toString() << " ";
    }
    ss << "]";

    return ss.str();
}

bool assertKeysetsEqual(const KeyStringSet& expectedKeys, const KeyStringSet& actualKeys) {
    if (expectedKeys.size() != actualKeys.size()) {
        log() << "Expected: " << dumpKeyset(expectedKeys) << ", "
              << "Actual: " << dumpKeyset(actualKeys);
        return false;
    }

    if (!std::equal(expectedKeys.begin(), expectedKeys.end(), actualKeys.begin())) {
        log() << "Expected: " << dumpKeyset(expectedKeys) << ", "
              << "Actual: " << dumpKeyset(actualKeys);
        return false;
    }

    return true;
}

// Full-document tests with no projection.

TEST(WildcardKeyGeneratorFullDocumentTest, ExtractTopLevelKey) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};
    auto inputDoc = fromjson("{a: 1}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}")});
    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorFullDocumentTest, ExtractKeysFromNestedObject) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};
    auto inputDoc = fromjson("{a: {b: 'one', c: 2}}");

    auto expectedKeys =
        makeKeySet({fromjson("{'': 'a.b', '': 'one'}"), fromjson("{'': 'a.c', '': 2}")});

    auto expectedMultikeyPaths = makeKeySet();

    KeyStringSet outputKeys;
    KeyStringSet multikeyMetadataKeys;
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorFullDocumentTest, ShouldIndexEmptyObject) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};
    auto inputDoc = fromjson("{a: 1, b: {}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"), fromjson("{'': 'b', '': {}}")});
    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorFullDocumentTest, ShouldIndexNonNestedEmptyArrayAsUndefined) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};
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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorFullDocumentTest, ExtractMultikeyPath) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};
    auto inputDoc = fromjson("{a: [1, 2, {b: 'one', c: 2}, {d: 3}]}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.d', '': 3}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorFullDocumentTest, ExtractMultikeyPathAndDedupKeys) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};
    auto inputDoc = fromjson("{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {d: 3}]}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.d', '': 3}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorFullDocumentTest, ExtractZeroElementMultikeyPath) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};
    auto inputDoc = fromjson("{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {d: 3}], e: []}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': 2}"),
                                    fromjson("{'': 'a.b', '': 'one'}"),
                                    fromjson("{'': 'a.c', '': 2}"),
                                    fromjson("{'': 'a.d', '': 3}"),
                                    fromjson("{'': 'e', '': undefined}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'e'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorFullDocumentTest, ExtractNestedMultikeyPaths) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorFullDocumentTest, ExtractMixedPathTypesAndAllSubpaths) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Single-subtree implicit projection.

TEST(WildcardKeyGeneratorSingleSubtreeTest, ExtractSubtreeWithSinglePathComponent) {
    WildcardKeyGenerator keyGen{fromjson("{'g.$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorSingleSubtreeTest, ExtractSubtreeWithMultiplePathComponents) {
    WildcardKeyGenerator keyGen{fromjson("{'g.h.$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorSingleSubtreeTest, ExtractMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'g.h.j.$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'g.h.j'}"), fromjson("{'': 1, '': 'g.h.j.k'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorSingleSubtreeTest, ExtractNestedMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'a.e.$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Explicit inclusion tests.

TEST(WildcardKeyGeneratorInclusionTest, InclusionProjectionSingleSubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{g: 1}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorInclusionTest, InclusionProjectionNestedSubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'g.h': 1}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorInclusionTest, InclusionProjectionMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'g.h.j': 1}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'g.h.j', '': 10}"),
                                    fromjson("{'': 'g.h.j.k', '': 11}"),
                                    fromjson("{'': 'g.h.j.k', '': 11.5}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'g.h.j'}"), fromjson("{'': 1, '': 'g.h.j.k'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorInclusionTest, InclusionProjectionNestedMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'a.e': 1}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{a: [1, 2, {b: 'one', c: 2}, {c: 2, d: 3}, {c: 'two', d: 3, e: [4, 5]}, [6, 7, {f: 8}]], "
        "g: {h: {i: 9, j: [10, {k: 11}, {k: [11.5]}], k: 12.0}}, l: 'string'}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': {}}"),
                                    fromjson("{'': 'a.e', '': 4}"),
                                    fromjson("{'': 'a.e', '': 5}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorInclusionTest, InclusionProjectionMultipleSubtrees) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'a.b': 1, 'a.c': 1, 'a.e': 1, 'g.h.i': 1}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Explicit exclusion tests.

TEST(WildcardKeyGeneratorExclusionTest, ExclusionProjectionSingleSubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{g: 0}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorExclusionTest, ExclusionProjectionNestedSubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'g.h': 0}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorExclusionTest, ExclusionProjectionMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'g.h.j': 0}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorExclusionTest, ExclusionProjectionNestedMultikeySubtree) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'a.e': 0}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorExclusionTest, ExclusionProjectionMultipleSubtrees) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'a.b': 0, 'a.c': 0, 'a.e': 0, 'g.h.i': 0}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Test _id inclusion and exclusion behaviour.

TEST(WildcardKeyGeneratorIdTest, ExcludeIdFieldIfProjectionIsEmpty) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, ExcludeIdFieldForSingleSubtreeKeyPattern) {
    WildcardKeyGenerator keyGen{fromjson("{'a.$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 1}"),
                                    fromjson("{'': 'a.e', '': 4}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, PermitIdFieldAsSingleSubtreeKeyPattern) {
    WildcardKeyGenerator keyGen{fromjson("{'_id.$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys =
        makeKeySet({fromjson("{'': '_id.id1', '': 1}"), fromjson("{'': '_id.id2', '': 2}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, PermitIdSubfieldAsSingleSubtreeKeyPattern) {
    WildcardKeyGenerator keyGen{fromjson("{'_id.id1.$**': 1}"),
                                {},
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': '_id.id1', '': 1}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, ExcludeIdFieldByDefaultForInclusionProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{a: 1}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 1}"),
                                    fromjson("{'': 'a.e', '': 4}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, PermitIdSubfieldInclusionInExplicitProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'_id.id1': 1}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': '_id.id1', '': 1}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, ExcludeIdFieldByDefaultForExclusionProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{a: 0}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys =
        makeKeySet({fromjson("{'': 'g.h.i', '': 9}"), fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, PermitIdSubfieldExclusionInExplicitProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{'_id.id1': 0}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, IncludeIdFieldIfExplicitlySpecifiedInProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{_id: 1, a: 1}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, ExcludeIdFieldIfExplicitlySpecifiedInProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{_id: 0, a: 1}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a', '': 1}"),
                                    fromjson("{'': 'a', '': [6, 7, {f: 8}]}"),
                                    fromjson("{'': 'a.b', '': 1}"),
                                    fromjson("{'': 'a.e', '': 4}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'a'}"), fromjson("{'': 1, '': 'a.e'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorIdTest, IncludeIdFieldIfExplicitlySpecifiedInExclusionProjection) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                fromjson("{_id: 1, a: 0}"),
                                nullptr,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{_id: {id1: 1, id2: 2}, a: [1, {b: 1, e: [4]}, [6, 7, {f: 8}]], g: {h: {i: 9, k: 12.0}}}");

    auto expectedKeys = makeKeySet({fromjson("{'': '_id.id1', '': 1}"),
                                    fromjson("{'': '_id.id2', '': 2}"),
                                    fromjson("{'': 'g.h.i', '': 9}"),
                                    fromjson("{'': 'g.h.k', '': 12.0}")});

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

// Collation tests.

TEST(WildcardKeyGeneratorCollationTest, CollationMixedPathAndKeyTypes) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                &collator,
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

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
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorDottedFieldsTest, DoNotIndexDottedFields) {
    WildcardKeyGenerator keyGen{fromjson("{'$**': 1}"),
                                {},
                                {},
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson(
        "{'a.b': 0, '.b': 1, 'b.': 2, a: {'.b': 3, 'b.': 4, 'b.c': 5, 'q': 6}, b: [{'d.e': 7}, {r: "
        "8}, [{'a.b': 9}]], c: 10}}");

    auto expectedKeys = makeKeySet({fromjson("{'': 'a.q', '': 6}"),
                                    fromjson("{'': 'b.r', '': 8}"),
                                    fromjson("{'': 'b', '': [{'a.b': 9}]}"),
                                    fromjson("{'': 'c', '': 10}")});

    auto expectedMultikeyPaths =
        makeKeySet({fromjson("{'': 1, '': 'b'}")},
                   RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId});

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

TEST(WildcardKeyGeneratorDottedFieldsTest, DoNotIndexDottedFieldsWithSimilarSubpathInKey) {
    WildcardKeyGenerator keyGen{fromjson("{'a.b.$**': 1}"),
                                {},
                                {},
                                KeyString::Version::kLatestVersion,
                                Ordering::make(BSONObj())};

    auto inputDoc = fromjson("{'a.b': 0}");

    auto expectedKeys = makeKeySet();

    auto expectedMultikeyPaths = makeKeySet();

    auto outputKeys = makeKeySet();
    auto multikeyMetadataKeys = makeKeySet();
    keyGen.generateKeys(inputDoc, &outputKeys, &multikeyMetadataKeys);

    ASSERT(assertKeysetsEqual(expectedKeys, outputKeys));
    ASSERT(assertKeysetsEqual(expectedMultikeyPaths, multikeyMetadataKeys));
}

}  // namespace
}  // namespace mongo
