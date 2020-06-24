/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <algorithm>
#include <random>

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::doc_diff {
namespace {


// We use the same seed and random number generator throughout the tests to be able to reproduce a
// failure easily.
auto getSeed() {
    const static auto seed = std::random_device()();
    return seed;
}
PseudoRandom* getRNG() {
    static auto rng = PseudoRandom(getSeed());
    return &rng;
}

BSONObj createObjWithLargePrefix(StringData suffix, int len = 100) {
    const static auto largeObj = BSON("prefixLargeField" << std::string(len, 'a'));
    return largeObj.addFields(fromjson(suffix.rawData()));
}

std::string getFieldName(int level, int fieldNum) {
    return str::stream() << "f" << level << fieldNum;
}

Value getScalarFieldValue() {
    auto rng = getRNG();
    switch (rng->nextInt64(10)) {
        case 0:
            return Value("val"_sd);
        case 1:
            return Value(BSONNULL);
        case 2:
            return Value(-1LL);
        case 3:
            return Value(0);
        case 4:
            return Value(1.10);
        case 5:
            return Value(false);
        case 6:
            return Value(BSONRegEx("p"));
        case 7:
            return Value(Date_t());
        case 8:
            return Value(UUID::gen());
        case 9:
            return Value(BSONBinData("asdf", 4, BinDataGeneral));
        default:
            MONGO_UNREACHABLE;
    }
}

std::vector<BSONObj> getDocumentsRepo() {
    const static std::vector<BSONObj> documents = {
        createObjWithLargePrefix("{}"),  // Empty object.
        // Simple subobjects.
        createObjWithLargePrefix("{field1: {level1Field1: {level2Field1: 'val'}}, field2: "
                                 "{level1Field1: {}}, field3: {level1Field3: 'va'}}"),
        createObjWithLargePrefix(
            "{field1: {level1Field1: {level1Field1: 1}}, field2: {level1Field1: {}}, field3: "
            "{level1Field3: 'va2'}, field4: ['arrayVal1']}"),

        // Simple arrays.
        createObjWithLargePrefix(
            "{field1: ['arrayVal1', 'arrayVal2'], field2: ['arrayVal1', 'arrayVal2', 'arrayVal3' ],"
            "field4: {}, field3: null}"),
        createObjWithLargePrefix("{field2: ['arrayVal1', ['subArrayVal1', 'subArrayVal2', "
                                 "'subArrayVal3'], 'val'], field1: ['arrayVal1', 'arrayVal2'], "
                                 "field3: ['arrayVal1', 'arrayVal2']}"),
        createObjWithLargePrefix("{field2: ['arrayVal1', ['subArrayVal1','subArrayVal2', "
                                 "'subArrayVal', 'val' ], 'val'], field1: ['arrayVal1', "
                                 "'arrayVal2'], field3: ['arrayVal1', 'arrayVal2']}"),

        // Array and sub-object combination.
        createObjWithLargePrefix("{field1: {level1Field1: [{level1Field1: [1]}]}, field0: "
                                 "{level1Field1: {}}, field3: {level1Field3: 'val2'}}"),
        createObjWithLargePrefix("{field1: {level1Field1: [{level1Field1: [1, 2]}]}, field2: "
                                 "{level1Field1: {}}, field3: {level1Field3: ['val']}}"),
        createObjWithLargePrefix(
            "{field3: {level1Field1: [{level1Field1: [1, 2]}]}, field1: "
            "{level1Field1: {}}, field2: {level1Field3: ['val']}}, field4: [[]]"),

        // Unrelated documents.
        createObjWithLargePrefix(
            "{newField1: {level1Field1: [{level1Field1: [1, 2]}]}, newField2: {level1Field1: {}}, "
            "newField4: {level1Field3: ['val']}}, newField3: [[]]"),
        createObjWithLargePrefix(
            "{newField2: {level1Field1: {}}, newField1: {level1Field1: [{level1Field1: [1, 2]}]},"
            "newField4: {level1Field3: ['val']}}, newField3: [[]]"),
        createObjWithLargePrefix(
            "{newField3: {level1Field1: [{level1Field1: [1, 2]}]}, newField2: {level1Field1: {}}, "
            "newField4: {level1Field3: ['val']}}, newField1: [[]]"),
    };
    return documents;
}

void runTest(std::vector<BSONObj> documents, size_t numSimulations) {
    // Shuffle them into a random order
    auto rng = getRNG();
    LOGV2(4785301, "Seed used for the test ", "seed"_attr = getSeed());
    for (size_t simulation = 0; simulation < numSimulations; ++simulation) {
        std::shuffle(documents.begin(), documents.end(), rng->urbg());

        auto preDoc = documents[0];
        for (size_t i = 1; i < documents.size(); ++i) {
            const auto diff = computeDiff(preDoc, documents[i]);

            ASSERT(diff);
            const auto postObj = applyDiff(preDoc, *diff);
            ASSERT_BSONOBJ_BINARY_EQ(documents[i], postObj);

            // Applying the diff the second time also generates the same object.
            ASSERT_BSONOBJ_BINARY_EQ(postObj, applyDiff(postObj, *diff));

            preDoc = documents[i];
        }
    }
}
TEST(DocumentDiffTest, PredefinedDocumentsTest) {
    runTest(getDocumentsRepo(), 10);
}

BSONObj generateDoc(MutableDocument* doc, int depthLevel) {
    // Append a large field at each level so that the likelihood of generating a sub-diff is high.
    doc->reset(createObjWithLargePrefix("{}", 100), true);

    // Reduce the probabilty of generated nested objects as we go deeper. After depth level 6, we
    // should not be generating anymore nested objects.
    const double subObjProbability = 0.3 - (depthLevel * 0.05);
    const double subArrayProbability = 0.2 - (depthLevel * 0.05);

    auto rng = getRNG();
    const int numFields = (5 - depthLevel) + rng->nextInt32(4);
    for (int fieldNum = 0; fieldNum < numFields; ++fieldNum) {
        const auto fieldName = getFieldName(depthLevel, fieldNum);
        auto num = rng->nextCanonicalDouble();
        if (num <= subObjProbability) {
            MutableDocument subDoc;
            doc->addField(fieldName, Value(generateDoc(&subDoc, depthLevel + 1)));
        } else if (num <= (subObjProbability + subArrayProbability)) {
            std::uniform_int_distribution<int> arrayLengthGen(0, 10);
            const auto length = arrayLengthGen(rng->urbg());
            std::vector<Value> values;

            // Make sure that only one array element is a document to avoid exponentially bloating
            // up the document.
            if (length) {
                MutableDocument subDoc;
                values.push_back(Value(generateDoc(&subDoc, depthLevel + 1)));
            }
            for (auto i = 1; i < length; i++) {
                values.push_back(getScalarFieldValue());
            }
            doc->addField(fieldName, Value(values));
        } else {
            doc->addField(fieldName, getScalarFieldValue());
        }
    }
    return doc->freeze().toBson();
}

TEST(DocumentDiffTest, RandomizedDocumentBuilderTest) {
    const auto numDocs = 20;
    std::vector<BSONObj> documents(numDocs);
    for (int i = 0; i < numDocs; ++i) {
        MutableDocument doc;
        documents[i] = generateDoc(&doc, 0);
    }
    runTest(std::move(documents), 10);
}

}  // namespace
}  // namespace mongo::doc_diff
