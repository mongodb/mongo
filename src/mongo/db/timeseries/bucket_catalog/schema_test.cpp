/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data_comparator.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::timeseries::bucket_catalog {

namespace {
using Entry = FlatBSONStore<SchemaElement, BSONTypeValue>::Entry;

size_t emptyStoreSize() {
    static const std::string emptyFieldName;
    return sizeof(Entry) + emptyFieldName.capacity() + sizeof(BSONTypeValue);
}

size_t emptySchemaSize() {
    return sizeof(FlatBSONStore<SchemaElement, BSONTypeValue>) + emptyStoreSize();
}

TEST(Schema, DataMemoryUsage) {
    // Check empty Data memory usage.
    SchemaStore::Data data;
    ASSERT_EQ(data.calculateMemUsage(), sizeof(BSONTypeValue));

    // Check non-empty Data memory usage.
    std::string fieldName("fieldName");
    BSONObj doc = BSON(fieldName << 1);
    BSONElement BSONElem = doc[fieldName];
    data.setValue(BSONElem);
    ASSERT_EQ(data.calculateMemUsage(), sizeof(BSONTypeValue));
}

TEST(Schema, ElementMemoryUsage) {
    SchemaElement schemaElem;
    std::string fieldName;

    // Check empty SchemaElement memory usage. Must account for empty string which may have memory
    // allocated.
    ASSERT_EQ(schemaElem.calculateMemUsage(), fieldName.capacity() + sizeof(BSONTypeValue));

    // Check non-empty SchemaElement.
    fieldName = "fieldName";
    BSONObj doc = BSON(fieldName << 1);
    BSONElement BSONElem = doc[fieldName];
    schemaElem.data().setValue(BSONElem);
    schemaElem.setFieldName(fieldName.data());
    ASSERT_EQ(schemaElem.calculateMemUsage(), fieldName.capacity() + sizeof(BSONTypeValue));
}

TEST(Schema, StoreMemoryUsage) {
    // Empty SchemaStore has one root Entry with an empty Element.
    SchemaStore schemaStore;
    ASSERT_EQ(schemaStore.calculateMemUsage(), emptyStoreSize());

    auto obj = schemaStore.root();

    // Insert an object with a 20 byte field name. The Obj should have 2 entries, the first is the
    // empty root and the second is an empty element with just a field name.
    std::string fieldName = "twentyByteLongString";
    obj.insert(obj.end(), fieldName);
    int64_t expectedMemoryUsage =
        emptyStoreSize() + sizeof(Entry) + fieldName.size() + sizeof(BSONTypeValue);
    ASSERT_GTE(schemaStore.calculateMemUsage(), expectedMemoryUsage);

    // Insert another identical obj. SchemaStore has an entries vector that can allocate for more
    // elements than its size.
    obj.insert(obj.end(), fieldName);
    expectedMemoryUsage =
        emptyStoreSize() + (2 * (sizeof(Entry) + fieldName.size() + sizeof(BSONTypeValue)));
    ASSERT_GTE(schemaStore.calculateMemUsage(), expectedMemoryUsage);
}

TEST(Schema, SchemaMemoryUsage) {
    Schema schema;

    // Confirm memUsage only reflects the root node before inserting anything.
    ASSERT_EQ(schema.calculateMemUsage(), emptySchemaSize());

    const auto* strCmp = &simpleStringDataComparator;

    // Insert non-empty element to Schema.
    std::string fieldA = "a";
    BSONObj doc1 = BSON(fieldA << 1 << "meta" << 4);
    schema.update(doc1, "meta"_sd, strCmp);
    int64_t oneElementSchemaSize =
        emptySchemaSize() + sizeof(Entry) + fieldA.capacity() + sizeof(BSONTypeValue);
    ASSERT_EQ(schema.calculateMemUsage(), oneElementSchemaSize);

    // Update same field value is a no-op and doesn't change memory usage.
    BSONObj doc2 = BSON(fieldA << 3 << "meta" << 4);
    schema.update(doc2, "meta"_sd, strCmp);
    ASSERT_EQ(schema.calculateMemUsage(), oneElementSchemaSize);

    // Update with additional field should increase memory usage.
    std::string fieldB = "b";
    BSONObj doc3 = BSON(fieldB << 3 << "meta" << 4);
    schema.update(doc3, "meta"_sd, strCmp);
    ASSERT_GT(schema.calculateMemUsage(), oneElementSchemaSize);
}

TEST(Schema, NestedSchemaMemoryUsage) {
    Schema schemaObj;
    const auto* strCmp = &simpleStringDataComparator;

    auto obj =
        BSON("a" << BSON("z" << 1) << "b"
                 << BSON_ARRAY(BSON("z" << 1)
                               << BSON("z" << 2)
                               << BSON_ARRAY(BSON("x" << 1) << BSON("y" << 2) << BSON("z" << 3))));
    schemaObj.update(obj, "_meta"_sd, strCmp);

    std::string sampleFieldName;
    int64_t sampleElementSize = sizeof(Entry) + sampleFieldName.capacity() + sizeof(BSONTypeValue);
    // 14 elements account for 8 inserted elements and 6 null elements for every
    // array sub-element.
    int64_t approxSchemaMemUsage = emptySchemaSize() + (14 * sampleElementSize);
    ASSERT_GTE(schemaObj.calculateMemUsage(), approxSchemaMemUsage);
    ASSERT_LTE(schemaObj.calculateMemUsage(), approxSchemaMemUsage * 2);

    schemaObj.update(BSON("c" << BSON_ARRAY(BSON("z" << 1) << BSON("z" << 2))), "_meta"_sd, strCmp);
    // Additional 5 elements from 3 being inserted and 2 null elements for array sub elements.
    approxSchemaMemUsage = emptySchemaSize() + (19 * sampleElementSize);
    ASSERT_GTE(schemaObj.calculateMemUsage(), approxSchemaMemUsage);
    ASSERT_LTE(schemaObj.calculateMemUsage(), approxSchemaMemUsage * 2);
}
}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
