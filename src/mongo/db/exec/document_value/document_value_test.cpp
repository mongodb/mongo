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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/document_internal.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/time_support.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

Document::FieldPair getNthField(Document doc, size_t index) {
    FieldIterator it(doc);
    while (index--)  // advance index times
        it.next();
    return it.next();
}

namespace Document {

using mongo::Document;

BSONObj toBson(const Document& document) {
    return document.toBson();
}

Document fromBson(BSONObj obj) {
    return Document(obj);
}

void assertRoundTrips(const Document& document1) {
    BSONObj obj1 = toBson(document1);
    Document document2 = fromBson(obj1);
    BSONObj obj2 = toBson(document2);
    ASSERT_BSONOBJ_EQ(obj1, obj2);
    ASSERT_DOCUMENT_EQ(document1, document2);
}

TEST(DocumentConstruction, Default) {
    Document document;
    ASSERT_EQUALS(0ULL, document.computeSize());
    assertRoundTrips(document);
}

TEST(DocumentConstruction, FromEmptyBson) {
    Document document = fromBson(BSONObj());
    ASSERT_EQUALS(0ULL, document.computeSize());
    assertRoundTrips(document);
}

TEST(DocumentConstruction, FromNonEmptyBson) {
    Document document = fromBson(BSON("a" << 1 << "b"
                                          << "q"));
    ASSERT_EQUALS(2ULL, document.computeSize());
    ASSERT_EQUALS("a", getNthField(document, 0).first);
    ASSERT_EQUALS(1, getNthField(document, 0).second.getInt());
    ASSERT_EQUALS("b", getNthField(document, 1).first);
    ASSERT_EQUALS("q", getNthField(document, 1).second.getString());
}

TEST(DocumentConstruction, FromInitializerList) {
    auto document = Document{{"a", 1}, {"b", "q"_sd}};
    ASSERT_EQUALS(2ULL, document.computeSize());
    ASSERT_EQUALS("a", getNthField(document, 0).first);
    ASSERT_EQUALS(1, getNthField(document, 0).second.getInt());
    ASSERT_EQUALS("b", getNthField(document, 1).first);
    ASSERT_EQUALS("q", getNthField(document, 1).second.getString());
}

TEST(DocumentConstruction, FromEmptyDocumentClone) {
    Document document;
    ASSERT_EQUALS(0ULL, document.computeSize());
    // Prior to SERVER-26462, cloning an empty document would cause a segmentation fault.
    Document documentClone = document.clone();
    ASSERT_DOCUMENT_EQ(document, documentClone);

    // Prior to SERVER-39209 this would make ASAN complain.
    Document documentClone2 = documentClone.clone();
    ASSERT_DOCUMENT_EQ(document, documentClone2);

    // For good measure, try a third clone
    Document documentClone3 = documentClone2.clone();
    ASSERT_DOCUMENT_EQ(document, documentClone3);
}

TEST(DocumentConstruction, FromBsonReset) {
    auto document = Document{{"a", 1}, {"b", "q"_sd}};
    auto bson = toBson(document);

    MutableDocument md;
    md.reset(bson, false);
    auto newDocument = md.freeze();

    ASSERT_BSONOBJ_EQ(bson, toBson(newDocument));
}

/**
 * Appends to 'builder' an object nested 'depth' levels deep.
 */
void appendNestedObject(size_t depth, BSONObjBuilder* builder) {
    if (depth == 1U) {
        builder->append("a", 1);
    } else {
        BSONObjBuilder subobj(builder->subobjStart("a"));
        appendNestedObject(depth - 1, &subobj);
        subobj.doneFast();
    }
}

TEST(DocumentSerialization, CanSerializeDocumentExactlyAtDepthLimit) {
    BSONObjBuilder builder;
    appendNestedObject(BSONDepth::getMaxAllowableDepth(), &builder);
    BSONObj originalBSONObj = builder.obj();

    Document doc(originalBSONObj);
    BSONObjBuilder serializationResult;
    doc.toBson(&serializationResult);
    ASSERT_BSONOBJ_EQ(originalBSONObj, serializationResult.obj());
}

TEST(DocumentSerialization, CannotSerializeDocumentThatExceedsDepthLimit) {
    MutableDocument md;
    md.addField("a", Value(1));
    Document doc(md.freeze());
    for (size_t idx = 0; idx < BSONDepth::getMaxAllowableDepth(); ++idx) {
        MutableDocument md;
        md.addField("nested", Value(doc));
        doc = md.freeze();
    }

    BSONObjBuilder throwaway;
    ASSERT_THROWS_CODE(doc.toBson(&throwaway), AssertionException, ErrorCodes::Overflow);
    throwaway.abandon();
}

TEST(DocumentDepthCalculations, Sanity) {
    {
        // A scalar has depth 0.
        ASSERT_EQ(0, Value(1).depth(BSONDepth::getMaxAllowableDepth()));
    }
    {
        // Nesting documents increments depth.
        int32_t initialDepth = 1;
        MutableDocument md;
        md.addField("a", Value(1));
        Document doc(md.freeze());
        Value val(doc);
        int32_t iters = 16;
        ASSERT_EQ(initialDepth, val.depth(BSONDepth::getMaxAllowableDepth()));
        for (int32_t idx = 0; idx < iters; ++idx) {
            MutableDocument md;
            md.addField("a", Value(doc));
            doc = md.freeze();
            Value val(doc);
            ASSERT_EQ(idx + initialDepth + 1, val.depth(BSONDepth::getMaxAllowableDepth()));
        }
    }
    {
        // Simple document with no nested paths has depth 1.
        Value val(BSON("a" << 1));
        ASSERT_EQ(1, val.depth(BSONDepth::getMaxAllowableDepth()));
    }
    {
        // Depth is max of children.
        BSONObj bson = BSON("a" << 1 << "b" << BSON("c" << 1));
        Document document = fromBson(bson);
        Value val(document);
        ASSERT_EQ(2, val.depth(BSONDepth::getMaxAllowableDepth()));
    }
    {
        // Arrays increment depth.
        BSONObj bson = BSON("a" << BSON_ARRAY(1 << 1));
        Value val(fromBson(bson));
        ASSERT_EQ(2, val.depth(BSONDepth::getMaxAllowableDepth()));
    }
    {
        // Array length does not affect depth.
        BSONObj bson = BSON("a" << BSON_ARRAY(1 << 1));
        BSONObj bson2 = BSON("a" << BSON_ARRAY(1 << 1 << 1));
        Value val(fromBson(bson));
        Value val2(fromBson(bson2));
        ASSERT_EQ(val.depth(BSONDepth::getMaxAllowableDepth()),
                  val2.depth(BSONDepth::getMaxAllowableDepth()));
    }
    {
        // Nested arrays increment depth.
        BSONObj bson = BSON("a" << BSON_ARRAY(1 << BSON_ARRAY(1 << 1)));
        Value val(fromBson(bson));
        ASSERT_EQ(3, val.depth(BSONDepth::getMaxAllowableDepth()));
    }
    {
        // If maxDepth at least document depth, this function returns -1.
        BSONObj bson = BSON("a" << 1 << "b" << BSON("c" << 1));
        Document document = fromBson(bson);
        Value val(document);
        int32_t depth = 2;
        for (int32_t maxDepth = 0; maxDepth < 2 * depth; maxDepth++) {
            if (maxDepth <= depth) {
                ASSERT_EQ(-1, val.depth(maxDepth));
            } else {
                ASSERT_EQ(depth, val.depth(maxDepth));
            }
        }
    }
}

TEST(DocumentGetFieldNonCaching, UncachedTopLevelFields) {
    BSONObj bson = BSON("scalar" << 1 << "scalar2" << true);
    Document document = fromBson(bson);

    // Should be able to get all top level fields without caching.
    const auto* storage = static_cast<const DocumentStorage*>(document.getPtr());
    for (auto&& elt : bson) {
        auto value = document.getNestedScalarFieldNonCaching(elt.fieldNameStringData());
        ASSERT_EQ(Value::compare(*value, Value(elt), nullptr), 0);

        // Verify that the cache does not contain the field.
        auto pos = storage->findFieldInCache(elt.fieldNameStringData());
        ASSERT_FALSE(pos.found());
    }

    // Try to get a top level field which doesn't exist.
    auto nonExistentField = document.getNestedScalarFieldNonCaching("doesnotexist");
    ASSERT_TRUE(nonExistentField->missing());

    assertRoundTrips(document);
}

TEST(DocumentGetFieldNonCaching, CachedTopLevelFields) {
    BSONObj bson = BSON("scalar" << 1 << "array" << BSON_ARRAY(1 << 2 << 3) << "scalar2" << true);
    Document document = fromBson(bson);

    // Force 'scalar2' to be cached.
    ASSERT_FALSE(document["scalar2"].missing());

    // Attempt to access scalar2 with the non caching accessor. It should be cached already.
    {
        auto value = document.getNestedScalarFieldNonCaching("scalar2");
        ASSERT_EQ(value->getBool(), true);
    }
}

TEST(DocumentGetFieldNonCaching, ModifiedTopLevelFields) {
    // Update "val1" and "val2.val2" to be scalar and in document cache.
    MutableDocument mutDocument(fromBson(BSON("val1" << BSON("val1" << BSONObj()))));
    mutDocument.setField("val1", Value(true));
    MutableDocument mutSubDocument(fromBson(BSON("val2" << BSON("val2" << BSONObj()))));
    mutSubDocument.setField("val2", Value(true));
    mutDocument.addField("val2", mutSubDocument.freezeToValue());
    Document document = mutDocument.freeze();

    // Attempt to access the top-level and nested fields that reside in the cache with the non
    // caching accessor.
    {
        ASSERT_EQ(document.getNestedScalarFieldNonCaching("val1")->getBool(), true);
        ASSERT_EQ(document.getNestedScalarFieldNonCaching("val1.val1")->getType(), BSONType::eoo);

        ASSERT_EQ(document.getNestedScalarFieldNonCaching("val2.val2")->getBool(), true);
        ASSERT_EQ(document.getNestedScalarFieldNonCaching("val2.val2.val2")->getType(),
                  BSONType::eoo);
    }
}

TEST(DocumentGetFieldNonCaching, NonArrayDottedPaths) {
    BSONObj bson = BSON("address" << BSON("zip" << 123 << "street"
                                                << "foo"));
    Document document = fromBson(bson);

    auto isFieldCached = [&](StringData field) {
        const DocumentStorage* storage = static_cast<const DocumentStorage*>(document.getPtr());
        auto pos = storage->findFieldInCache(field);
        return pos.found();
    };

    // With no fields cached.
    {
        // Get a nested field without caching.
        auto zip = document.getNestedScalarFieldNonCaching("address.zip");
        ASSERT_EQ(zip->getInt(), 123);

        // Check that it was not cached.
        auto zipAfterAccess = document.getNestedScalarFieldNonCaching("address.zip");
        ASSERT_FALSE(isFieldCached("address.zip"));

        // Check that the top level field isn't cached after accessing one of its children.
        auto address = document.getNestedScalarFieldNonCaching("address");
        ASSERT_FALSE(isFieldCached("address"));

        // Get a dotted field which does not exist.
        auto nonExistent = document.getNestedScalarFieldNonCaching("address.doesnotexist");
        ASSERT_TRUE(nonExistent->missing());

        // Check that the top level field isn't cached after a failed attempt to access one of its
        // children.
        address = document.getNestedScalarFieldNonCaching("address");
        ASSERT_FALSE(isFieldCached("address"));

        // Get a dotted field which extends past a scalar.
        auto pathPastScalar = document.getNestedScalarFieldNonCaching("address.zip.doesnotexist");
        ASSERT_TRUE(pathPastScalar->missing());
    }

    // Now force 'address.street' to be cached.
    ASSERT_FALSE(document.getNestedField("address.street").missing());

    // Check that we get the right value when accessing it with the non caching accessor.
    {
        // The top level field should be cached.
        auto topLevel = document.getNestedScalarFieldNonCaching("address");
        ASSERT_TRUE(topLevel.has_value());

        auto street = document.getNestedScalarFieldNonCaching("address.street");
        ASSERT_EQ(street->getString(), "foo");
    }

    // Check that the other subfield, 'zip' is not cached.
    {
        auto zip = document.getNestedScalarFieldNonCaching("address.zip");
        ASSERT_FALSE(isFieldCached("address.zip"));
    }

    // Check that attempting to get a subfield of 'address.street' returns an empty Value.
    {
        auto nonExistent = document.getNestedScalarFieldNonCaching("address.street.doesnotexist");
        ASSERT_TRUE(nonExistent->missing());
    }
}

TEST(DocumentGetFieldNonCaching, NonArrayLongDottedPath) {
    BSONObj bson = BSON("a" << BSON("b" << BSON("c" << BSON("d" << BSON("e" << 1)))));
    Document document = fromBson(bson);

    // Not cached.
    {
        auto value = document.getNestedScalarFieldNonCaching("a.b.c.d.e");
        ASSERT_EQ(value->getInt(), 1);
    }

    // Force part of the path to get cached.
    ASSERT_FALSE(document.getNestedField("a.b.c").missing());

    // The function should be able to traverse a path which is part Value and part BSONElement.
    {
        auto value = document.getNestedScalarFieldNonCaching("a.b.c.d.e");
        ASSERT_EQ(value->getInt(), 1);
    }

    // Force the entire path to be cached.
    ASSERT_FALSE(document.getNestedField("a.b.c.d.e").missing());

    {
        auto value = document.getNestedScalarFieldNonCaching("a.b.c.d.e");
        ASSERT_EQ(value->getInt(), 1);
    }
}

TEST(DocumentGetFieldNonCaching, TraverseArray) {
    BSONObj bson =
        BSON("topLevelArray" << BSON_ARRAY(BSON("foo" << 1) << BSON("foo" << 2) << BSON("foo" << 3))
                             << "subObj" << BSON("subArray" << BSON_ARRAY(1 << 2)));
    Document document = fromBson(bson);

    auto checkBoostNoneIsReturned = [&document]() {
        auto topLevelArray = document.getNestedScalarFieldNonCaching("topLevelArray.foo");
        ASSERT_EQ(topLevelArray, boost::none);

        // Attempting to traverse an uncached nested array results in boost::none being
        // returned.
        auto subArray = document.getNestedScalarFieldNonCaching("subObj.subArray.foobar");
        ASSERT_EQ(subArray, boost::none);

        // Landing on either array field should return boost::none.
        topLevelArray = document.getNestedScalarFieldNonCaching("topLevelArray");
        ASSERT_EQ(topLevelArray, boost::none);

        subArray = document.getNestedScalarFieldNonCaching("subObj.subArray");
        ASSERT_EQ(subArray, boost::none);
    };

    // Check with no fields cached.
    checkBoostNoneIsReturned();

    // Force the top level array to be cached.
    ASSERT_FALSE(document["topLevelArray"].missing());

    // Check with one field cached.
    checkBoostNoneIsReturned();

    // Force the array that's in a sub object to be cached.
    ASSERT_FALSE(document.getNestedField("subObj.subArray").missing());

    // Check it works with both fields (and the full sub object) cached.
    checkBoostNoneIsReturned();
}

TEST(DocumentSize, ApproximateSizeIsSnapshotted) {
    const auto rawBson = BSON("field" << "value");
    const Document document{rawBson};
    const auto noCacheSize = document.getApproximateSize();

    // Force the cache construction, making the total size of the 'Document' bigger.
    // 'getApproximateSize()' must still return the same value.
    document["field"];
    const auto fullCacheSizeSnapshot = document.getApproximateSize();
    const auto fullCacheSizeCurrent = document.getCurrentApproximateSize();
    ASSERT_EQ(noCacheSize, fullCacheSizeSnapshot);
    ASSERT_LT(noCacheSize, fullCacheSizeCurrent);
}

TEST(DocumentSize, ApproximateSizeDuringBuildIsUpdated) {
    MutableDocument builder;
    builder.addField("a1", Value(1));
    builder.addField("a2", mongo::Value(2));
    builder.addField("a3", mongo::Value(3));
    auto middleBuildSize = builder.getApproximateSize();

    builder.addField("a4", Value(4));
    builder.addField("a5", mongo::Value(5));
    builder.addField("a6", mongo::Value(6));
    auto peekSize = builder.peek().getApproximateSize();

    builder.addField("a7", Value(7));
    builder.addField("a8", mongo::Value(8));
    builder.addField("a9", mongo::Value(9));
    auto beforeFreezeSize = builder.getApproximateSize();

    Document result = builder.freeze();
    auto frozenSize = result.getApproximateSize();

    ASSERT_LT(middleBuildSize, peekSize);
    ASSERT_LT(peekSize, beforeFreezeSize);
    ASSERT_EQ(beforeFreezeSize, frozenSize);
}

TEST(ShredDocument, OutputHasNoBackingBSON) {
    BSONObj bson =
        BSON("a" << 1 << "subObj" << BSON("a" << 1) << "subArray" << BSON_ARRAY(BSON("a" << 1)));
    auto original = fromBson(bson);
    auto originalSize = original.getApproximateSize();

    auto shredded = original.shred();
    auto originalSizeAfterShredding = original.getApproximateSize();
    // Fields in the original doc shouldn't be cached since it was raw bson
    ASSERT_EQ(originalSize, originalSizeAfterShredding);

    // BSON is more compact than ValueElement
    auto shreddedSize = shredded.getApproximateSize();
    ASSERT_LT(originalSize, shreddedSize);

    // Accessing a field shouldn't change the size since all fields are already cached.
    shredded["a"];
    ASSERT_EQ(shredded.getCurrentApproximateSize(), shreddedSize);
}

TEST(ShredDocument, HandlesModifiedDocuments) {
    BSONObj bson = BSON("a" << 1 << "subObj" << BSON("a" << 1));
    Document original = fromBson(bson);
    MutableDocument md(original);
    md["b"] = Value(2);
    md["subObj"]["b"] = Value(2);
    Document shredded = md.freeze().shred();

    ASSERT(!shredded["b"].missing());
    ASSERT(!shredded["subObj"]["b"].missing());
}

TEST(ShredDocument, HandlesMetadata) {
    BSONObj bson = BSON("a" << 1 << "subObj" << BSON("a" << 1));
    Document original = fromBson(bson);
    MutableDocument md(original);
    DocumentMetadataFields meta;
    meta.setSearchScore(6);
    md.setMetadata(std::move(meta));
    Document shredded = md.freeze().shred();
    ASSERT_EQ(6, shredded.metadata().getSearchScore());
}

TEST(DocumentMerge, Sanity) {
    /**
     * doc1
     * { "a": 1, "b": 2.2, "c": null }
     *
     * doc2
     * { "a": 42, "d": false }
     *
     * result
     * {  "a": 42, "b": 2.2, "c": null, "d": false }
     */
    Document doc1 = fromBson(BSON("a" << 1 << "b" << 2.2 << "c" << BSONNULL));
    Document doc2 = fromBson(BSON("a" << 42 << "d" << false));

    auto mergedDoc = Document::deepMerge(doc1, doc2);
    // Value in 'doc2' prevails over value in 'doc1'.
    ASSERT_EQUALS(42, mergedDoc["a"].getInt());
    ASSERT_EQUALS(2.2, mergedDoc["b"].getDouble());
    ASSERT_EQUALS(BSONType::null, mergedDoc["c"].getType());
    ASSERT_EQUALS(false, mergedDoc["d"].getBool());
}

TEST(DocumentMerge, ArraysValueInRightDocumentPrevails) {
    /**
     * doc1
     * {
     *      "key": [ { "a": 1 }, { "b": 2 } ]
     * }
     *
     * doc2
     * {
     *      "key": [ { "c": 3 }, { "d": 4 } ]
     * }
     *
     * result
     * {
     *      "key": [ { "c": 3 }, { "c": 4 } ]
     * }
     */
    Document doc1 = fromBson(BSON("key" << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 2))));
    Document doc2 = fromBson(BSON("key" << BSON_ARRAY(BSON("c" << 3) << BSON("d" << 4))));

    auto mergedDoc = Document::deepMerge(doc1, doc2);
    auto arr = mergedDoc["key"].getArray();
    ASSERT_EQ(3, arr[0]["c"].getInt());
    ASSERT_EQ(4, arr[1]["d"].getInt());
}

TEST(DocumentMerge, SubDocumentsAreMerged) {
    /**
     * doc1
     * {
     *      "key": { "a": 1, "b": 2 }
     * }
     *
     * doc2
     * {
     *      "key": { "c": 3, "d": 4 }
     * }
     *
     * result
     * {
     *      "key": { "a": 1, "b": 2, "c": 3 , "d": 4 }
     * }
     */
    Document doc1 = fromBson(BSON("key" << BSON("a" << 1 << "b" << 2)));
    Document doc2 = fromBson(BSON("key" << BSON("c" << 3 << "d" << 4)));

    auto mergedDoc = Document::deepMerge(doc1, doc2);
    ASSERT_EQ(1, mergedDoc["key"]["a"].getInt());
    ASSERT_EQ(2, mergedDoc["key"]["b"].getInt());
    ASSERT_EQ(3, mergedDoc["key"]["c"].getInt());
    ASSERT_EQ(4, mergedDoc["key"]["d"].getInt());
}

TEST(DocumentMerge, SubDocumentsAreMergedRecursively) {
    /**
     * doc1
     * {
     *      "key": { "a": { "b": 1 }, "c": { "d": 2 } }
     * }
     *
     * doc2
     * {
     *      "key": { "a": { "e": 3 }, "f": { "g": 4 } }
     * }
     *
     * result
     * {
     *      "key": { "a": { "b": 1, "e": 3 }, "c": { "d": 2 }, "f": { "g": 4 } }
     * }
     */
    Document doc1 = fromBson(BSON("key" << BSON("a" << BSON("b" << 1) << "c" << BSON("d" << 2))));
    Document doc2 = fromBson(BSON("key" << BSON("a" << BSON("e" << 3) << "f" << BSON("g" << 4))));

    auto mergedDoc = Document::deepMerge(doc1, doc2);
    ASSERT_EQ(1, mergedDoc["key"]["a"]["b"].getInt());
    ASSERT_EQ(3, mergedDoc["key"]["a"]["e"].getInt());
    ASSERT_EQ(2, mergedDoc["key"]["c"]["d"].getInt());
    ASSERT_EQ(4, mergedDoc["key"]["f"]["g"].getInt());
}

/** Add Document fields. */
class AddField {
public:
    void run() {
        MutableDocument md;
        md.addField("foo", Value(1));
        ASSERT_EQUALS(1ULL, md.peek().computeSize());
        ASSERT_EQUALS(1, md.peek()["foo"].getInt());
        md.addField("bar", Value(99));
        ASSERT_EQUALS(2ULL, md.peek().computeSize());
        ASSERT_EQUALS(99, md.peek()["bar"].getInt());
        // No assertion is triggered by a duplicate field name.
        md.addField("a", Value(5));

        Document final = md.freeze();
        ASSERT_EQUALS(3ULL, final.computeSize());
        assertRoundTrips(final);

        // Add field to the document as an lvalue.
        MutableDocument md1;
        Value v1(1);
        md1.addField("a", v1);
        ASSERT_VALUE_EQ(md1.peek().getField("a"), v1);

        // Set field to the document as an lvalue.
        Value v2(2);
        md1.setField("a", v2);
        ASSERT_VALUE_EQ(md1.peek().getField("a"), v2);

        // Set nested field to the document as an lvalue.
        FieldPath xxyyzz("xx.yy.zz");
        Value v3("nested"_sd);
        md1.setNestedField(xxyyzz, v3);
        ASSERT_VALUE_EQ(md1.peek().getNestedField(xxyyzz), v3);
    }
};

/** Get Document values. */
class GetValue {
public:
    void run() {
        Document document = fromBson(BSON("a" << 1 << "b" << 2.2));
        ASSERT_EQUALS(1, document["a"].getInt());
        ASSERT_EQUALS(1, document["a"].getInt());
        ASSERT_EQUALS(2.2, document["b"].getDouble());
        ASSERT_EQUALS(2.2, document["b"].getDouble());
        // Missing field.
        ASSERT(document["c"].missing());
        ASSERT(document["c"].missing());
        assertRoundTrips(document);
    }
};

/** Get Document fields. */
class SetField {
public:
    void run() {
        Document original = fromBson(BSON("a" << 1 << "b" << 2.2 << "c" << 99));

        // Initial positions. Used at end of function to make sure nothing moved
        const Position apos = original.positionOf("a");
        const Position bpos = original.positionOf("c");
        const Position cpos = original.positionOf("c");

        MutableDocument md(original);

        // Set the first field.
        md.setField("a", Value("foo"_sd));
        ASSERT_EQUALS(3ULL, md.peek().computeSize());
        ASSERT_EQUALS("foo", md.peek()["a"].getString());
        ASSERT_EQUALS("foo", getNthField(md.peek(), 0).second.getString());
        assertRoundTrips(md.peek());
        // Set the second field.
        md["b"] = Value("bar"_sd);
        ASSERT_EQUALS(3ULL, md.peek().computeSize());
        ASSERT_EQUALS("bar", md.peek()["b"].getString());
        ASSERT_EQUALS("bar", getNthField(md.peek(), 1).second.getString());
        assertRoundTrips(md.peek());

        // Remove the second field.
        md.setField("b", Value());
        LOGV2(20585, "{md_peek}", "md_peek"_attr = md.peek().toString());
        ASSERT_EQUALS(2ULL, md.peek().computeSize());
        ASSERT(md.peek()["b"].missing());
        ASSERT_EQUALS("a", getNthField(md.peek(), 0).first);
        ASSERT_EQUALS("c", getNthField(md.peek(), 1).first);
        ASSERT_EQUALS(99, md.peek()["c"].getInt());
        assertRoundTrips(md.peek());

        // Remove the first field.
        md["a"] = Value();
        ASSERT_EQUALS(1ULL, md.peek().computeSize());
        ASSERT(md.peek()["a"].missing());
        ASSERT_EQUALS("c", getNthField(md.peek(), 0).first);
        ASSERT_EQUALS(99, md.peek()["c"].getInt());
        assertRoundTrips(md.peek());

        // Remove the final field. Verify document is empty.
        md.remove("c");
        ASSERT(md.peek().empty());
        ASSERT_EQUALS(0ULL, md.peek().computeSize());
        ASSERT_DOCUMENT_EQ(md.peek(), Document());
        ASSERT(!FieldIterator(md.peek()).more());
        ASSERT(md.peek()["c"].missing());
        assertRoundTrips(md.peek());

        // Set a nested field using []
        md["x"]["y"]["z"] = Value("nested"_sd);
        ASSERT_VALUE_EQ(md.peek()["x"]["y"]["z"], Value("nested"_sd));

        // Set a nested field using setNestedField
        FieldPath xxyyzz("xx.yy.zz");
        md.setNestedField(xxyyzz, Value("nested"_sd));
        ASSERT_VALUE_EQ(md.peek().getNestedField(xxyyzz), Value("nested"_sd));

        // Set a nested fields through an existing empty document
        md["xxx"] = Value(Document());
        md["xxx"]["yyy"] = Value(Document());
        FieldPath xxxyyyzzz("xxx.yyy.zzz");
        md.setNestedField(xxxyyyzzz, Value("nested"_sd));
        ASSERT_VALUE_EQ(md.peek().getNestedField(xxxyyyzzz), Value("nested"_sd));

        // Make sure nothing moved
        ASSERT_EQUALS(apos, md.peek().positionOf("a"));
        ASSERT_EQUALS(bpos, md.peek().positionOf("c"));
        ASSERT_EQUALS(cpos, md.peek().positionOf("c"));
        ASSERT_EQUALS(Position(), md.peek().positionOf("d"));
    }
};

/** Document comparator. */
class Compare {
public:
    void run() {
        assertComparison(0, BSONObj(), BSONObj());
        assertComparison(0, BSON("a" << 1), BSON("a" << 1));
        assertComparison(-1, BSONObj(), BSON("a" << 1));
        assertComparison(-1, BSON("a" << 1), BSON("c" << 1));
        assertComparison(0, BSON("a" << 1 << "r" << 2), BSON("a" << 1 << "r" << 2));
        assertComparison(-1, BSON("a" << 1), BSON("a" << 1 << "r" << 2));
        assertComparison(0, BSON("a" << 2), BSON("a" << 2));
        assertComparison(-1, BSON("a" << 1), BSON("a" << 2));
        assertComparison(-1, BSON("a" << 1 << "b" << 1), BSON("a" << 1 << "b" << 2));
        // numbers sort before strings
        assertComparison(-1, BSON("a" << 1), BSON("a" << "foo"));
        // numbers sort before strings, even if keys compare otherwise
        assertComparison(-1, BSON("b" << 1), BSON("a" << "foo"));
        // null before number, even if keys compare otherwise
        assertComparison(-1, BSON("z" << BSONNULL), BSON("a" << 1));
    }

public:
    int cmp(const BSONObj& a, const BSONObj& b) {
        int result = DocumentComparator().compare(fromBson(a), fromBson(b));
        return  // sign
            result < 0   ? -1
            : result > 0 ? 1
                         : 0;
    }
    void assertComparison(int expectedResult, const BSONObj& a, const BSONObj& b) {
        ASSERT_EQUALS(expectedResult, cmp(a, b));
        ASSERT_EQUALS(-expectedResult, cmp(b, a));
        if (expectedResult == 0) {
            ASSERT_EQUALS(hash(a), hash(b));
        }
    }
    size_t hash(const BSONObj& obj) {
        size_t seed = 0x106e1e1;
        const StringDataComparator* stringComparator = nullptr;
        Document(obj).hash_combine(seed, stringComparator);
        return seed;
    }
};

/** Shallow copy clone of a single field Document. */
class Clone {
public:
    void run() {
        const Document document = fromBson(BSON("a" << BSON("b" << 1)));
        MutableDocument cloneOnDemand(document);

        // Check equality.
        ASSERT_DOCUMENT_EQ(document, cloneOnDemand.peek());
        // Check pointer equality of sub document.
        ASSERT_EQUALS(document["a"].getDocument().getPtr(),
                      cloneOnDemand.peek()["a"].getDocument().getPtr());


        // Change field in clone and ensure the original document's field is unchanged.
        cloneOnDemand.setField(StringData("a"), Value(2));
        ASSERT_VALUE_EQ(Value(1), document.getNestedField(FieldPath("a.b")));


        // setNestedField and ensure the original document is unchanged.

        cloneOnDemand.reset(document);
        std::vector<Position> path;
        ASSERT_VALUE_EQ(Value(1), document.getNestedField(FieldPath("a.b"), &path));

        cloneOnDemand.setNestedField(path, Value(2));

        ASSERT_VALUE_EQ(Value(1), document.getNestedField(FieldPath("a.b")));
        ASSERT_VALUE_EQ(Value(2), cloneOnDemand.peek().getNestedField(FieldPath("a.b")));
        ASSERT_DOCUMENT_EQ(DOC("a" << DOC("b" << 1)), document);
        ASSERT_DOCUMENT_EQ(DOC("a" << DOC("b" << 2)), cloneOnDemand.freeze());
    }
};

/** Shallow copy clone of a multi field Document. */
class CloneMultipleFields {
public:
    void run() {
        Document document = fromBson(fromjson("{a:1,b:['ra',4],c:{z:1},d:'lal'}"));
        Document clonedDocument = document.clone();
        ASSERT_DOCUMENT_EQ(document, clonedDocument);
    }
};

/** FieldIterator for an empty Document. */
class FieldIteratorEmpty {
public:
    void run() {
        FieldIterator iterator((Document()));
        ASSERT(!iterator.more());
    }
};

/** FieldIterator for a single field Document. */
class FieldIteratorSingle {
public:
    void run() {
        FieldIterator iterator(fromBson(BSON("a" << 1)));
        ASSERT(iterator.more());
        Document::FieldPair field = iterator.next();
        ASSERT_EQUALS("a", field.first);
        ASSERT_EQUALS(1, field.second.getInt());
        ASSERT(!iterator.more());
    }
};

/** FieldIterator for a multiple field Document. */
class FieldIteratorMultiple {
public:
    void run() {
        FieldIterator iterator(fromBson(BSON("a" << 1 << "b" << 5.6 << "c"
                                                 << "z")));
        ASSERT(iterator.more());
        Document::FieldPair field = iterator.next();
        ASSERT_EQUALS("a", field.first);
        ASSERT_EQUALS(1, field.second.getInt());
        ASSERT(iterator.more());

        Document::FieldPair field2 = iterator.next();
        ASSERT_EQUALS("b", field2.first);
        ASSERT_EQUALS(5.6, field2.second.getDouble());
        ASSERT(iterator.more());

        Document::FieldPair field3 = iterator.next();
        ASSERT_EQUALS("c", field3.first);
        ASSERT_EQUALS("z", field3.second.getString());
        ASSERT(!iterator.more());
    }
};

class AllTypesDoc {
public:
    void run() {
        // These are listed in order of BSONType with some duplicates
        append("minkey", MINKEY);
        // EOO not valid in middle of BSONObj
        append("double", 1.0);
        append("c++", "string\0after NUL"_sd);
        append("StringData", "string\0after NUL"_sd);
        append("emptyObj", BSONObj());
        append("filledObj", BSON("a" << 1));
        append("emptyArray", BSON("" << BSONArray()).firstElement());
        append("filledArray", BSON("" << BSON_ARRAY(1 << "a")).firstElement());
        append("binData", BSONBinData("a\0b", 3, BinDataGeneral));
        append("binDataCustom", BSONBinData("a\0b", 3, bdtCustom));
        append("binDataUUID", BSONBinData("123456789\0abcdef", 16, bdtUUID));
        append("undefined", BSONUndefined);
        append("oid", OID());
        append("true", true);
        append("false", false);
        append("date", Date_t::now());
        append("null", BSONNULL);
        append("regex", BSONRegEx(".*"));
        append("regexFlags", BSONRegEx(".*", "i"));
        append("regexEmpty", BSONRegEx("", ""));
        append("dbref", BSONDBRef("foo", OID()));
        append("code", BSONCode("function() {}"));
        append("codeNul", BSONCode("var nul = '\0'"_sd));
        append("symbol", BSONSymbol("foo"));
        append("symbolNul", BSONSymbol("f\0o"_sd));
        append("codeWScope", BSONCodeWScope("asdf", BSONObj()));
        append("codeWScopeWScope", BSONCodeWScope("asdf", BSON("one" << 1)));
        append("int", 1);
        append("timestamp", Timestamp());
        append("long", 1LL);
        append("very long", 1LL << 40);
        append("maxkey", MAXKEY);

        const BSONArray arr = arrBuilder.arr();

        // can't use append any more since arrBuilder is done
        objBuilder << "mega array" << arr;
        docBuilder["mega array"] = mongo::Value(values);

        const BSONObj obj = objBuilder.obj();
        const Document doc = docBuilder.freeze();

        const BSONObj obj2 = toBson(doc);
        const Document doc2 = fromBson(obj);

        // logical equality
        ASSERT_BSONOBJ_EQ(obj, obj2);
        ASSERT_DOCUMENT_EQ(doc, doc2);

        // binary equality
        ASSERT_EQUALS(obj.objsize(), obj2.objsize());
        ASSERT_EQUALS(memcmp(obj.objdata(), obj2.objdata(), obj.objsize()), 0);

        // ensure sorter serialization round-trips correctly
        BufBuilder bb;
        doc.serializeForSorter(bb);
        BufReader reader(bb.buf(), bb.len());
        const Document doc3 =
            Document::deserializeForSorter(reader, Document::SorterDeserializeSettings());
        BSONObj obj3 = toBson(doc3);
        ASSERT_EQUALS(obj.objsize(), obj3.objsize());
        ASSERT_EQUALS(memcmp(obj.objdata(), obj3.objdata(), obj.objsize()), 0);
    }

    template <typename T>
    void append(const char* name, const T& thing) {
        objBuilder << name << thing;
        arrBuilder << thing;
        docBuilder[name] = mongo::Value(thing);
        values.push_back(mongo::Value(thing));
    }

    std::vector<mongo::Value> values;
    MutableDocument docBuilder;
    BSONObjBuilder objBuilder;
    BSONArrayBuilder arrBuilder;
};

TEST(DocumentTest, ToBsonSizeTraits) {
    constexpr size_t longStringLength = 9 * 1024 * 1024;
    static_assert(longStringLength <= BSONObjMaxInternalSize &&
                  2 * longStringLength > BSONObjMaxInternalSize &&
                  2 * longStringLength <= BufferMaxSize);
    std::string longString(longStringLength, 'A');
    MutableDocument md;
    md.addField("a", Value(longString));
    ASSERT_DOES_NOT_THROW(md.peek().toBson());
    md.addField("b", Value(longString));
    ASSERT_THROWS_CODE(md.peek().toBson(), DBException, ErrorCodes::BSONObjectTooLarge);
    ASSERT_THROWS_CODE(
        md.peek().toBson<BSONObj::DefaultSizeTrait>(), DBException, ErrorCodes::BSONObjectTooLarge);
    ASSERT_DOES_NOT_THROW(md.peek().toBson<BSONObj::LargeSizeTrait>());
}
}  // namespace Document

namespace MetaFields {
using mongo::Document;

TEST(MetaFields, ChangeStreamControlDocument) {
    // Documents should not have the 'control event' flag set.
    ASSERT_FALSE(Document().metadata().isChangeStreamControlEvent());

    // Empty document created via building should not have the 'control event' flag set.
    {
        MutableDocument docBuilder;
        Document doc = docBuilder.freeze();
        ASSERT_FALSE(doc.metadata().isChangeStreamControlEvent());

        // Cloning the document should also not set the flag.
        Document cloned = doc.clone();
        ASSERT_FALSE(cloned.metadata().isChangeStreamControlEvent());
        ASSERT_FALSE(doc.metadata().isChangeStreamControlEvent());
    }

    // Explicitly setting the 'control event' flag on the document should work.
    {
        MutableDocument docBuilder;
        docBuilder.metadata().setChangeStreamControlEvent();
        Document doc = docBuilder.freeze();
        ASSERT_TRUE(doc.metadata().isChangeStreamControlEvent());

        // Cloning the document should also clone the flag.
        Document cloned = doc.clone();
        ASSERT_TRUE(cloned.metadata().isChangeStreamControlEvent());
        ASSERT_TRUE(doc.metadata().isChangeStreamControlEvent());
    }

    // Creating a regular document from BSON should not set the flag.
    {
        Document source = Document::fromBsonWithMetaData(BSON("foo" << "bar"));

        MutableDocument docBuilder;
        docBuilder.copyMetaDataFrom(source);
        auto doc = docBuilder.freeze();
        ASSERT_FALSE(doc.metadata().isChangeStreamControlEvent());
    }

    // Creating a document from BSON with the flag present should set the flag correctly.
    for (auto value : {true, false}) {
        Document doc = Document::fromBsonWithMetaData(
            BSON("foo" << "bar" << Document::metaFieldChangeStreamControlEvent << value));

        // Note: the presence of '$changeStreamControlEvent' is enough to set the equivalent
        // metadata bit. The value that '$changeStreamControlEvent' is set to does not matter.
        ASSERT_TRUE(doc.metadata().isChangeStreamControlEvent());
    }
}

TEST(MetaFields, TextScoreBasics) {
    // Documents should not have a text score until it is set.
    ASSERT_FALSE(Document().metadata().hasTextScore());

    // Setting the text score should work as expected.
    MutableDocument docBuilder;
    docBuilder.metadata().setTextScore(1.0);
    Document doc = docBuilder.freeze();
    ASSERT_TRUE(doc.metadata().hasTextScore());
    ASSERT_EQ(1.0, doc.metadata().getTextScore());
}

TEST(MetaFields, RandValBasics) {
    // Documents should not have a random value until it is set.
    ASSERT_FALSE(Document().metadata().hasRandVal());

    // Setting the random value field should work as expected.
    MutableDocument docBuilder;
    docBuilder.metadata().setRandVal(1.0);
    Document doc = docBuilder.freeze();
    ASSERT_TRUE(doc.metadata().hasRandVal());
    ASSERT_EQ(1, doc.metadata().getRandVal());

    // Setting the random value twice should keep the second value.
    MutableDocument docBuilder2;
    docBuilder2.metadata().setRandVal(1.0);
    docBuilder2.metadata().setRandVal(2.0);
    Document doc2 = docBuilder2.freeze();
    ASSERT_TRUE(doc2.metadata().hasRandVal());
    ASSERT_EQ(2.0, doc2.metadata().getRandVal());
}

TEST(MetaFields, SearchScoreBasic) {
    // Documents should not have a search score until it is set.
    ASSERT_FALSE(Document().metadata().hasSearchScore());

    // Setting the search score field should work as expected.
    MutableDocument docBuilder;
    docBuilder.metadata().setSearchScore(1.23);
    Document doc = docBuilder.freeze();
    ASSERT_TRUE(doc.metadata().hasSearchScore());
    ASSERT_EQ(1.23, doc.metadata().getSearchScore());

    // Setting the searchScore twice should keep the second value.
    MutableDocument docBuilder2;
    docBuilder2.metadata().setSearchScore(1.0);
    docBuilder2.metadata().setSearchScore(2.0);
    Document doc2 = docBuilder2.freeze();
    ASSERT_TRUE(doc2.metadata().hasSearchScore());
    ASSERT_EQ(2.0, doc2.metadata().getSearchScore());
}

TEST(MetaFields, SearchHighlightsBasic) {
    // Documents should not have a search highlights until it is set.
    ASSERT_FALSE(Document().metadata().hasSearchHighlights());

    // Setting the search highlights field should work as expected.
    MutableDocument docBuilder;
    Value highlights = DOC_ARRAY("a"_sd << "b"_sd);
    docBuilder.metadata().setSearchHighlights(highlights);
    Document doc = docBuilder.freeze();
    ASSERT_TRUE(doc.metadata().hasSearchHighlights());
    ASSERT_VALUE_EQ(doc.metadata().getSearchHighlights(), highlights);

    // Setting the searchHighlights twice should keep the second value.
    MutableDocument docBuilder2;
    Value otherHighlights = DOC_ARRAY("snippet1"_sd << "snippet2"_sd
                                                    << "snippet3"_sd);
    docBuilder2.metadata().setSearchHighlights(highlights);
    docBuilder2.metadata().setSearchHighlights(otherHighlights);
    Document doc2 = docBuilder2.freeze();
    ASSERT_TRUE(doc2.metadata().hasSearchHighlights());
    ASSERT_VALUE_EQ(doc2.metadata().getSearchHighlights(), otherHighlights);
}

TEST(MetaFields, SearchScoreDetailsBasic) {
    // Documents should not have a value for searchScoreDetails until it is set.
    ASSERT_FALSE(Document().metadata().hasSearchScoreDetails());

    // Setting the searchScoreDetails field should work as expected.
    MutableDocument docBuilder;
    BSONObj details = BSON("scoreDetails" << "foo");
    docBuilder.metadata().setSearchScoreDetails(details);
    Document doc = docBuilder.freeze();
    ASSERT_TRUE(doc.metadata().hasSearchScoreDetails());
    ASSERT_BSONOBJ_EQ(doc.metadata().getSearchScoreDetails(), details);

    // Setting the searchScoreDetails twice should keep the second value.
    MutableDocument docBuilder2;
    BSONObj otherDetails = BSON("scoreDetails" << "bar");
    docBuilder2.metadata().setSearchScoreDetails(details);
    docBuilder2.metadata().setSearchScoreDetails(otherDetails);
    Document doc2 = docBuilder2.freeze();
    ASSERT_TRUE(doc2.metadata().hasSearchScoreDetails());
    ASSERT_BSONOBJ_EQ(doc2.metadata().getSearchScoreDetails(), otherDetails);
}

TEST(MetaFields, IndexKeyMetadataSerializesCorrectly) {
    Document doc{BSON("a" << 1)};
    MutableDocument mutableDoc{doc};
    mutableDoc.metadata().setIndexKey(BSON("b" << 1));
    doc = mutableDoc.freeze();

    ASSERT_TRUE(doc.metadata().hasIndexKey());
    ASSERT_BSONOBJ_EQ(doc.metadata().getIndexKey(), BSON("b" << 1));

    auto serialized = doc.toBsonWithMetaData();
    ASSERT_BSONOBJ_EQ(serialized, BSON("a" << 1 << "$indexKey" << BSON("b" << 1)));
}

TEST(MetaFields, FromBsonWithMetadataAcceptsIndexKeyMetadata) {
    auto doc = Document::fromBsonWithMetaData(BSON("a" << 1 << "$indexKey" << BSON("b" << 1)));
    ASSERT_TRUE(doc.metadata().hasIndexKey());
    ASSERT_BSONOBJ_EQ(doc.metadata().getIndexKey(), BSON("b" << 1));
    auto bsonWithoutMetadata = doc.toBson();
    ASSERT_BSONOBJ_EQ(bsonWithoutMetadata, BSON("a" << 1));
}

TEST(MetaFields, FromBsonWithMetadataHandlesEmptyFieldName) {
    auto bson = BSON("" << 1 << "$indexKey" << BSON("b" << 1));
    auto doc = Document::fromBsonWithMetaData(bson);
    ASSERT_TRUE(doc.metadata().hasIndexKey());
    ASSERT_BSONOBJ_EQ(doc.metadata().getIndexKey(), BSON("b" << 1));
    auto bsonWithoutMetadata = doc.toBson();
    ASSERT_BSONOBJ_EQ(bsonWithoutMetadata, BSON("" << 1));
}

TEST(MetaFields, CopyMetadataFromCopiesAllMetadata) {
    // Used to set 'score' metadata.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    Document source = Document::fromBsonWithMetaData(
        BSON("a" << 1 << "$textScore" << 9.9 << "b" << 1 << "$randVal" << 42.0 << "c" << 1
                 << "$sortKey" << BSON("x" << 1) << "d" << 1 << "$dis" << 3.2 << "e" << 1 << "$pt"
                 << BSON_ARRAY(1 << 2) << "f" << 1 << "$searchScore" << 5.4 << "g" << 1
                 << "$searchHighlights"
                 << "foo"
                 << "h" << 1 << "$indexKey" << BSON("y" << 1) << "$searchScoreDetails"
                 << BSON("scoreDetails" << "foo") << "$searchSortValues" << BSON("a" << 1)
                 << "$vectorSearchScore" << 6.7 << "$score" << 8.1));

    MutableDocument destination{};
    destination.copyMetaDataFrom(source);
    auto result = destination.freeze();

    ASSERT_EQ(result.metadata().getTextScore(), 9.9);
    ASSERT_EQ(result.metadata().getRandVal(), 42.0);
    ASSERT_VALUE_EQ(result.metadata().getSortKey(), Value(1));
    ASSERT_EQ(result.metadata().getGeoNearDistance(), 3.2);
    ASSERT_VALUE_EQ(result.metadata().getGeoNearPoint(), Value{BSON_ARRAY(1 << 2)});
    ASSERT_EQ(result.metadata().getSearchScore(), 5.4);
    ASSERT_VALUE_EQ(result.metadata().getSearchHighlights(), Value{"foo"_sd});
    ASSERT_BSONOBJ_EQ(result.metadata().getIndexKey(), BSON("y" << 1));
    ASSERT_BSONOBJ_EQ(result.metadata().getSearchScoreDetails(), BSON("scoreDetails" << "foo"));
    ASSERT_BSONOBJ_EQ(result.metadata().getSearchSortValues(), BSON("a" << 1));
    ASSERT_EQ(result.metadata().getVectorSearchScore(), 6.7);
    ASSERT_EQ(result.metadata().getScore(), 8.1);
    ASSERT_FALSE(result.metadata().isChangeStreamControlEvent());
}

class SerializationTest : public unittest::Test {
protected:
    Document roundTrip(const Document& input) {
        BufBuilder bb;
        input.serializeForSorter(bb);
        BufReader reader(bb.buf(), bb.len());
        return Document::deserializeForSorter(reader, Document::SorterDeserializeSettings());
    }

    void assertRoundTrips(const Document& input) {
        // Round trip to/from a buffer.
        auto output = roundTrip(input);
        ASSERT_DOCUMENT_EQ(output, input);
        ASSERT_EQ(output.metadata().hasTextScore(), input.metadata().hasTextScore());
        ASSERT_EQ(output.metadata().hasRandVal(), input.metadata().hasRandVal());
        ASSERT_EQ(output.metadata().hasSearchScore(), input.metadata().hasSearchScore());
        ASSERT_EQ(output.metadata().hasSearchHighlights(), input.metadata().hasSearchHighlights());
        ASSERT_EQ(output.metadata().hasIndexKey(), input.metadata().hasIndexKey());
        ASSERT_EQ(output.metadata().hasVectorSearchScore(),
                  input.metadata().hasVectorSearchScore());
        ASSERT_EQ(output.metadata().hasScore(), input.metadata().hasScore());
        if (input.metadata().hasTextScore()) {
            ASSERT_EQ(output.metadata().getTextScore(), input.metadata().getTextScore());
        }
        if (input.metadata().hasRandVal()) {
            ASSERT_EQ(output.metadata().getRandVal(), input.metadata().getRandVal());
        }
        if (input.metadata().hasSearchScore()) {
            ASSERT_EQ(output.metadata().getSearchScore(), input.metadata().getSearchScore());
        }
        if (input.metadata().hasSearchHighlights()) {
            ASSERT_VALUE_EQ(output.metadata().getSearchHighlights(),
                            input.metadata().getSearchHighlights());
        }
        if (input.metadata().hasIndexKey()) {
            ASSERT_BSONOBJ_EQ(output.metadata().getIndexKey(), input.metadata().getIndexKey());
        }
        if (input.metadata().hasSearchScoreDetails()) {
            ASSERT_BSONOBJ_EQ(output.metadata().getSearchScoreDetails(),
                              input.metadata().getSearchScoreDetails());
        }
        if (input.metadata().hasVectorSearchScore()) {
            ASSERT_EQ(output.metadata().getVectorSearchScore(),
                      input.metadata().getVectorSearchScore());
        }
        if (input.metadata().hasScore()) {
            ASSERT_EQ(output.metadata().getScore(), input.metadata().getScore());
        }
        ASSERT_EQ(input.metadata().isChangeStreamControlEvent(),
                  output.metadata().isChangeStreamControlEvent());

        ASSERT(output.toBson().binaryEqual(input.toBson()));
    }
};

TEST_F(SerializationTest, MetaSerializationNoVals) {
    MutableDocument docBuilder;
    docBuilder.metadata().setTextScore(10.0);
    docBuilder.metadata().setRandVal(20.0);
    docBuilder.metadata().setSearchScore(30.0);
    docBuilder.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd << "def"_sd));
    docBuilder.metadata().setSearchScoreDetails(BSON("scoreDetails" << "foo"));
    docBuilder.metadata().setVectorSearchScore(40.0);
    docBuilder.metadata().setScore(60.0);
    assertRoundTrips(docBuilder.freeze());
}

TEST_F(SerializationTest, MetaSerializationWithVals) {
    // Same as above test, but add a non-meta field as well.
    MutableDocument docBuilder(DOC("foo" << 10));
    docBuilder.metadata().setTextScore(10.0);
    docBuilder.metadata().setRandVal(20.0);
    docBuilder.metadata().setSearchScore(30.0);
    docBuilder.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd << "def"_sd));
    docBuilder.metadata().setIndexKey(BSON("key" << 42));
    docBuilder.metadata().setSearchScoreDetails(BSON("scoreDetails" << "foo"));
    docBuilder.metadata().setVectorSearchScore(40.0);
    docBuilder.metadata().setScore(60.0);
    docBuilder.metadata().setChangeStreamControlEvent();
    assertRoundTrips(docBuilder.freeze());
}

TEST_F(SerializationTest, MetaSerializationSearchHighlightsNonArray) {
    MutableDocument docBuilder;
    docBuilder.metadata().setTextScore(10.0);
    docBuilder.metadata().setRandVal(20.0);
    docBuilder.metadata().setSearchScore(30.0);
    // Everything should still round trip even if the searchHighlights metadata isn't an array.
    docBuilder.metadata().setSearchHighlights(Value(1.23));
    assertRoundTrips(docBuilder.freeze());
}

TEST(MetaFields, ToAndFromBson) {
    // Used to set 'score' metadata.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    MutableDocument docBuilder;
    docBuilder.metadata().setTextScore(10.0);
    docBuilder.metadata().setRandVal(20.0);
    docBuilder.metadata().setSearchScore(30.0);
    docBuilder.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd << "def"_sd));
    docBuilder.metadata().setSearchScoreDetails(BSON("scoreDetails" << "foo"));
    docBuilder.metadata().setSearchSortValues(BSON("a" << 42));
    docBuilder.metadata().setVectorSearchScore(40.0);
    docBuilder.metadata().setScore(60.0);
    docBuilder.metadata().setChangeStreamControlEvent();
    Document doc = docBuilder.freeze();
    BSONObj obj = doc.toBsonWithMetaData();
    ASSERT_EQ(10.0, obj[Document::metaFieldTextScore].Double());
    ASSERT_EQ(20, obj[Document::metaFieldRandVal].numberLong());
    ASSERT_EQ(30.0, obj[Document::metaFieldSearchScore].Double());
    ASSERT_BSONOBJ_EQ(obj[Document::metaFieldSearchHighlights].embeddedObject(),
                      BSON_ARRAY("abc"_sd << "def"_sd));
    ASSERT_BSONOBJ_EQ(obj[Document::metaFieldSearchScoreDetails].Obj(),
                      BSON("scoreDetails" << "foo"));
    ASSERT_BSONOBJ_EQ(BSON("a" << 42), obj[Document::metaFieldSearchSortValues].Obj());
    ASSERT_EQ(40.0, obj[Document::metaFieldVectorSearchScore].Double());
    ASSERT_EQ(60.0, obj[Document::metaFieldScore].Double());
    ASSERT_TRUE(obj[Document::metaFieldChangeStreamControlEvent].boolean());
    Document fromBson = Document::fromBsonWithMetaData(obj);
    ASSERT_TRUE(fromBson.metadata().hasTextScore());
    ASSERT_TRUE(fromBson.metadata().hasRandVal());
    ASSERT_EQ(10.0, fromBson.metadata().getTextScore());
    ASSERT_EQ(20, fromBson.metadata().getRandVal());
    ASSERT_BSONOBJ_EQ(BSON("scoreDetails" << "foo"), fromBson.metadata().getSearchScoreDetails());
    ASSERT_BSONOBJ_EQ(BSON("a" << 42), fromBson.metadata().getSearchSortValues());
    ASSERT_EQ(40.0, fromBson.metadata().getVectorSearchScore());
    ASSERT_EQ(60.0, fromBson.metadata().getScore());
    ASSERT_TRUE(fromBson.metadata().isChangeStreamControlEvent());
}

TEST(MetaFields, ToAndFromBsonTrivialConvertibility) {
    Value sortKey{Document{{"token"_sd, "SOMENCODEDATA"_sd}}};
    // Create a document with a backing BSONObj and separate metadata.
    auto origObjNoMetadata = BSON("a" << 42);
    ASSERT_FALSE(origObjNoMetadata.hasField(Document::metaFieldSortKey));
    ASSERT_FALSE(origObjNoMetadata.hasField(Document::metaFieldChangeStreamControlEvent));

    MutableDocument docBuilder;
    docBuilder.reset(origObjNoMetadata, false);
    docBuilder.metadata().setSortKey(sortKey, true);
    docBuilder.metadata().setChangeStreamControlEvent();
    Document docWithSeparateBsonAndMetadata = docBuilder.freeze();

    BSONObj origObjWithMetadata = docWithSeparateBsonAndMetadata.toBsonWithMetaData();
    ASSERT_TRUE(origObjWithMetadata.hasField(Document::metaFieldSortKey));
    ASSERT_TRUE(origObjWithMetadata.hasField(Document::metaFieldChangeStreamControlEvent));
    Document restoredDocWithMetadata = Document::fromBsonWithMetaData(origObjWithMetadata);
    ASSERT_DOCUMENT_EQ(docWithSeparateBsonAndMetadata, restoredDocWithMetadata);

    // Test the 'isTriviallyConvertible()' function.
    // The original document is trivially convertible without metadata because the metadata was
    // added to the document separately from the backing BSON object.
    ASSERT_TRUE(docWithSeparateBsonAndMetadata.isTriviallyConvertible());
    // The original document is NOT trivially convertible with metadata because the metadata was
    // added to the document and does not exist in the BSONObj.
    ASSERT_FALSE(docWithSeparateBsonAndMetadata.isTriviallyConvertibleWithMetadata());
    // The restored document is trivially convertible with metadata because the underlying BSONObj
    // contains the metadata serialized from the original document.
    ASSERT_TRUE(restoredDocWithMetadata.isTriviallyConvertibleWithMetadata());
    // The restored document is NOT trivially convertible without metadata because the metadata
    // fields need to be stripped from the underlying BSONObj.
    ASSERT_FALSE(restoredDocWithMetadata.isTriviallyConvertible());

    // Test that the conversion with metadata 'origObjWithMetadata' -> 'restoredDocWithMetadata' ->
    // 'restoredObjWithMetadata' is trivial because the backing BSON already contains metadata and
    // neither the metadata nor the non-metadata fields have been modified.
    BSONObj restoredObjWithMetadata = restoredDocWithMetadata.toBsonWithMetaData();
    ASSERT_TRUE(restoredObjWithMetadata.hasField(Document::metaFieldSortKey));
    // Test that 'restoredObjWithMetadata' is referring to the exact same memory location as
    // 'origObjWithMetadata', i.e. both objdata() and objsize() match.
    ASSERT_EQ(origObjWithMetadata.objdata(), restoredObjWithMetadata.objdata());
    ASSERT_EQ(origObjWithMetadata.objsize(), restoredObjWithMetadata.objsize());

    // Test that the conversion without metadata 'origObjWithMetadata' -> 'restoredDocWithMetadata'
    // -> 'strippedRestoredObj' is NOT trivial because the backing BSON has metadata that must be
    // omitted during serialization.
    BSONObj strippedRestoredObj = restoredDocWithMetadata.toBson();
    ASSERT_FALSE(strippedRestoredObj.hasField(Document::metaFieldSortKey));
    // 'restoredDocWithMetadata' is trivially convertible with metadata and converting it to BSON
    // without metadata will return a new BSON object.
    ASSERT_TRUE(origObjNoMetadata.binaryEqual(strippedRestoredObj));
    ASSERT_NE(origObjNoMetadata.objdata(), strippedRestoredObj.objdata());

    // Test that the conversion without metadata 'origObjNoMetadata' ->
    // 'docWithSeparateBsonAndMetadata' -> 'restoredObjNoMetadata' is trivial because
    // 'origObjNoMetadata' does not contain any metadata.
    BSONObj restoredObjNoMetadata = docWithSeparateBsonAndMetadata.toBson();
    ASSERT_FALSE(restoredObjNoMetadata.hasField(Document::metaFieldSortKey));
    // Test that 'restoredObjNoMetadata' is referring to the exact same memory location as
    // 'origObjNoMetadata', i.e. both objdata() and objsize() match.
    ASSERT_EQ(origObjNoMetadata.objdata(), restoredObjNoMetadata.objdata());
    ASSERT_EQ(origObjNoMetadata.objsize(), restoredObjNoMetadata.objsize());
}

TEST(MetaFields, TrivialConvertibilityBsonWithoutMetadata) {
    // Test that an unmodified document without metadata is trivially convertible to BSON with and
    // without metadata.
    auto bsonWithoutMetadata = BSON("a" << 42);
    Document doc(bsonWithoutMetadata);
    ASSERT_TRUE(doc.isTriviallyConvertible());
    ASSERT_TRUE(doc.isTriviallyConvertibleWithMetadata());
}

TEST(MetaFields, TrivialConvertibilityNoBson) {
    // A Document created with no backing BSON is not trivially convertible.
    auto docNoBson = Document{{"a", 42}};
    ASSERT_FALSE(docNoBson.isTriviallyConvertible());
    ASSERT_FALSE(docNoBson.isTriviallyConvertibleWithMetadata());

    // An empty Document is trivially convertible, since the default BSONObj is also empty.
    auto emptyDoc = Document{};
    ASSERT_TRUE(emptyDoc.isTriviallyConvertible());
    ASSERT_TRUE(emptyDoc.isTriviallyConvertibleWithMetadata());
}

TEST(MetaFields, TrivialConvertibilityModified) {
    // Modifying a document with a backing BSON renders it not trivially convertible.
    MutableDocument mutDocModified(Document(BSON("a" << 42)));
    mutDocModified.addField("b", Value(43));
    auto modifiedDoc = mutDocModified.freeze();
    ASSERT_FALSE(modifiedDoc.isTriviallyConvertible());
    ASSERT_FALSE(modifiedDoc.isTriviallyConvertibleWithMetadata());
}

TEST(MetaFields, TrivialConvertibilityMetadataModified) {
    // Modifying the metadata of a document with a backing BSON renders it not trivially convertible
    // with metadata.
    MutableDocument mutDocModifiedMd(
        Document::fromBsonWithMetaData(BSON(Document::metaFieldTextScore << 10.0)));
    mutDocModifiedMd.metadata().setRandVal(20.0);
    auto modifiedMdDoc = mutDocModifiedMd.freeze();
    ASSERT_FALSE(modifiedMdDoc.isTriviallyConvertible());
    ASSERT_FALSE(modifiedMdDoc.isTriviallyConvertibleWithMetadata());
}

TEST(MetaFields, MetaFieldsIncludedInDocumentApproximateSize) {
    MutableDocument docBuilder;
    docBuilder.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd << "def"_sd));
    const size_t smallMetadataDocSize = docBuilder.freeze().getApproximateSize();

    // The second document has a larger "search highlights" object.
    MutableDocument docBuilder2;
    docBuilder2.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd << "def"_sd
                                                                  << "ghijklmnop"_sd));
    Document doc2 = docBuilder2.freeze();
    const size_t bigMetadataDocSize = doc2.getApproximateSize();
    ASSERT_GT(bigMetadataDocSize, smallMetadataDocSize);

    // Do a sanity check on the amount of space taken by metadata in document 2. Note that the size
    // of certain data types may vary on different build variants, so we cannot assert on the exact
    // size.
    ASSERT_LT(doc2.getMetadataApproximateSize(), 400U);

    Document emptyDoc;
    ASSERT_LT(emptyDoc.getMetadataApproximateSize(), 100U);
}

TEST(MetaFields, BadSerialization) {
    // Write an unrecognized option to the buffer.
    BufBuilder bb;
    // Signal there are 0 fields.
    bb.appendNum(0);
    // This would specify a meta field with an invalid type.
    bb.appendNum(char(DocumentMetadataFields::MetaType::kNumFields) + 1);
    // Signals end of input.
    bb.appendNum(char(0));
    BufReader reader(bb.buf(), bb.len());
    ASSERT_THROWS_CODE(
        Document::deserializeForSorter(reader, Document::SorterDeserializeSettings()),
        AssertionException,
        28744);
}
}  // namespace MetaFields

namespace Value {

using mongo::Value;

BSONObj toBson(const Value& value) {
    if (value.missing())
        return BSONObj();  // EOO

    BSONObjBuilder bob;
    value.addToBsonObj(&bob, "");
    return bob.obj();
}

Value fromBson(const BSONObj& obj) {
    BSONElement element = obj.firstElement();
    return Value(element);
}

void assertRoundTrips(const Value& value1) {
    BSONObj obj1 = toBson(value1);
    Value value2 = fromBson(obj1);
    BSONObj obj2 = toBson(value2);
    ASSERT_BSONOBJ_EQ(obj1, obj2);
    ASSERT_VALUE_EQ(value1, value2);
    ASSERT_EQUALS(value1.getType(), value2.getType());
}

class BSONArrayTest {
public:
    void run() {
        ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3)), DOC_ARRAY(1 << 2 << 3));
        ASSERT_VALUE_EQ(Value(BSONArray()), Value(std::vector<Value>()));
    }
};

/** Int type. */
class Int {
public:
    void run() {
        Value value = Value(5);
        ASSERT_EQUALS(5, value.getInt());
        ASSERT_EQUALS(5, value.getLong());
        ASSERT_EQUALS(5, value.getDouble());
        ASSERT_EQUALS(BSONType::numberInt, value.getType());
        assertRoundTrips(value);
    }
};

/** Long type. */
class Long {
public:
    void run() {
        Value value = Value(99LL);
        ASSERT_EQUALS(99, value.getLong());
        ASSERT_EQUALS(99, value.getDouble());
        ASSERT_EQUALS(BSONType::numberLong, value.getType());
        assertRoundTrips(value);
    }
};

/** Double type. */
class Double {
public:
    void run() {
        Value value = Value(5.5);
        ASSERT_EQUALS(5.5, value.getDouble());
        ASSERT_EQUALS(BSONType::numberDouble, value.getType());
        assertRoundTrips(value);
    }
};

/** String type. */
class String {
public:
    void run() {
        Value value = Value("foo"_sd);
        ASSERT_EQUALS("foo", value.getString());
        ASSERT_EQUALS(BSONType::string, value.getType());
        assertRoundTrips(value);
    }
};

/** String with a null character. */
class StringWithNull {
public:
    void run() {
        std::string withNull("a\0b", 3);
        BSONObj objWithNull = BSON("" << withNull);
        ASSERT_EQUALS(withNull, objWithNull[""].str());
        Value value = fromBson(objWithNull);
        ASSERT_EQUALS(withNull, value.getString());
        assertRoundTrips(value);
    }
};

/**
 * SERVER-43205: Constructing a Value with a very large BSONElement string causes the Value
 * constructor to throw before it can completely initialize its ValueStorage member, which has the
 * potential to lead to incorrect state.
 */
class LongString {
public:
    void run() {
        std::string longString(16793500, 'x');
        auto obj = BSON("str" << longString);
        ASSERT_THROWS_CODE(
            [&]() {
                Value{obj["str"]};
            }(),
            DBException,
            16493);
    }
};

/** Date type. */
class Date {
public:
    void run() {
        Value value = Value(Date_t::fromMillisSinceEpoch(999));
        ASSERT_EQUALS(999, value.getDate().toMillisSinceEpoch());
        ASSERT_EQUALS(mongo::BSONType::date, value.getType());
        assertRoundTrips(value);
    }
};

/** Timestamp type. */
class JSTimestamp {
public:
    void run() {
        Value value = Value(Timestamp(777));
        ASSERT(Timestamp(777) == value.getTimestamp());
        ASSERT_EQUALS(mongo::BSONType::timestamp, value.getType());
        assertRoundTrips(value);

        value = Value(Timestamp(~0U, 3));
        ASSERT(Timestamp(~0U, 3) == value.getTimestamp());
        ASSERT_EQUALS(mongo::BSONType::timestamp, value.getType());
        assertRoundTrips(value);
    }
};

/** Document with no fields. */
class EmptyDocument {
public:
    void run() {
        mongo::Document document = mongo::Document();
        Value value = Value(document);
        ASSERT_EQUALS(document.getPtr(), value.getDocument().getPtr());
        ASSERT_EQUALS(BSONType::object, value.getType());
        assertRoundTrips(value);
    }
};

/** Document type. */
class Document {
public:
    void run() {
        mongo::MutableDocument md;
        md.addField("a", Value(5));
        md.addField("apple", Value("rrr"_sd));
        md.addField("banana", Value(-.3));
        mongo::Document document = md.freeze();

        Value value = Value(document);
        // Check document pointers are equal.
        ASSERT_EQUALS(document.getPtr(), value.getDocument().getPtr());
        // Check document contents.
        ASSERT_EQUALS(5, document["a"].getInt());
        ASSERT_EQUALS("rrr", document["apple"].getString());
        ASSERT_EQUALS(-.3, document["banana"].getDouble());
        ASSERT_EQUALS(BSONType::object, value.getType());
        assertRoundTrips(value);

        MutableDocument md1;
        Value v(1);
        md1.addField("a", v);

        MutableDocument md2;
        // Construct Value from an rvalue.
        md2.addField("nested", Value(md1.freeze()));

        ASSERT_VALUE_EQ(md2.peek().getNestedField("nested.a"), v);
        ASSERT_DOCUMENT_EQ(md1.freeze(), mongo::Document());
    }
};

/** Array with no elements. */
class EmptyArray {
public:
    void run() {
        std::vector<Value> array;
        Value value(array);
        const std::vector<Value>& array2 = value.getArray();

        ASSERT(array2.empty());
        ASSERT_EQUALS(BSONType::array, value.getType());
        ASSERT_EQUALS(0U, value.getArrayLength());
        assertRoundTrips(value);
    }
};

/** Array type. */
class Array {
public:
    void run() {
        std::vector<Value> array;
        array.push_back(Value(5));
        array.push_back(Value("lala"_sd));
        array.push_back(Value(3.14));
        Value value = Value(array);
        const std::vector<Value>& array2 = value.getArray();

        ASSERT(!array2.empty());
        ASSERT_EQUALS(array2.size(), 3U);
        ASSERT_EQUALS(5, array2[0].getInt());
        ASSERT_EQUALS("lala", array2[1].getString());
        ASSERT_EQUALS(3.14, array2[2].getDouble());
        ASSERT_EQUALS(mongo::BSONType::array, value.getType());
        ASSERT_EQUALS(3U, value.getArrayLength());
        assertRoundTrips(value);
    }
};

/** Oid type. */
class Oid {
public:
    void run() {
        Value value = fromBson(BSON("" << OID("abcdefabcdefabcdefabcdef")));
        ASSERT_EQUALS(OID("abcdefabcdefabcdefabcdef"), value.getOid());
        ASSERT_EQUALS(BSONType::oid, value.getType());
        assertRoundTrips(value);
    }
};

/** Bool type. */
class Bool {
public:
    void run() {
        Value value = fromBson(BSON("" << true));
        ASSERT_EQUALS(true, value.getBool());
        ASSERT_EQUALS(BSONType::boolean, value.getType());
        assertRoundTrips(value);
    }
};

/** Regex type. */
class Regex {
public:
    void run() {
        Value value = fromBson(fromjson("{'':/abc/}"));
        ASSERT_EQUALS(std::string("abc"), value.getRegex());
        ASSERT_EQUALS(BSONType::regEx, value.getType());
        assertRoundTrips(value);
    }
};

/** Symbol type (currently unsupported). */
class Symbol {
public:
    void run() {
        Value value(BSONSymbol("FOOBAR"));
        ASSERT_EQUALS("FOOBAR", value.getSymbol());
        ASSERT_EQUALS(BSONType::symbol, value.getType());
        assertRoundTrips(value);
    }
};

/** Undefined type. */
class Undefined {
public:
    void run() {
        Value value = Value(BSONUndefined);
        ASSERT_EQUALS(BSONType::undefined, value.getType());
        assertRoundTrips(value);
    }
};

/** Null type. */
class Null {
public:
    void run() {
        Value value = Value(BSONNULL);
        ASSERT_EQUALS(BSONType::null, value.getType());
        assertRoundTrips(value);
    }
};

/** True value. */
class True {
public:
    void run() {
        Value value = Value(true);
        ASSERT_EQUALS(true, value.getBool());
        ASSERT_EQUALS(BSONType::boolean, value.getType());
        assertRoundTrips(value);
    }
};

/** False value. */
class False {
public:
    void run() {
        Value value = Value(false);
        ASSERT_EQUALS(false, value.getBool());
        ASSERT_EQUALS(BSONType::boolean, value.getType());
        assertRoundTrips(value);
    }
};

/** -1 value. */
class MinusOne {
public:
    void run() {
        Value value = Value(-1);
        ASSERT_EQUALS(-1, value.getInt());
        ASSERT_EQUALS(BSONType::numberInt, value.getType());
        assertRoundTrips(value);
    }
};

/** 0 value. */
class Zero {
public:
    void run() {
        Value value = Value(0);
        ASSERT_EQUALS(0, value.getInt());
        ASSERT_EQUALS(BSONType::numberInt, value.getType());
        assertRoundTrips(value);
    }
};

/** 1 value. */
class One {
public:
    void run() {
        Value value = Value(1);
        ASSERT_EQUALS(1, value.getInt());
        ASSERT_EQUALS(BSONType::numberInt, value.getType());
        assertRoundTrips(value);
    }
};

namespace Coerce {

class ToBoolBase {
public:
    virtual ~ToBoolBase() {}
    void run() {
        ASSERT_EQUALS(expected(), value().coerceToBool());
    }

protected:
    virtual Value value() = 0;
    virtual bool expected() = 0;
};

class ToBoolTrue : public ToBoolBase {
    bool expected() override {
        return true;
    }
};

class ToBoolFalse : public ToBoolBase {
    bool expected() override {
        return false;
    }
};

/** Coerce 0 to bool. */
class ZeroIntToBool : public ToBoolFalse {
    Value value() override {
        return Value(0);
    }
};

/** Coerce -1 to bool. */
class NonZeroIntToBool : public ToBoolTrue {
    Value value() override {
        return Value(-1);
    }
};

/** Coerce 0LL to bool. */
class ZeroLongToBool : public ToBoolFalse {
    Value value() override {
        return Value(0LL);
    }
};

/** Coerce 5LL to bool. */
class NonZeroLongToBool : public ToBoolTrue {
    Value value() override {
        return Value(5LL);
    }
};

/** Coerce 0.0 to bool. */
class ZeroDoubleToBool : public ToBoolFalse {
    Value value() override {
        return Value(0);
    }
};

/** Coerce -1.3 to bool. */
class NonZeroDoubleToBool : public ToBoolTrue {
    Value value() override {
        return Value(-1.3);
    }
};

/** Coerce "" to bool. */
class StringToBool : public ToBoolTrue {
    Value value() override {
        return Value(StringData());
    }
};

/** Coerce {} to bool. */
class ObjectToBool : public ToBoolTrue {
    Value value() override {
        return Value(mongo::Document());
    }
};

/** Coerce [] to bool. */
class ArrayToBool : public ToBoolTrue {
    Value value() override {
        return Value(std::vector<Value>());
    }
};

/** Coerce Date(0) to bool. */
class DateToBool : public ToBoolTrue {
    Value value() override {
        return Value(Date_t{});
    }
};

/** Coerce js literal regex to bool. */
class RegexToBool : public ToBoolTrue {
    Value value() override {
        return fromBson(fromjson("{''://}"));
    }
};

/** Coerce true to bool. */
class TrueToBool : public ToBoolTrue {
    Value value() override {
        return fromBson(BSON("" << true));
    }
};

/** Coerce false to bool. */
class FalseToBool : public ToBoolFalse {
    Value value() override {
        return fromBson(BSON("" << false));
    }
};

/** Coerce null to bool. */
class NullToBool : public ToBoolFalse {
    Value value() override {
        return Value(BSONNULL);
    }
};

/** Coerce undefined to bool. */
class UndefinedToBool : public ToBoolFalse {
    Value value() override {
        return Value(BSONUndefined);
    }
};

class ToIntBase {
public:
    virtual ~ToIntBase() {}
    void run() {
        if (asserts())
            ASSERT_THROWS(value().coerceToInt(), AssertionException);
        else
            ASSERT_EQUALS(expected(), value().coerceToInt());
    }

protected:
    virtual Value value() = 0;
    virtual int expected() {
        return 0;
    }
    virtual bool asserts() {
        return false;
    }
};

/** Coerce -5 to int. */
class IntToInt : public ToIntBase {
    Value value() override {
        return Value(-5);
    }
    int expected() override {
        return -5;
    }
};

/** Coerce long to int. */
class LongToInt : public ToIntBase {
    Value value() override {
        return Value(0xff00000007LL);
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce 9.8 to int. */
class DoubleToInt : public ToIntBase {
    Value value() override {
        return Value(9.8);
    }
    int expected() override {
        return 9;
    }
};

/** Coerce null to int. */
class NullToInt : public ToIntBase {
    Value value() override {
        return Value(BSONNULL);
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce undefined to int. */
class UndefinedToInt : public ToIntBase {
    Value value() override {
        return Value(BSONUndefined);
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce "" to int unsupported. */
class StringToInt {
public:
    void run() {
        ASSERT_THROWS(Value(StringData()).coerceToInt(), AssertionException);
    }
};

/** Coerce maxInt to int */
class MaxIntToInt : public ToIntBase {
    Value value() override {
        return Value((double)std::numeric_limits<int>::max());
    }
    int expected() override {
        return std::numeric_limits<int>::max();
    }
};

/** Coerce minInt to int */
class MinIntToInt : public ToIntBase {
    Value value() override {
        return Value((double)std::numeric_limits<int>::min());
    }
    int expected() override {
        return std::numeric_limits<int>::min();
    }
};

/** Coerce maxInt + 1 to int */
class TooLargeToInt : public ToIntBase {
    Value value() override {
        return Value((double)std::numeric_limits<int>::max() + 1);
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce minInt - 1 to int */
class TooLargeNegativeToInt : public ToIntBase {
    Value value() override {
        return Value((double)std::numeric_limits<int>::min() - 1);
    }
    bool asserts() override {
        return true;
    }
};

class ToLongBase {
public:
    virtual ~ToLongBase() {}
    void run() {
        if (asserts())
            ASSERT_THROWS(value().coerceToLong(), AssertionException);
        else
            ASSERT_EQUALS(expected(), value().coerceToLong());
    }

protected:
    virtual Value value() = 0;
    virtual long long expected() {
        return 0;
    }
    virtual bool asserts() {
        return false;
    }
};

/** Coerce -5 to long. */
class IntToLong : public ToLongBase {
    Value value() override {
        return Value(-5);
    }
    long long expected() override {
        return -5;
    }
};

/** Coerce long to long. */
class LongToLong : public ToLongBase {
    Value value() override {
        return Value(0xff00000007LL);
    }
    long long expected() override {
        return 0xff00000007LL;
    }
};

/** Coerce 9.8 to long. */
class DoubleToLong : public ToLongBase {
    Value value() override {
        return Value(9.8);
    }
    long long expected() override {
        return 9;
    }
};

/** Coerce infinity to long. */
class InfToLong : public ToLongBase {
    Value value() override {
        return Value(std::numeric_limits<double>::infinity());
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce negative infinity to long. **/
class NegInfToLong : public ToLongBase {
    Value value() override {
        return Value(std::numeric_limits<double>::infinity() * -1);
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce large to long. **/
class InvalidLargeToLong : public ToLongBase {
    Value value() override {
        return Value(pow(2, 63));
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce lowest double to long. **/
class LowestDoubleToLong : public ToLongBase {
    Value value() override {
        return Value(static_cast<double>(std::numeric_limits<long long>::lowest()));
    }
    long long expected() override {
        return std::numeric_limits<long long>::lowest();
    }
};

/** Coerce 'towards infinity' to long **/
class TowardsInfinityToLong : public ToLongBase {
    Value value() override {
        return Value(static_cast<double>(std::nextafter(std::numeric_limits<long long>::lowest(),
                                                        std::numeric_limits<double>::lowest())));
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce null to long. */
class NullToLong : public ToLongBase {
    Value value() override {
        return Value(BSONNULL);
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce undefined to long. */
class UndefinedToLong : public ToLongBase {
    Value value() override {
        return Value(BSONUndefined);
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce string to long unsupported. */
class StringToLong {
public:
    void run() {
        ASSERT_THROWS(Value(StringData()).coerceToLong(), AssertionException);
    }
};

class ToDoubleBase {
public:
    virtual ~ToDoubleBase() {}
    void run() {
        if (asserts())
            ASSERT_THROWS(value().coerceToDouble(), AssertionException);
        else
            ASSERT_EQUALS(expected(), value().coerceToDouble());
    }

protected:
    virtual Value value() = 0;
    virtual double expected() {
        return 0;
    }
    virtual bool asserts() {
        return false;
    }
};

/** Coerce -5 to double. */
class IntToDouble : public ToDoubleBase {
    Value value() override {
        return Value(-5);
    }
    double expected() override {
        return -5;
    }
};

/** Coerce long to double. */
class LongToDouble : public ToDoubleBase {
    Value value() override {
        // A long that cannot be exactly represented as a double.
        return Value(static_cast<double>(0x8fffffffffffffffLL));
    }
    double expected() override {
        return static_cast<double>(0x8fffffffffffffffLL);
    }
};

/** Coerce double to double. */
class DoubleToDouble : public ToDoubleBase {
    Value value() override {
        return Value(9.8);
    }
    double expected() override {
        return 9.8;
    }
};

/** Coerce null to double. */
class NullToDouble : public ToDoubleBase {
    Value value() override {
        return Value(BSONNULL);
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce undefined to double. */
class UndefinedToDouble : public ToDoubleBase {
    Value value() override {
        return Value(BSONUndefined);
    }
    bool asserts() override {
        return true;
    }
};

/** Coerce string to double unsupported. */
class StringToDouble {
public:
    void run() {
        ASSERT_THROWS(Value(StringData()).coerceToDouble(), AssertionException);
    }
};

class ToDateBase {
public:
    virtual ~ToDateBase() {}
    void run() {
        ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(expected()), value().coerceToDate());
    }

protected:
    virtual Value value() = 0;
    virtual long long expected() = 0;
};

/** Coerce date to date. */
class DateToDate : public ToDateBase {
    Value value() override {
        return Value(Date_t::fromMillisSinceEpoch(888));
    }
    long long expected() override {
        return 888;
    }
};

/**
 * Convert timestamp to date.  This extracts the time portion of the timestamp, which
 * is different from BSON behavior of interpreting all bytes as a date.
 */
class TimestampToDate : public ToDateBase {
    Value value() override {
        return Value(Timestamp(777, 666));
    }
    long long expected() override {
        return 777 * 1000;
    }
};

/** Coerce string to date unsupported. */
class StringToDate {
public:
    void run() {
        ASSERT_THROWS(Value(StringData()).coerceToDate(), AssertionException);
    }
};

class ToStringBase {
public:
    virtual ~ToStringBase() {}
    void run() {
        ASSERT_EQUALS(expected(), value().coerceToString());
    }

protected:
    virtual Value value() = 0;
    virtual std::string expected() {
        return "";
    }
};

/** Coerce -0.2 to string. */
class DoubleToString : public ToStringBase {
    Value value() override {
        return Value(-0.2);
    }
    std::string expected() override {
        return "-0.2";
    }
};

/** Coerce -4 to string. */
class IntToString : public ToStringBase {
    Value value() override {
        return Value(-4);
    }
    std::string expected() override {
        return "-4";
    }
};

/** Coerce 10000LL to string. */
class LongToString : public ToStringBase {
    Value value() override {
        return Value(10000LL);
    }
    std::string expected() override {
        return "10000";
    }
};

/** Coerce string to string. */
class StringToString : public ToStringBase {
    Value value() override {
        return Value("fO_o"_sd);
    }
    std::string expected() override {
        return "fO_o";
    }
};

/** Coerce timestamp to string. */
class TimestampToString : public ToStringBase {
    Value value() override {
        return Value(Timestamp(1, 2));
    }
    std::string expected() override {
        return Timestamp(1, 2).toStringPretty();
    }
};

/** Coerce date to string. */
class DateToString : public ToStringBase {
    Value value() override {
        return Value(Date_t::fromMillisSinceEpoch(1234567890123LL));
    }
    std::string expected() override {
        return "2009-02-13T23:31:30.123Z";
    }  // from js
};

/** Coerce null to string. */
class NullToString : public ToStringBase {
    Value value() override {
        return Value(BSONNULL);
    }
};

/** Coerce undefined to string. */
class UndefinedToString : public ToStringBase {
    Value value() override {
        return Value(BSONUndefined);
    }
};

/** Coerce document to string unsupported. */
class DocumentToString {
public:
    void run() {
        ASSERT_THROWS(Value(mongo::Document()).coerceToString(), AssertionException);
    }
};

/** Coerce timestamp to timestamp. */
class TimestampToTimestamp {
public:
    void run() {
        Value value = Value(Timestamp(1010));
        ASSERT(Timestamp(1010) == value.coerceToTimestamp());
    }
};

/** Coerce date to timestamp unsupported. */
class DateToTimestamp {
public:
    void run() {
        ASSERT_THROWS(Value(Date_t::fromMillisSinceEpoch(1010)).coerceToTimestamp(),
                      AssertionException);
    }
};

}  // namespace Coerce

/** Get the "widest" of two numeric types. */
class GetWidestNumeric {
public:
    void run() {
        // Numeric types.
        assertWidest(BSONType::numberInt, BSONType::numberInt, BSONType::numberInt);
        assertWidest(BSONType::numberLong, BSONType::numberInt, BSONType::numberLong);
        assertWidest(BSONType::numberDouble, BSONType::numberInt, BSONType::numberDouble);
        assertWidest(BSONType::numberLong, BSONType::numberLong, BSONType::numberLong);
        assertWidest(BSONType::numberDouble, BSONType::numberLong, BSONType::numberDouble);
        assertWidest(BSONType::numberDouble, BSONType::numberDouble, BSONType::numberDouble);

        // Missing value and numeric types (result Undefined).
        assertWidest(BSONType::undefined, BSONType::numberInt, BSONType::undefined);
        assertWidest(BSONType::undefined, BSONType::numberInt, BSONType::undefined);
        assertWidest(BSONType::undefined, BSONType::numberLong, BSONType::null);
        assertWidest(BSONType::undefined, BSONType::numberLong, BSONType::undefined);
        assertWidest(BSONType::undefined, BSONType::numberDouble, BSONType::null);
        assertWidest(BSONType::undefined, BSONType::numberDouble, BSONType::undefined);

        // Missing value types (result Undefined).
        assertWidest(BSONType::undefined, BSONType::null, BSONType::null);
        assertWidest(BSONType::undefined, BSONType::null, BSONType::undefined);
        assertWidest(BSONType::undefined, BSONType::undefined, BSONType::undefined);

        // Other types (result Undefined).
        assertWidest(BSONType::undefined, BSONType::numberInt, BSONType::boolean);
        assertWidest(BSONType::undefined, BSONType::string, BSONType::numberDouble);
    }

private:
    void assertWidest(BSONType expectedWidest, BSONType a, BSONType b) {
        ASSERT_EQUALS(expectedWidest, Value::getWidestNumeric(a, b));
        ASSERT_EQUALS(expectedWidest, Value::getWidestNumeric(b, a));
    }
};

/** Add a Value to a BSONObj. */
class AddToBsonObj {
public:
    void run() {
        BSONObjBuilder bob;
        Value(4.4).addToBsonObj(&bob, "a");
        Value(22).addToBsonObj(&bob, "b");
        Value("astring"_sd).addToBsonObj(&bob, "c");
        ASSERT_BSONOBJ_EQ(BSON("a" << 4.4 << "b" << 22 << "c"
                                   << "astring"),
                          bob.obj());
    }
};

/** Add a Value to a BSONArray. */
class AddToBsonArray {
public:
    void run() {
        BSONArrayBuilder bab;
        Value(4.4).addToBsonArray(&bab);
        Value(22).addToBsonArray(&bab);
        Value("astring"_sd).addToBsonArray(&bab);
        ASSERT_BSONOBJ_EQ(BSON_ARRAY(4.4 << 22 << "astring"), bab.arr());
    }
};

/** Value comparator. */
class Compare {
public:
    void run() {
        BSONObjBuilder undefinedBuilder;
        undefinedBuilder.appendUndefined("");
        BSONObj undefined = undefinedBuilder.obj();

        // Undefined / null.
        assertComparison(0, undefined, undefined);
        assertComparison(-1, undefined, BSON("" << BSONNULL));
        assertComparison(0, BSON("" << BSONNULL), BSON("" << BSONNULL));

        // Undefined / null with other types.
        assertComparison(-1, undefined, BSON("" << 1));
        assertComparison(-1, undefined, BSON("" << "bar"));
        assertComparison(-1, BSON("" << BSONNULL), BSON("" << -1));
        assertComparison(-1, BSON("" << BSONNULL), BSON("" << "bar"));

        // Numeric types.
        assertComparison(0, 5, 5LL);
        assertComparison(0, -2, -2.0);
        assertComparison(0, 90LL, 90.0);
        assertComparison(-1, 5, 6LL);
        assertComparison(-1, -2, 2.1);
        assertComparison(1, 90LL, 89.999);
        assertComparison(-1, 90, 90.1);
        assertComparison(0,
                         std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::signaling_NaN());
        assertComparison(-1, std::numeric_limits<double>::quiet_NaN(), 5);

        // strings compare between numbers and objects
        assertComparison(1, "abc", 90);
        assertComparison(-1, "abc", BSON("a" << "b"));

        // String comparison.
        assertComparison(-1, "", "a");
        assertComparison(0, "a", "a");
        assertComparison(-1, "a", "b");
        assertComparison(-1, "aa", "b");
        assertComparison(1, "bb", "b");
        assertComparison(1, "bb", "b");
        assertComparison(1, "b-", "b");
        assertComparison(-1, "b-", "ba");
        // With a null character.
        assertComparison(1, std::string("a\0", 2), "a");

        // Object.
        assertComparison(0, fromjson("{'':{}}"), fromjson("{'':{}}"));
        assertComparison(0, fromjson("{'':{x:1}}"), fromjson("{'':{x:1}}"));
        assertComparison(-1, fromjson("{'':{}}"), fromjson("{'':{x:1}}"));
        assertComparison(-1, fromjson("{'':{'z': 1}}"), fromjson("{'':{'a': 'a'}}"));

        // Array.
        assertComparison(0, fromjson("{'':[]}"), fromjson("{'':[]}"));
        assertComparison(-1, fromjson("{'':[0]}"), fromjson("{'':[1]}"));
        assertComparison(-1, fromjson("{'':[0,0]}"), fromjson("{'':[1]}"));
        assertComparison(-1, fromjson("{'':[0]}"), fromjson("{'':[0,0]}"));
        assertComparison(-1, fromjson("{'':[0]}"), fromjson("{'':['']}"));

        // OID.
        assertComparison(0, OID("abcdefabcdefabcdefabcdef"), OID("abcdefabcdefabcdefabcdef"));
        assertComparison(1, OID("abcdefabcdefabcdefabcdef"), OID("010101010101010101010101"));

        // Bool.
        assertComparison(0, true, true);
        assertComparison(0, false, false);
        assertComparison(1, true, false);

        // Date.
        assertComparison(0, Date_t::fromMillisSinceEpoch(555), Date_t::fromMillisSinceEpoch(555));
        assertComparison(1, Date_t::fromMillisSinceEpoch(555), Date_t::fromMillisSinceEpoch(554));
        // Negative date.
        assertComparison(1, Date_t::fromMillisSinceEpoch(0), Date_t::fromMillisSinceEpoch(-1));

        // Regex.
        assertComparison(0, fromjson("{'':/a/}"), fromjson("{'':/a/}"));
        assertComparison(-1, fromjson("{'':/a/}"), fromjson("{'':/a/i}"));
        assertComparison(-1, fromjson("{'':/a/}"), fromjson("{'':/aa/}"));

        // Timestamp.
        assertComparison(0, Timestamp(1234), Timestamp(1234));
        assertComparison(-1, Timestamp(4), Timestamp(1234));
        // High bit set.
        assertComparison(1, Timestamp(~0U, 2), Timestamp(0, 3));

        // Cross-type comparisons. Listed in order of canonical types.
        assertComparison(-1, Value(mongo::MINKEY), Value());
        assertComparison(0, Value(), Value());
        assertComparison(0, Value(), Value(BSONUndefined));
        assertComparison(-1, Value(BSONUndefined), Value(BSONNULL));
        assertComparison(-1, Value(BSONNULL), Value(1));
        assertComparison(0, Value(1), Value(1LL));
        assertComparison(0, Value(1), Value(1.0));
        assertComparison(-1, Value(1), Value("string"_sd));
        assertComparison(0, Value("string"_sd), Value(BSONSymbol("string")));
        assertComparison(-1, Value("string"_sd), Value(mongo::Document()));
        assertComparison(-1, Value(mongo::Document()), Value(std::vector<Value>()));
        assertComparison(-1, Value(std::vector<Value>()), Value(BSONBinData("", 0, MD5Type)));
        assertComparison(-1, Value(BSONBinData("", 0, MD5Type)), Value(mongo::OID()));
        assertComparison(-1, Value(mongo::OID()), Value(false));
        assertComparison(-1, Value(false), Value(Date_t()));
        assertComparison(-1, Value(Date_t()), Value(Timestamp()));
        assertComparison(-1, Value(Timestamp()), Value(BSONRegEx("")));
        assertComparison(-1, Value(BSONRegEx("")), Value(BSONDBRef("", mongo::OID())));
        assertComparison(-1, Value(BSONDBRef("", mongo::OID())), Value(BSONCode("")));
        assertComparison(-1, Value(BSONCode("")), Value(BSONCodeWScope("", BSONObj())));
        assertComparison(-1, Value(BSONCodeWScope("", BSONObj())), Value(mongo::MAXKEY));
    }

private:
    template <class T, class U>
    void assertComparison(int expectedResult, const T& a, const U& b) {
        assertComparison(expectedResult, BSON("" << a), BSON("" << b));
    }
    void assertComparison(int expectedResult, const Timestamp& a, const Timestamp& b) {
        BSONObjBuilder first;
        first.append("", a);
        BSONObjBuilder second;
        second.append("", b);
        assertComparison(expectedResult, first.obj(), second.obj());
    }
    int sign(int cmp) {
        if (cmp == 0)
            return 0;
        else if (cmp < 0)
            return -1;
        else
            return 1;
    }
    int cmp(const Value& a, const Value& b) {
        return sign(ValueComparator().compare(a, b));
    }
    void assertComparison(int expectedResult, const BSONObj& a, const BSONObj& b) {
        assertComparison(expectedResult, fromBson(a), fromBson(b));
    }
    void assertComparison(int expectedResult, const Value& a, const Value& b) {
        LOGV2(20586, "testing {a} and {b}", "a"_attr = a.toString(), "b"_attr = b.toString());

        // reflexivity
        ASSERT_EQUALS(0, cmp(a, a));
        ASSERT_EQUALS(0, cmp(b, b));

        // symmetry
        ASSERT_EQUALS(expectedResult, cmp(a, b));
        ASSERT_EQUALS(-expectedResult, cmp(b, a));

        if (expectedResult == 0) {
            // equal values must hash equally.
            ASSERT_EQUALS(hash(a), hash(b));
        } else {
            // unequal values must hash unequally.
            // (not true in general but we should error if it fails in any of these cases)
            ASSERT_NOT_EQUALS(hash(a), hash(b));
        }

        // same as BSON
        ASSERT_EQUALS(expectedResult,
                      sign(toBson(a).firstElement().woCompare(toBson(b).firstElement())));
    }
    size_t hash(const Value& v) {
        size_t seed = 0xf00ba6;
        const StringDataComparator* stringComparator = nullptr;
        v.hash_combine(seed, stringComparator);
        return seed;
    }
};

class SubFields {
public:
    void run() {
        const Value val = fromBson(fromjson("{'': {a: [{x:1, b:[1, {y:1, c:1234, z:1}, 1]}]}}"));
        // ^ this outer object is removed by fromBson

        ASSERT(val.getType() == BSONType::object);

        ASSERT(val[999].missing());
        ASSERT(val["missing"].missing());
        ASSERT(val["a"].getType() == BSONType::array);

        ASSERT(val["a"][999].missing());
        ASSERT(val["a"]["missing"].missing());
        ASSERT(val["a"][0].getType() == BSONType::object);

        ASSERT(val["a"][0][999].missing());
        ASSERT(val["a"][0]["missing"].missing());
        ASSERT(val["a"][0]["b"].getType() == BSONType::array);

        ASSERT(val["a"][0]["b"][999].missing());
        ASSERT(val["a"][0]["b"]["missing"].missing());
        ASSERT(val["a"][0]["b"][1].getType() == BSONType::object);

        ASSERT(val["a"][0]["b"][1][999].missing());
        ASSERT(val["a"][0]["b"][1]["missing"].missing());
        ASSERT(val["a"][0]["b"][1]["c"].getType() == BSONType::numberInt);
        ASSERT_EQUALS(val["a"][0]["b"][1]["c"].getInt(), 1234);
    }
};

class SerializationOfMissingForSorter {
    // Can't be tested in AllTypesDoc since missing values are omitted when adding to BSON.
public:
    void run() {
        const Value missing;
        const Value arrayOfMissing = Value(std::vector<Value>(10));

        BufBuilder bb;
        missing.serializeForSorter(bb);
        arrayOfMissing.serializeForSorter(bb);

        BufReader reader(bb.buf(), bb.len());
        ASSERT_VALUE_EQ(missing,
                        Value::deserializeForSorter(reader, Value::SorterDeserializeSettings()));
        ASSERT_VALUE_EQ(arrayOfMissing,
                        Value::deserializeForSorter(reader, Value::SorterDeserializeSettings()));
    }
};

// Integer limits.
const int kIntMax = std::numeric_limits<int>::max();
const int kIntMin = std::numeric_limits<int>::lowest();
const long long kIntMaxAsLongLong = kIntMax;
const long long kIntMinAsLongLong = kIntMin;
const double kIntMaxAsDouble = kIntMax;
const double kIntMinAsDouble = kIntMin;
const Decimal128 kIntMaxAsDecimal = Decimal128(kIntMax);
const Decimal128 kIntMinAsDecimal = Decimal128(kIntMin);

// 64-bit integer limits.
const long long kLongLongMax = std::numeric_limits<long long>::max();
const long long kLongLongMin = std::numeric_limits<long long>::lowest();
const double kLongLongMaxAsDouble = static_cast<double>(kLongLongMax);
const double kLongLongMinAsDouble = static_cast<double>(kLongLongMin);
const Decimal128 kLongLongMaxAsDecimal = Decimal128(static_cast<int64_t>(kLongLongMax));
const Decimal128 kLongLongMinAsDecimal = Decimal128(static_cast<int64_t>(kLongLongMin));

// Double limits.
const double kDoubleMax = std::numeric_limits<double>::max();
const double kDoubleMin = std::numeric_limits<double>::lowest();
const Decimal128 kDoubleMaxAsDecimal = Decimal128(kDoubleMin);
const Decimal128 kDoubleMinAsDecimal = Decimal128(kDoubleMin);

TEST(ValueIntegral, CorrectlyIdentifiesValidIntegralValues) {
    ASSERT_TRUE(Value(kIntMax).integral());
    ASSERT_TRUE(Value(kIntMin).integral());
    ASSERT_TRUE(Value(kIntMaxAsLongLong).integral());
    ASSERT_TRUE(Value(kIntMinAsLongLong).integral());
    ASSERT_TRUE(Value(kIntMaxAsDouble).integral());
    ASSERT_TRUE(Value(kIntMinAsDouble).integral());
    ASSERT_TRUE(Value(kIntMaxAsDecimal).integral());
    ASSERT_TRUE(Value(kIntMinAsDecimal).integral());
}

TEST(ValueIntegral, CorrectlyIdentifiesInvalidIntegralValues) {
    ASSERT_FALSE(Value(kLongLongMax).integral());
    ASSERT_FALSE(Value(kLongLongMin).integral());
    ASSERT_FALSE(Value(kLongLongMaxAsDouble).integral());
    ASSERT_FALSE(Value(kLongLongMinAsDouble).integral());
    ASSERT_FALSE(Value(kLongLongMaxAsDecimal).integral());
    ASSERT_FALSE(Value(kLongLongMinAsDecimal).integral());
    ASSERT_FALSE(Value(kDoubleMax).integral());
    ASSERT_FALSE(Value(kDoubleMin).integral());
}

TEST(ValueIntegral, CorrectlyIdentifiesValid64BitIntegralValues) {
    ASSERT_TRUE(Value(kIntMax).integral64Bit());
    ASSERT_TRUE(Value(kIntMin).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMax).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMin).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMinAsDouble).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMaxAsDecimal).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMinAsDecimal).integral64Bit());
}

TEST(ValueIntegral, CorrectlyIdentifiesInvalid64BitIntegralValues) {
    ASSERT_FALSE(Value(kLongLongMaxAsDouble).integral64Bit());
    ASSERT_FALSE(Value(kDoubleMax).integral64Bit());
    ASSERT_FALSE(Value(kDoubleMin).integral64Bit());
    ASSERT_FALSE(Value(kDoubleMaxAsDecimal).integral64Bit());
    ASSERT_FALSE(Value(kDoubleMinAsDecimal).integral64Bit());
}

TEST(ValueOutput, StreamOutputForIllegalDateProducesErrorToken) {
    auto sout = std::ostringstream{};
    sout << Value{Date_t::min()};
    ASSERT_EQ("illegal date", sout.str());
}

}  // namespace Value

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("document") {}

    void setupTests() override {
        add<Document::AddField>();
        add<Document::GetValue>();
        add<Document::SetField>();
        add<Document::Compare>();
        add<Document::Clone>();
        add<Document::CloneMultipleFields>();
        add<Document::FieldIteratorEmpty>();
        add<Document::FieldIteratorSingle>();
        add<Document::FieldIteratorMultiple>();
        add<Document::AllTypesDoc>();

        add<Value::BSONArrayTest>();
        add<Value::Int>();
        add<Value::Long>();
        add<Value::Double>();
        add<Value::String>();
        add<Value::StringWithNull>();
        add<Value::LongString>();
        add<Value::Date>();
        add<Value::JSTimestamp>();
        add<Value::EmptyDocument>();
        add<Value::EmptyArray>();
        add<Value::Array>();
        add<Value::Oid>();
        add<Value::Bool>();
        add<Value::Regex>();
        add<Value::Symbol>();
        add<Value::Undefined>();
        add<Value::Null>();
        add<Value::True>();
        add<Value::False>();
        add<Value::MinusOne>();
        add<Value::Zero>();
        add<Value::One>();

        add<Value::Coerce::ZeroIntToBool>();
        add<Value::Coerce::NonZeroIntToBool>();
        add<Value::Coerce::ZeroLongToBool>();
        add<Value::Coerce::NonZeroLongToBool>();
        add<Value::Coerce::ZeroDoubleToBool>();
        add<Value::Coerce::NonZeroDoubleToBool>();
        add<Value::Coerce::StringToBool>();
        add<Value::Coerce::ObjectToBool>();
        add<Value::Coerce::ArrayToBool>();
        add<Value::Coerce::DateToBool>();
        add<Value::Coerce::RegexToBool>();
        add<Value::Coerce::TrueToBool>();
        add<Value::Coerce::FalseToBool>();
        add<Value::Coerce::NullToBool>();
        add<Value::Coerce::UndefinedToBool>();
        add<Value::Coerce::IntToInt>();
        add<Value::Coerce::LongToInt>();
        add<Value::Coerce::DoubleToInt>();
        add<Value::Coerce::NullToInt>();
        add<Value::Coerce::UndefinedToInt>();
        add<Value::Coerce::StringToInt>();
        add<Value::Coerce::MaxIntToInt>();
        add<Value::Coerce::MinIntToInt>();
        add<Value::Coerce::TooLargeToInt>();
        add<Value::Coerce::TooLargeNegativeToInt>();
        add<Value::Coerce::IntToLong>();
        add<Value::Coerce::LongToLong>();
        add<Value::Coerce::DoubleToLong>();
        add<Value::Coerce::NullToLong>();
        add<Value::Coerce::UndefinedToLong>();
        add<Value::Coerce::StringToLong>();
        add<Value::Coerce::InfToLong>();
        add<Value::Coerce::NegInfToLong>();
        add<Value::Coerce::InvalidLargeToLong>();
        add<Value::Coerce::LowestDoubleToLong>();
        add<Value::Coerce::TowardsInfinityToLong>();
        add<Value::Coerce::IntToDouble>();
        add<Value::Coerce::LongToDouble>();
        add<Value::Coerce::DoubleToDouble>();
        add<Value::Coerce::NullToDouble>();
        add<Value::Coerce::UndefinedToDouble>();
        add<Value::Coerce::StringToDouble>();
        add<Value::Coerce::DateToDate>();
        add<Value::Coerce::TimestampToDate>();
        add<Value::Coerce::StringToDate>();
        add<Value::Coerce::DoubleToString>();
        add<Value::Coerce::IntToString>();
        add<Value::Coerce::LongToString>();
        add<Value::Coerce::StringToString>();
        add<Value::Coerce::TimestampToString>();
        add<Value::Coerce::DateToString>();
        add<Value::Coerce::NullToString>();
        add<Value::Coerce::UndefinedToString>();
        add<Value::Coerce::DocumentToString>();
        add<Value::Coerce::TimestampToTimestamp>();
        add<Value::Coerce::DateToTimestamp>();

        add<Value::GetWidestNumeric>();
        add<Value::AddToBsonObj>();
        add<Value::AddToBsonArray>();
        add<Value::Compare>();
        add<Value::SubFields>();
        add<Value::SerializationOfMissingForSorter>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace
}  // namespace mongo
