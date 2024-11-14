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

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ftdc/metadata_compressor.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {

#define TEST_BEGIN auto runTest = [this](bool multiservice) { \
    std::cout << "Running " << _testInfo.testName() <<" with multiservice=" << \
    multiservice << std::endl

#define TEST_END                          \
    }                                     \
    ;                                     \
    do {                                  \
        for (auto mode : {true, false}) { \
            runTest(mode);                \
        }                                 \
    } while (0)

const Date_t kDate = Date_t::now();
const std::vector<BSONObj> kField1Elements = {BSON("logLevel" << 2),
                                              BSON("auditSomething" << false),
                                              BSON("tlsMode"
                                                   << "disabled"),
                                              BSON("featureFlagToaster"
                                                   << BSON("value" << true << "version"
                                                                   << "4.4")),
                                              BSON("authMechs" << BSON_ARRAY("scram"
                                                                             << "x509"))};
const std::vector<BSONObj> kField2Elements = {BSON("enableSomething" << true),
                                              BSON("delaySeconds" << 400),
                                              BSON("someName"
                                                   << "foobar")};

BSONObj buildSample(
    bool multiservice,
    const boost::optional<const std::vector<BSONObj>&>& field1Elements = boost::none,
    const boost::optional<const std::vector<BSONObj>&>& field2Elements = boost::none) {
    BSONObjBuilder sampleBuilder;
    std::unique_ptr<BSONObjBuilder> serviceBuilder;
    auto* endLevelBuilder = &sampleBuilder;

    sampleBuilder.appendDate("start", kDate);

    if (multiservice) {
        serviceBuilder = std::make_unique<BSONObjBuilder>(sampleBuilder.subobjStart("common"));
        serviceBuilder->appendDate("start", kDate);
        endLevelBuilder = serviceBuilder.get();
    }

    if (field1Elements) {
        BSONObjBuilder subBob = endLevelBuilder->subobjStart("field1");
        subBob.appendDate("start", kDate);
        for (auto& obj : field1Elements.value()) {
            subBob.appendElements(obj);
        }
        subBob.appendDate("end", kDate);
    }
    if (field2Elements) {
        BSONObjBuilder subBob = endLevelBuilder->subobjStart("field2");
        subBob.appendDate("start", kDate);
        for (auto& obj : field2Elements.value()) {
            subBob.appendElements(obj);
        }
        subBob.appendDate("end", kDate);
    }

    if (multiservice) {
        serviceBuilder->appendDate("end", kDate);
    }
    serviceBuilder.reset(nullptr);
    sampleBuilder.appendDate("end", kDate);
    return sampleBuilder.obj();
}

void alterOneFieldValueAndAddSample(
    FTDCMetadataCompressor& compressor,
    std::vector<BSONObj>& field1Elements,
    size_t idx,
    const boost::optional<const std::vector<BSONObj>&>& field2Elements = boost::none) {

    BSONObj deltaElement;

    if (field1Elements.at(idx).firstElementType() == BSONType::String) {
        deltaElement = BSON(field1Elements.at(idx).firstElementFieldNameStringData() << 42);
    } else {
        deltaElement =
            BSON(field1Elements.at(idx).firstElementFieldNameStringData() << "new_value");
    }
    field1Elements.at(idx) = deltaElement;

    std::vector<BSONObj> changedElements{deltaElement};
    auto deltaCountExpected = compressor.getDeltaCount() + 1;
    auto deltaDocExpected = buildSample(compressor.isMultiService(), changedElements);

    auto sample = buildSample(compressor.isMultiService(), field1Elements, field2Elements);
    auto result = compressor.addSample(sample);

    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), deltaDocExpected);
    ASSERT_EQ(compressor.getDeltaCount(), deltaCountExpected);
}

// Tests addSample returns just the delta document on non-schema breaking changes.
TEST(FTDCMetadataCompressorTest, TestAddSample_BasicDeltas) {
    TEST_BEGIN;

    FTDCMetadataCompressor compressor(UseMultiServiceSchema{multiservice});

    auto field1Elements = kField1Elements;
    auto field2Elements = kField2Elements;

    // Set the reference document
    auto sample = buildSample(multiservice, field1Elements, field2Elements);
    auto result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    // Alter one field value in field1, then call addSample
    std::vector<BSONObj> changedElements;
    std::vector<BSONObj> field2ChangedElements;

    auto deltaElement = BSON(field1Elements.at(0).firstElementFieldNameStringData() << "new_value");
    field1Elements.at(0) = deltaElement;
    auto sampleDoc = buildSample(multiservice, field1Elements, field2Elements);
    changedElements.push_back(deltaElement);
    auto deltaDoc = buildSample(multiservice, changedElements);
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
    sampleDoc = buildSample(multiservice, field1Elements, field2Elements);
    deltaDoc = buildSample(multiservice, changedElements);
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
    sampleDoc = buildSample(multiservice, field1Elements, field2Elements);
    deltaDoc = buildSample(multiservice, changedElements, field2ChangedElements);
    result = compressor.addSample(sampleDoc);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), deltaDoc);
    ASSERT_EQ(compressor.getDeltaCount(), 3);

    TEST_END;
}

// Tests that services with unchanged periodic metadata are not included in the delta document.
TEST(FTDCMetadataCompressorTest, TestAddSample_MultiServiceDelta) {
    auto cmd1 = BSON("cmd" << BSON("foo" << 1));
    auto cmd2 = BSON("cmd" << BSON("foo" << 2));
    auto sample1 = BSON("start" << kDate << "common" << cmd1 << "router" << cmd1 << "end" << kDate);
    auto sample2 = BSON("start" << kDate << "common" << cmd1 << "router" << cmd2 << "end" << kDate);
    auto sample3 = BSON("start" << kDate << "common" << cmd2 << "router" << cmd2 << "end" << kDate);
    auto delta1 = BSON("start" << kDate << "router" << cmd2 << "end" << kDate);
    auto delta2 = BSON("start" << kDate << "common" << cmd2 << "end" << kDate);

    FTDCMetadataCompressor compressor(UseMultiServiceSchema{true});
    auto result = compressor.addSample(sample1);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample1);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    ASSERT_FALSE(compressor.addSample(sample1).has_value());

    result = compressor.addSample(sample2);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), delta1);
    ASSERT_EQ(compressor.getDeltaCount(), 1);

    ASSERT_FALSE(compressor.addSample(sample2).has_value());

    result = compressor.addSample(sample3);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), delta2);
    ASSERT_EQ(compressor.getDeltaCount(), 2);
}

// Tests the sample document at some snapshot, i, can be reconstructed from the initial
// reference and the delta documents.
TEST(FTDCMetadataCompressorTest, TestReconstruction) {
    TEST_BEGIN;

    namespace mmb = mongo::mutablebson;

    FTDCMetadataCompressor compressor(UseMultiServiceSchema{multiservice});

    auto field1Elements = kField1Elements;
    auto field2Elements = kField2Elements;
    std::vector<BSONObj> samples;
    std::vector<BSONObj> deltas;

    // Build the list of sample documents to test against.
    // -- initial ref doc
    samples.push_back(buildSample(multiservice, field1Elements, field2Elements));

    // -- first delta
    field1Elements.at(0) = BSON(field1Elements.at(0).firstElementFieldNameStringData() << "newval");
    samples.push_back(buildSample(multiservice, field1Elements, field2Elements));

    // -- second delta
    field2Elements.at(0) = BSON(field2Elements.at(0).firstElementFieldNameStringData() << "foo");
    samples.push_back(buildSample(multiservice, field1Elements, field2Elements));

    // -- third delta
    field1Elements.at(1) =
        BSON(field1Elements.at(1).firstElementFieldNameStringData() << BSON("foo"
                                                                            << "bar"));
    field2Elements.at(0) = BSON(field2Elements.at(0).firstElementFieldNameStringData() << true);
    samples.push_back(buildSample(multiservice, field1Elements, field2Elements));

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

            if (deltaElement.type() != BSONType::Object) {
                ASSERT_FALSE(currentElement.isType(BSONType::Object));
                ASSERT_OK(currentElement.setValueBSONElement(deltaElement));
                continue;
            }

            // Do reconstruction for next level of objects
            ASSERT(currentElement.isType(BSONType::Object));

            BSONObjIterator deltaLvl2Itr(deltaElement.Obj());
            while (deltaLvl2Itr.more()) {
                auto deltaLvl2Element = deltaLvl2Itr.next();

                auto currentLvl2Element = mmb::findFirstChildNamed(
                    currentElement, deltaLvl2Element.fieldNameStringData());
                ASSERT(currentLvl2Element.ok());

                if (multiservice) {
                    // go one more level deeper
                    if (deltaLvl2Element.type() != BSONType::Object) {
                        ASSERT_FALSE(currentLvl2Element.isType(BSONType::Object));
                        ASSERT_OK(currentLvl2Element.setValueBSONElement(deltaLvl2Element));
                        continue;
                    }

                    ASSERT(currentLvl2Element.isType(BSONType::Object));
                    BSONObjIterator deltaLvl3Itr(deltaLvl2Element.Obj());
                    while (deltaLvl3Itr.more()) {
                        auto deltaLvl3Element = deltaLvl3Itr.next();
                        auto currentLvl3Element = mmb::findFirstChildNamed(
                            currentLvl2Element, deltaLvl3Element.fieldNameStringData());
                        ASSERT(currentLvl3Element.ok());
                        ASSERT_OK(currentLvl3Element.setValueBSONElement(deltaLvl3Element));
                    }
                } else {
                    ASSERT_OK(currentLvl2Element.setValueBSONElement(deltaLvl2Element));
                }
            }
        }

        // Assert the reconstructed snapshot matches the sample at time i
        auto reconstructed = doc.getObject();
        ASSERT_BSONOBJ_EQ(samples.at(i), reconstructed);
    }

    TEST_END;
}

// Tests addSample resets delta tracking if a top-level element is added/removed/reordered/renamed.
TEST(FTDCMetadataCompressorTest, TestAddSample_Level1SchemaChange) {
    TEST_BEGIN;

    FTDCMetadataCompressor compressor(UseMultiServiceSchema{multiservice});

    auto field1Elements = kField1Elements;

    // Set the reference document
    auto sample = buildSample(multiservice, kField1Elements, kField2Elements);
    auto result = compressor.addSample(sample);
    ASSERT(result);

    // Alter one field value; assert delta count increments
    alterOneFieldValueAndAddSample(compressor, field1Elements, 0, kField2Elements);

    // Remove "field2".
    // Assert delta count resets to 0 and the delta doc is the same as the sample.
    sample = buildSample(multiservice, field1Elements);
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
    sample = buildSample(multiservice, field1Elements, kField2Elements);
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
    sample = buildSample(multiservice, kField2Elements, field1Elements);
    result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    TEST_END;
}

// Tests addSample resets delta tracking if a level 2 element is added/removed/reordered/renamed.
TEST(FTDCMetadataCompressorTest, TestAddSample_Level2SchemaChange) {
    TEST_BEGIN;

    FTDCMetadataCompressor compressor(UseMultiServiceSchema{multiservice});

    auto field1Elements = kField1Elements;

    // Set the reference document
    auto sample = buildSample(multiservice, kField1Elements, kField2Elements);
    auto result = compressor.addSample(sample);
    ASSERT(result);

    // Alter one field value; assert delta count increments
    alterOneFieldValueAndAddSample(compressor, field1Elements, 0, kField2Elements);

    // Remove one field value.
    // Assert delta count resets to 0 and the delta doc is the same as the sample.
    BSONObj removedElement = field1Elements.back();
    field1Elements.pop_back();
    sample = buildSample(multiservice, field1Elements, kField2Elements);
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
    sample = buildSample(multiservice, field1Elements, kField2Elements);
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
    sample = buildSample(multiservice, field1Elements, kField2Elements);
    result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    TEST_END;
}

// Tests addSample where a subobject only contains the "start" and "end" timestamps
TEST(FTDCMetadataCompressorTest, TestAddSample_StartAndEndOnly) {
    TEST_BEGIN;

    FTDCMetadataCompressor compressor(UseMultiServiceSchema{multiservice});

    std::vector<BSONObj> field1Elements, field2Elements;

    // Build sample where field1 & field2 only have "start" and "end" fields.
    auto sample = buildSample(multiservice, field1Elements, field2Elements);

    // Set the reference document
    auto result = compressor.addSample(sample);
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result.value(), sample);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    // Second addSample returns none
    result = compressor.addSample(sample);
    ASSERT_FALSE(result);
    ASSERT_EQ(compressor.getDeltaCount(), 0);

    TEST_END;
}

#undef TEST_BEGIN
#undef TEST_END
}  // namespace mongo
