/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/ftdc/metadata_compressor.h"

#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/unittest.h"

namespace mongo {


const Date_t kDate = Date_t::now();
const std::vector<BSONObj> kField1Elements = {
    BSON("logLevel" << 2),
    BSON("auditSomething" << false),
    BSON("tlsMode" << "disabled"),
    BSON("featureFlagToaster" << BSON("value" << true << "version"
                                              << "4.4")),
    BSON("authMechs" << BSON_ARRAY("scram" << "x509"))};
const std::vector<BSONObj> kField2Elements = {
    BSON("enableSomething" << true), BSON("delaySeconds" << 400), BSON("someName" << "foobar")};

BSONObj buildSample(
    const boost::optional<const std::vector<BSONObj>&>& field1Elements = boost::none,
    const boost::optional<const std::vector<BSONObj>&>& field2Elements = boost::none) {
    BSONObjBuilder sampleBuilder;
    sampleBuilder.appendDate("start", kDate);

    if (field1Elements) {
        BSONObjBuilder subBob = sampleBuilder.subobjStart("field1");
        subBob.appendDate("start", kDate);
        for (auto& obj : field1Elements.value()) {
            subBob.appendElements(obj);
        }
        subBob.appendDate("end", kDate);
    }
    if (field2Elements) {
        BSONObjBuilder subBob = sampleBuilder.subobjStart("field2");
        subBob.appendDate("start", kDate);
        for (auto& obj : field2Elements.value()) {
            subBob.appendElements(obj);
        }
        subBob.appendDate("end", kDate);
    }
    sampleBuilder.appendDate("end", kDate);
    return sampleBuilder.obj();
}

void alterOneFieldValueAndAddSample(
    FTDCMetadataCompressor& compressor,
    std::vector<BSONObj>& field1Elements,
    size_t idx,
    const boost::optional<const std::vector<BSONObj>&>& field2Elements = boost::none) {

    BSONObj deltaElement;

    if (field1Elements.at(idx).firstElementType() == BSONType::string) {
        deltaElement = BSON(field1Elements.at(idx).firstElementFieldNameStringData() << 42);
    } else {
        deltaElement =
            BSON(field1Elements.at(idx).firstElementFieldNameStringData() << "new_value");
    }
    field1Elements.at(idx) = deltaElement;

    std::vector<BSONObj> changedElements{deltaElement};
    auto deltaCountExpected = compressor.getDeltaCount() + 1;
    auto deltaDocExpected = buildSample(changedElements);

    auto sample = buildSample(field1Elements, field2Elements);
    auto result = compressor.addSample(sample);

    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), deltaDocExpected);
    ASSERT_EQ(compressor.getDeltaCount(), deltaCountExpected);
}

// Tests addSample returns just the delta document on non-schema breaking changes.
TEST(FTDCMetadataCompressor, TestAddSample_BasicDeltas) {
    FTDCMetadataCompressor compressor;

    auto field1Elements = kField1Elements;
    auto field2Elements = kField2Elements;

    // Set the reference document
    auto sample = buildSample(field1Elements, field2Elements);
    auto result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    // Alter one field value in field1, then call addSample
    std::vector<BSONObj> changedElements;
    std::vector<BSONObj> field2ChangedElements;

    auto deltaElement = BSON(field1Elements.at(0).firstElementFieldNameStringData() << "new_value");
    field1Elements.at(0) = deltaElement;
    auto sampleDoc = buildSample(field1Elements, field2Elements);
    changedElements.push_back(deltaElement);
    auto deltaDoc = buildSample(changedElements);
    result = compressor.addSample(sampleDoc);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), deltaDoc);
    ASSERT_EQ(compressor.getDeltaCount(), 1);

    // Alter multiple field values in field1, then call addSample
    changedElements.clear();
    for (size_t i = 1; i < field1Elements.size(); i++) {
        deltaElement = BSON(field1Elements.at(i).firstElementFieldNameStringData() << "new_value");
        field1Elements.at(i) = deltaElement;
        changedElements.push_back(deltaElement);
    }
    sampleDoc = buildSample(field1Elements, field2Elements);
    deltaDoc = buildSample(changedElements);
    result = compressor.addSample(sampleDoc);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), deltaDoc);
    ASSERT_EQ(compressor.getDeltaCount(), 2);

    // Test no changes
    for (size_t i = 0; i < 2; i++) {
        result = compressor.addSample(sampleDoc);
        ASSERT(!result);
    }

    // Alter multiple field values in field1 and field2, then call addSample
    changedElements.clear();
    for (size_t i = 1; i < field1Elements.size(); i++) {
        deltaElement = BSON(field1Elements.at(i).firstElementFieldNameStringData() << 42);
        field1Elements.at(i) = deltaElement;
        changedElements.push_back(deltaElement);
    }
    for (size_t i = 1; i < field2Elements.size(); i++) {
        deltaElement = BSON(field2Elements.at(i).firstElementFieldNameStringData() << "new_value");
        field2Elements.at(i) = deltaElement;
        field2ChangedElements.push_back(deltaElement);
    }
    sampleDoc = buildSample(field1Elements, field2Elements);
    deltaDoc = buildSample(changedElements, field2ChangedElements);
    result = compressor.addSample(sampleDoc);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), deltaDoc);
    ASSERT_EQ(compressor.getDeltaCount(), 3);
}

// Tests the sample document at some snapshot, i, can be reconstructed from the initial
// reference and the delta documents.
TEST(FTDCMetadataCompressor, TestReconstruction) {
    namespace mmb = mongo::mutablebson;

    FTDCMetadataCompressor compressor;

    auto field1Elements = kField1Elements;
    auto field2Elements = kField2Elements;
    std::vector<BSONObj> samples;
    std::vector<BSONObj> deltas;

    // Build the list of sample documents to test against.
    // -- initial ref doc
    samples.push_back(buildSample(field1Elements, field2Elements));

    // -- first delta
    field1Elements.at(0) = BSON(field1Elements.at(0).firstElementFieldNameStringData() << "newval");
    samples.push_back(buildSample(field1Elements, field2Elements));

    // -- second delta
    field2Elements.at(0) = BSON(field2Elements.at(0).firstElementFieldNameStringData() << "foo");
    samples.push_back(buildSample(field1Elements, field2Elements));

    // -- third delta
    field1Elements.at(1) =
        BSON(field1Elements.at(1).firstElementFieldNameStringData() << BSON("foo" << "bar"));
    field2Elements.at(0) = BSON(field2Elements.at(0).firstElementFieldNameStringData() << true);
    samples.push_back(buildSample(field1Elements, field2Elements));

    // Build the list of delta documents
    for (auto& sample : samples) {
        auto result = compressor.addSample(sample);
        ASSERT(result);
        deltas.push_back(result.value());
    }
    ASSERT_EQ(compressor.getDeltaCount(), samples.size() - 1);

    // Do the reconstruction over a mutable BSON initially set to the first delta document
    // (i.e. the reference doc)
    mmb::Document doc(deltas.front());

    for (size_t i = 1; i < deltas.size(); i++) {
        BSONObjIterator deltaItr(deltas.at(i));

        while (deltaItr.more()) {
            auto deltaElement = deltaItr.next();

            auto currentElement =
                mmb::findFirstChildNamed(doc.root(), deltaElement.fieldNameStringData());
            ASSERT(currentElement.ok());

            if (deltaElement.type() != BSONType::object) {
                ASSERT_FALSE(currentElement.isType(BSONType::object));
                ASSERT_OK(currentElement.setValueBSONElement(deltaElement));
                continue;
            }

            // Do reconstruction for next level of objects
            ASSERT(currentElement.isType(BSONType::object));

            BSONObjIterator deltaLvl2Itr(deltaElement.Obj());
            while (deltaLvl2Itr.more()) {
                auto deltaLvl2Element = deltaLvl2Itr.next();

                auto currentLvl2Element = mmb::findFirstChildNamed(
                    currentElement, deltaLvl2Element.fieldNameStringData());
                ASSERT(currentLvl2Element.ok());
                ASSERT_OK(currentLvl2Element.setValueBSONElement(deltaLvl2Element));
            }
        }

        // Assert the reconstructed snapshot matches the sample at time i
        auto reconstructed = doc.getObject();
        ASSERT_BSONOBJ_EQ(samples.at(i), reconstructed);
    }
}

// Tests addSample resets delta tracking if a top-level element is added/removed/reordered/renamed.
TEST(FTDCMetadataCompressor, TestAddSample_Level1SchemaChange) {
    FTDCMetadataCompressor compressor;

    auto field1Elements = kField1Elements;

    // Set the reference document
    auto sample = buildSample(kField1Elements, kField2Elements);
    auto result = compressor.addSample(sample);
    ASSERT(result);

    // Alter one field value; assert delta count increments
    alterOneFieldValueAndAddSample(compressor, field1Elements, 0, kField2Elements);

    // Remove "field2".
    // Assert delta count resets to 0 and the delta doc is the same as the sample.
    sample = buildSample(field1Elements);
    result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    // Alter one field value to increment delta count
    alterOneFieldValueAndAddSample(compressor, field1Elements, 0, boost::none);

    // Add back "field2". Assert delta resets.
    sample = buildSample(field1Elements, kField2Elements);
    result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    // Alter one field value to increment delta count
    alterOneFieldValueAndAddSample(compressor, field1Elements, 0, kField2Elements);

    // Swap subobjects of "field1" and "field2". Assert delta resets.
    sample = buildSample(kField2Elements, field1Elements);
    result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);
}

// Tests addSample resets delta tracking if a level 2 element is added/removed/reordered/renamed.
TEST(FTDCMetadataCompressor, TestAddSample_Level2SchemaChange) {
    FTDCMetadataCompressor compressor;

    auto field1Elements = kField1Elements;

    // Set the reference document
    auto sample = buildSample(kField1Elements, kField2Elements);
    auto result = compressor.addSample(sample);
    ASSERT(result);

    // Alter one field value; assert delta count increments
    alterOneFieldValueAndAddSample(compressor, field1Elements, 0, kField2Elements);

    // Remove one field value.
    // Assert delta count resets to 0 and the delta doc is the same as the sample.
    BSONObj removedElement = field1Elements.back();
    field1Elements.pop_back();
    sample = buildSample(field1Elements, kField2Elements);
    result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    // Alter one field value to increment delta count
    alterOneFieldValueAndAddSample(compressor, field1Elements, 0, kField2Elements);

    // Add one field value. Assert delta resets.
    field1Elements.push_back(removedElement);
    sample = buildSample(field1Elements, kField2Elements);
    result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    // Alter one field value to increment delta count
    alterOneFieldValueAndAddSample(compressor, field1Elements, 0, kField2Elements);

    // Rename one field. Assert delta resets.
    field1Elements.at(1) = BSON("renamedField" << 42);
    sample = buildSample(field1Elements, kField2Elements);
    result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);
}

// Tests addSample where a subobject only contains the "start" and "end" timestamps
TEST(FTDCMetadataCompressor, TestAddSample_StartAndEndOnly) {
    FTDCMetadataCompressor compressor;

    std::vector<BSONObj> field1Elements, field2Elements;

    // Build sample where field1 & field2 only have "start" and "end" fields.
    auto sample = buildSample(field1Elements, field2Elements);

    // Set the reference document
    auto result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    // Second addSample returns none
    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);
}
}  // namespace mongo
