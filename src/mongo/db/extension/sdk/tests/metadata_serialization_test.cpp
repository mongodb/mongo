/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/exec/document_value/document.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension::sdk {
namespace {

TEST(DocumentTest, ValidateToBsonWithOnlyMetadataAllFields) {
    MutableDocument mutableDocument;
    mutableDocument.addField("field1", Value("value1"_sd));
    mutableDocument.metadata().setTextScore(5.0);
    mutableDocument.metadata().setSearchScore(3.5);
    mutableDocument.metadata().setRandVal(0.42);
    mutableDocument.metadata().setSearchHighlights(Value(BSON_ARRAY("highlight1" << "highlight2")));

    Document document = mutableDocument.freeze();
    BSONObj metadataOnly = document.toBsonWithMetaDataOnly();

    ASSERT_EQ(metadataOnly["$textScore"].Double(), 5.0);
    ASSERT_EQ(metadataOnly["$searchScore"].Double(), 3.5);
    ASSERT_EQ(metadataOnly["$randVal"].Double(), 0.42);
    ASSERT_TRUE(metadataOnly.hasField("$searchHighlights"));
    ASSERT_FALSE(metadataOnly.hasField("field1"));
}

TEST(DocumentTest, ValidateToBsonWithOnlyEmptyMetadata) {
    MutableDocument mutableDocument;
    mutableDocument.addField("field1", Value("value1"_sd));

    Document document = mutableDocument.freeze();
    BSONObj metadataOnly = document.toBsonWithMetaDataOnly();

    ASSERT_TRUE(metadataOnly.isEmpty());
}

TEST(DocumentTest, ValidateSerializationSucceeds) {
    MutableDocument mutableDocument;
    mutableDocument.addField("name", Value("test"_sd));
    mutableDocument.addField("count", Value(42));
    mutableDocument.metadata().setTextScore(2.5);
    mutableDocument.metadata().setSearchScore(1.8);

    Document document = mutableDocument.freeze();

    BSONObj documentOnly = document.toBson();
    BSONObj metadataOnly = document.toBsonWithMetaDataOnly();

    ASSERT_TRUE(documentOnly.hasField("name"));
    ASSERT_TRUE(documentOnly.hasField("count"));
    ASSERT_FALSE(documentOnly.hasField("$textScore"));
    ASSERT_FALSE(documentOnly.hasField("$searchScore"));

    ASSERT_FALSE(metadataOnly.hasField("name"));
    ASSERT_FALSE(metadataOnly.hasField("count"));
    ASSERT_TRUE(metadataOnly.hasField("$textScore"));
    ASSERT_TRUE(metadataOnly.hasField("$searchScore"));

    // Verify combined equals toBsonWithMetaData()
    BSONObjBuilder combined;
    combined.appendElements(documentOnly);
    combined.appendElements(metadataOnly);
    BSONObj combinedObj = combined.obj();

    ASSERT_BSONOBJ_EQ(combinedObj, document.toBsonWithMetaData());
}
}  // namespace
}  // namespace mongo::extension::sdk
