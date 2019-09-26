/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_metadata_fields.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(DocumentMetadataFieldsTest, AllMetadataRoundtripsThroughSerialization) {
    DocumentMetadataFields metadata;
    metadata.setTextScore(9.9);
    metadata.setRandVal(42.0);
    metadata.setSortKey(Value(1), /* isSingleElementKey = */ true);
    metadata.setGeoNearDistance(3.2);
    metadata.setGeoNearPoint(Value{BSON_ARRAY(1 << 2)});
    metadata.setSearchScore(5.4);
    metadata.setSearchHighlights(Value{"foo"_sd});
    metadata.setIndexKey(BSON("b" << 1));

    BufBuilder builder;
    metadata.serializeForSorter(builder);
    DocumentMetadataFields deserialized;
    BufReader reader(builder.buf(), builder.len());
    DocumentMetadataFields::deserializeForSorter(reader, &deserialized);

    ASSERT_EQ(deserialized.getTextScore(), 9.9);
    ASSERT_EQ(deserialized.getRandVal(), 42.0);
    ASSERT_VALUE_EQ(deserialized.getSortKey(), Value(1));
    ASSERT_TRUE(deserialized.isSingleElementKey());
    ASSERT_EQ(deserialized.getGeoNearDistance(), 3.2);
    ASSERT_VALUE_EQ(deserialized.getGeoNearPoint(), Value{BSON_ARRAY(1 << 2)});
    ASSERT_EQ(deserialized.getSearchScore(), 5.4);
    ASSERT_VALUE_EQ(deserialized.getSearchHighlights(), Value{"foo"_sd});
    ASSERT_BSONOBJ_EQ(deserialized.getIndexKey(), BSON("b" << 1));
}

TEST(DocumentMetadataFieldsTest, HasMethodsReturnFalseForEmptyMetadata) {
    DocumentMetadataFields metadata;
    ASSERT_FALSE(metadata);
    ASSERT_FALSE(metadata.hasTextScore());
    ASSERT_FALSE(metadata.hasRandVal());
    ASSERT_FALSE(metadata.hasSortKey());
    ASSERT_FALSE(metadata.hasGeoNearPoint());
    ASSERT_FALSE(metadata.hasGeoNearDistance());
    ASSERT_FALSE(metadata.hasSearchScore());
    ASSERT_FALSE(metadata.hasSearchHighlights());
    ASSERT_FALSE(metadata.hasIndexKey());
}

TEST(DocumentMetadataFieldsTest, HasMethodsReturnTrueForInitializedMetadata) {
    DocumentMetadataFields metadata;

    ASSERT_FALSE(metadata.hasTextScore());
    metadata.setTextScore(9.9);
    ASSERT_TRUE(metadata.hasTextScore());

    ASSERT_FALSE(metadata.hasRandVal());
    metadata.setRandVal(42.0);
    ASSERT_TRUE(metadata.hasRandVal());

    ASSERT_FALSE(metadata.hasSortKey());
    metadata.setSortKey(Value(1), /* isSingleElementKey = */ true);
    ASSERT_TRUE(metadata.hasSortKey());
    ASSERT_TRUE(metadata.isSingleElementKey());

    ASSERT_FALSE(metadata.hasGeoNearDistance());
    metadata.setGeoNearDistance(3.2);
    ASSERT_TRUE(metadata.hasGeoNearDistance());

    ASSERT_FALSE(metadata.hasGeoNearPoint());
    metadata.setGeoNearPoint(Value{BSON_ARRAY(1 << 2)});
    ASSERT_TRUE(metadata.hasGeoNearPoint());

    ASSERT_FALSE(metadata.hasSearchScore());
    metadata.setSearchScore(5.4);
    ASSERT_TRUE(metadata.hasSearchScore());

    ASSERT_FALSE(metadata.hasSearchHighlights());
    metadata.setSearchHighlights(Value{"foo"_sd});
    ASSERT_TRUE(metadata.hasSearchHighlights());

    ASSERT_FALSE(metadata.hasIndexKey());
    metadata.setIndexKey(BSON("b" << 1));
    ASSERT_TRUE(metadata.hasIndexKey());
}

TEST(DocumentMetadataFieldsTest, MoveConstructor) {
    DocumentMetadataFields metadata;
    metadata.setTextScore(9.9);
    metadata.setRandVal(42.0);
    metadata.setSortKey(Value(1), /* isSingleElementKey = */ true);
    metadata.setGeoNearDistance(3.2);
    metadata.setGeoNearPoint(Value{BSON_ARRAY(1 << 2)});
    metadata.setSearchScore(5.4);
    metadata.setSearchHighlights(Value{"foo"_sd});
    metadata.setIndexKey(BSON("b" << 1));

    DocumentMetadataFields moveConstructed(std::move(metadata));
    ASSERT_TRUE(moveConstructed);
    ASSERT_EQ(moveConstructed.getTextScore(), 9.9);
    ASSERT_EQ(moveConstructed.getRandVal(), 42.0);
    ASSERT_VALUE_EQ(moveConstructed.getSortKey(), Value(1));
    ASSERT_TRUE(moveConstructed.isSingleElementKey());
    ASSERT_EQ(moveConstructed.getGeoNearDistance(), 3.2);
    ASSERT_VALUE_EQ(moveConstructed.getGeoNearPoint(), Value{BSON_ARRAY(1 << 2)});
    ASSERT_EQ(moveConstructed.getSearchScore(), 5.4);
    ASSERT_VALUE_EQ(moveConstructed.getSearchHighlights(), Value{"foo"_sd});
    ASSERT_BSONOBJ_EQ(moveConstructed.getIndexKey(), BSON("b" << 1));

    ASSERT_FALSE(metadata);
}

TEST(DocumentMetadataFieldsTest, MoveAssignmentOperator) {
    DocumentMetadataFields metadata;
    metadata.setTextScore(9.9);
    metadata.setRandVal(42.0);
    metadata.setSortKey(Value(1), /* isSingleElementKey = */ true);
    metadata.setGeoNearDistance(3.2);
    metadata.setGeoNearPoint(Value{BSON_ARRAY(1 << 2)});
    metadata.setSearchScore(5.4);
    metadata.setSearchHighlights(Value{"foo"_sd});
    metadata.setIndexKey(BSON("b" << 1));

    DocumentMetadataFields moveAssigned;
    moveAssigned.setTextScore(12.3);
    moveAssigned = std::move(metadata);
    ASSERT_TRUE(moveAssigned);

    ASSERT_EQ(moveAssigned.getTextScore(), 9.9);
    ASSERT_EQ(moveAssigned.getRandVal(), 42.0);
    ASSERT_VALUE_EQ(moveAssigned.getSortKey(), Value(1));
    ASSERT_TRUE(moveAssigned.isSingleElementKey());
    ASSERT_EQ(moveAssigned.getGeoNearDistance(), 3.2);
    ASSERT_VALUE_EQ(moveAssigned.getGeoNearPoint(), Value{BSON_ARRAY(1 << 2)});
    ASSERT_EQ(moveAssigned.getSearchScore(), 5.4);
    ASSERT_VALUE_EQ(moveAssigned.getSearchHighlights(), Value{"foo"_sd});
    ASSERT_BSONOBJ_EQ(moveAssigned.getIndexKey(), BSON("b" << 1));

    ASSERT_FALSE(metadata);
}

TEST(DocumentMetadataFieldsTest, CopyConstructor) {
    DocumentMetadataFields metadata;
    metadata.setTextScore(9.9);

    DocumentMetadataFields copied{metadata};

    ASSERT_TRUE(metadata);
    ASSERT_TRUE(copied);
    ASSERT_EQ(metadata.getTextScore(), 9.9);
    ASSERT_EQ(copied.getTextScore(), 9.9);
}

TEST(DocumentMetadataFieldsTest, CopyAssignmentOperator) {
    DocumentMetadataFields metadata;
    metadata.setTextScore(9.9);

    DocumentMetadataFields copied;
    copied.setTextScore(12.3);
    copied = metadata;

    ASSERT_TRUE(metadata);
    ASSERT_TRUE(copied);
    ASSERT_EQ(metadata.getTextScore(), 9.9);
    ASSERT_EQ(copied.getTextScore(), 9.9);
}

TEST(DocumentMetadataFieldsTest, MergeWithOnlyCopiesMetadataThatDestinationDoesNotHave) {
    DocumentMetadataFields source;
    source.setTextScore(9.9);
    source.setRandVal(42.0);
    source.setSortKey(Value(1), /* isSingleElementKey = */ true);
    source.setGeoNearDistance(3.2);

    DocumentMetadataFields destination;
    destination.setTextScore(12.3);
    destination.setRandVal(84.0);

    destination.mergeWith(source);

    ASSERT_EQ(destination.getTextScore(), 12.3);
    ASSERT_EQ(destination.getRandVal(), 84.0);
    ASSERT_VALUE_EQ(destination.getSortKey(), Value(1));
    ASSERT_TRUE(destination.isSingleElementKey());
    ASSERT_EQ(destination.getGeoNearDistance(), 3.2);
    ASSERT_FALSE(destination.hasGeoNearPoint());
    ASSERT_FALSE(destination.hasSearchScore());
    ASSERT_FALSE(destination.hasSearchHighlights());
    ASSERT_FALSE(destination.hasIndexKey());
}

TEST(DocumentMetadataFieldsTest, CopyFromCopiesAllMetadataThatSourceHas) {
    DocumentMetadataFields source;
    source.setTextScore(9.9);
    source.setRandVal(42.0);
    source.setSortKey(Value(1), /* isSingleElementKey = */ true);
    source.setGeoNearDistance(3.2);

    DocumentMetadataFields destination;
    destination.setTextScore(12.3);
    destination.setRandVal(84.0);

    destination.copyFrom(source);

    ASSERT_EQ(destination.getTextScore(), 9.9);
    ASSERT_EQ(destination.getRandVal(), 42.0);
    ASSERT_VALUE_EQ(destination.getSortKey(), Value(1));
    ASSERT_TRUE(destination.isSingleElementKey());
    ASSERT_EQ(destination.getGeoNearDistance(), 3.2);
    ASSERT_FALSE(destination.hasGeoNearPoint());
    ASSERT_FALSE(destination.hasSearchScore());
    ASSERT_FALSE(destination.hasSearchHighlights());
    ASSERT_FALSE(destination.hasIndexKey());
}

}  // namespace mongo
