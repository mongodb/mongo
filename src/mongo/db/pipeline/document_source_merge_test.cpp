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

#include "mongo/platform/basic.h"

#include "mongo/unittest/bson_test_util.h"
#include <boost/intrusive_ptr.hpp>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/process_interface/non_shardsvr_process_interface.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;

constexpr StringData kWhenMatchedModeFieldName = DocumentSourceMergeSpec::kWhenMatchedFieldName;
constexpr StringData kWhenNotMatchedModeFieldName =
    DocumentSourceMergeSpec::kWhenNotMatchedFieldName;
constexpr StringData kIntoFieldName = DocumentSourceMergeSpec::kTargetNssFieldName;
constexpr StringData kOnFieldName = DocumentSourceMergeSpec::kOnFieldName;
const StringData kDefaultWhenMatchedMode =
    MergeWhenMatchedMode_serializer(MergeWhenMatchedModeEnum::kMerge);
const StringData kDefaultWhenNotMatchedMode =
    MergeWhenNotMatchedMode_serializer(MergeWhenNotMatchedModeEnum::kInsert);

/**
 * For the purpsoses of this test, assume every collection is unsharded. Stages may ask this during
 * setup. For example, to compute its constraints, the $merge stage needs to know if the output
 * collection is sharded.
 */
class MongoProcessInterfaceForTest : public StubMongoProcessInterface {
public:
    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return false;
    }

    /**
     * For the purposes of these tests, assume each collection is unsharded and has a document key
     * of just "_id".
     */
    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext* opCtx, const NamespaceString& nss) const override {
        return {"_id"};
    }

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString&,
                                      ChunkVersion) const override {
        return;  // Assume it always matches for our tests here.
    }
};

class DocumentSourceMergeTest : public AggregationContextFixture {
public:
    DocumentSourceMergeTest() : AggregationContextFixture() {
        getExpCtx()->mongoProcessInterface = std::make_shared<MongoProcessInterfaceForTest>();
    }

    intrusive_ptr<DocumentSourceMerge> createMergeStage(BSONObj spec) {
        auto specElem = spec.firstElement();
        intrusive_ptr<DocumentSourceMerge> mergeStage = dynamic_cast<DocumentSourceMerge*>(
            DocumentSourceMerge::createFromBson(specElem, getExpCtx()).get());
        ASSERT_TRUE(mergeStage);
        return mergeStage;
    }
};

TEST_F(DocumentSourceMergeTest, CorrectlyParsesIfMergeSpecIsString) {
    const auto& defaultDb = getExpCtx()->ns.db();
    const std::string targetColl = "target_collection";
    auto spec = BSON("$merge" << targetColl);
    auto mergeStage = createMergeStage(spec);
    ASSERT(mergeStage);
    ASSERT_EQ(mergeStage->getOutputNs().db(), defaultDb);
    ASSERT_EQ(mergeStage->getOutputNs().coll(), targetColl);
}

TEST_F(DocumentSourceMergeTest, CorrectlyParsesIfIntoIsString) {
    const auto& defaultDb = getExpCtx()->ns.db();
    const std::string targetColl = "target_collection";
    auto spec = BSON("$merge" << BSON("into" << targetColl));
    auto mergeStage = createMergeStage(spec);
    ASSERT(mergeStage);
    ASSERT_EQ(mergeStage->getOutputNs().db(), defaultDb);
    ASSERT_EQ(mergeStage->getOutputNs().coll(), targetColl);
}

TEST_F(DocumentSourceMergeTest, CorrectlyParsesIfIntoIsObject) {
    const auto& defaultDb = getExpCtx()->ns.db();
    const std::string targetDb = "target_db";
    const std::string targetColl = "target_collection";
    auto spec = BSON("$merge" << BSON("into" << BSON("coll" << targetColl)));
    auto mergeStage = createMergeStage(spec);
    ASSERT(mergeStage);
    ASSERT_EQ(mergeStage->getOutputNs().db(), defaultDb);
    ASSERT_EQ(mergeStage->getOutputNs().coll(), targetColl);

    spec = BSON("$merge" << BSON("into" << BSON("db" << targetDb << "coll" << targetColl)));
    mergeStage = createMergeStage(spec);
    ASSERT(mergeStage);
    ASSERT_EQ(mergeStage->getOutputNs().db(), targetDb);
    ASSERT_EQ(mergeStage->getOutputNs().coll(), targetColl);
}

TEST_F(DocumentSourceMergeTest, CorrectlyParsesIfWhenMatchedIsStringOrArray) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "whenMatched"
                                      << "merge"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched" << BSONArray()));
    ASSERT(createMergeStage(spec));
}

TEST_F(DocumentSourceMergeTest, CorrectlyParsesIfTargetAndAggregationNamespacesAreSame) {
    const auto targetNsSameAsAggregationNs = getExpCtx()->ns;
    const auto targetColl = targetNsSameAsAggregationNs.coll();
    const auto targetDb = targetNsSameAsAggregationNs.db();

    auto spec = BSON("$merge" << BSON("into" << BSON("coll" << targetColl << "db" << targetDb)));
    ASSERT(createMergeStage(spec));
}

TEST_F(DocumentSourceMergeTest, FailsToParseIncorrectMergeSpecType) {
    auto spec = BSON("$merge" << 1);
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51182);

    spec = BSON("$merge" << BSONArray());
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51182);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfMergeSpecObjectWithoutInto) {
    auto spec = BSON("$merge" << BSONObj());
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 40414);

    spec = BSON("$merge" << BSON("whenMatched"
                                 << "replace"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 40414);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsNotStringAndNotObject) {
    auto spec = BSON("$merge" << BSON("into" << 1));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51178);

    spec = BSON("$merge" << BSON("into" << BSON_ARRAY(1 << 2)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51178);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsNullish) {
    auto spec = BSON("$merge" << BSON("into" << BSONNULL));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51178);

    spec = BSON("$merge" << BSON("into" << BSONUndefined));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51178);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsAnEmptyString) {
    auto spec = BSON("$merge" << BSON("into"
                                      << ""));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 5786800);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsObjectWithInvalidFields) {
    auto spec = BSON("$merge" << BSON("into" << BSON("a"
                                                     << "b")));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 40415);

    spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                << "target_collection"
                                                << "a"
                                                << "b")));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 40415);

    spec = BSON("$merge" << BSON("into" << BSON("db"
                                                << "target_db"
                                                << "a"
                                                << "b")));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 40415);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsObjectWithEmptyCollectionName) {
    auto spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                     << "")));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 5786801);

    spec = BSON("$merge" << BSON("into" << BSONObj()));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 5786801);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsObjectWithNullishCollection) {
    auto spec = BSON("$merge" << BSON("into" << BSON("coll" << BSONNULL)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 5786801);

    spec = BSON("$merge" << BSON("into" << BSON("coll" << BSONUndefined)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 5786801);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsNotAValidUserCollection) {
    auto spec = BSON("$merge"
                     << ".test.");
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::InvalidNamespace);

    spec = BSON("$merge" << BSON("into"
                                 << ".test."));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::InvalidNamespace);

    spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                << ".test.")));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfDbIsNotString) {
    auto spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                     << "target_collection"
                                                     << "db" << true)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                << "target_collection"
                                                << "db" << BSONArray())));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                << "target_collection"
                                                << "db"
                                                << BSON(""
                                                        << "test"))));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfCollIsNotString) {
    auto spec = BSON("$merge" << BSON("into" << BSON("db"
                                                     << "target_db"
                                                     << "coll" << true)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$merge" << BSON("into" << BSON("db"
                                                << "target_db"
                                                << "coll" << BSONArray())));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$merge" << BSON("into" << BSON("db"
                                                << "target_db"
                                                << "coll"
                                                << BSON(""
                                                        << "test"))));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfDbIsNotAValidDatabaseName) {
    auto spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                     << "target_collection"
                                                     << "db"
                                                     << ".test")));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsObjectWithDbSpecifiedAndNoColl) {
    auto targetDb = "target_db";
    auto spec = BSON("$merge" << BSON("into" << BSON("db" << targetDb)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 5786801);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsObjectWithDbSpecifiedAndEmptyColl) {
    auto targetDb = "target_db";
    auto spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                     << ""
                                                     << "db" << targetDb)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 5786801);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfIntoIsObjectWithDbSpecifiedAndNullishColl) {
    auto targetDb = "target_db";
    auto spec = BSON("$merge" << BSON("into" << BSON("coll" << BSONNULL << "db" << targetDb)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 5786801);

    spec = BSON("$merge" << BSON("into" << BSON("coll" << BSONUndefined << "db" << targetDb)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 5786801);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfWhenMatchedModeIsNotStringOrArray) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "whenMatched" << true));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51191);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched" << 100));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51191);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched" << BSON("" << kDefaultWhenMatchedMode)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51191);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfWhenNotMatchedModeIsNotString) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "whenNotMatched" << true));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenNotMatched" << BSONArray()));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenNotMatched" << BSON("" << kDefaultWhenNotMatchedMode)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfWhenMatchedModeIsUnsupportedString) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "whenMatched"
                                      << "unsupported"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::BadValue);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "insert"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::BadValue);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfWhenNotMatchedModeIsUnsupportedString) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "whenNotMatched"
                                      << "unsupported"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::BadValue);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenNotMatched"
                                 << "merge"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::BadValue);
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfOnFieldIsNotStringOrArrayOfStrings) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "on" << 1));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51186);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "on" << BSONArray()));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51187);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "on" << BSON_ARRAY(1 << 2 << BSON("a" << 3))));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51134);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "on" << BSON("_id" << 1)));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51186);
}

TEST_F(DocumentSourceMergeTest, CorrectlyUsesTargetDbThatMatchesAggregationDb) {
    const auto targetDbSameAsAggregationDb = getExpCtx()->ns.db();
    const auto targetColl = "target_collection";
    auto spec = BSON("$merge" << BSON("into" << BSON("coll" << targetColl << "db"
                                                            << targetDbSameAsAggregationDb)));

    auto mergeStage = createMergeStage(spec);
    ASSERT_EQ(mergeStage->getOutputNs().db(), targetDbSameAsAggregationDb);
    ASSERT_EQ(mergeStage->getOutputNs().coll(), targetColl);
}

TEST_F(DocumentSourceMergeTest, SerializeDefaultModesWhenMatchedWhenNotMatched) {
    auto spec = BSON("$out" << BSON("into"
                                    << "target_collection"));
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_EQ(serialized["$merge"][kWhenMatchedModeFieldName].getStringData(),
              kDefaultWhenMatchedMode);
    ASSERT_EQ(serialized["$merge"][kWhenNotMatchedModeFieldName].getStringData(),
              kDefaultWhenNotMatchedMode);

    // Make sure we can reparse the serialized BSON.
    auto reparsedMergeStage = createMergeStage(serialized.toBson());
    auto reSerialized = reparsedMergeStage->serialize().getDocument();
    ASSERT_EQ(reSerialized["$merge"][kWhenMatchedModeFieldName].getStringData(),
              kDefaultWhenMatchedMode);
    ASSERT_EQ(reSerialized["$merge"][kWhenNotMatchedModeFieldName].getStringData(),
              kDefaultWhenNotMatchedMode);
}

TEST_F(DocumentSourceMergeTest, SerializeOnFieldDefaultsToId) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"));
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_EQ(serialized["$merge"][kOnFieldName].getStringData(), "_id");
}

TEST_F(DocumentSourceMergeTest, SerializeCompoundOnFields) {
    auto spec = BSON("$out" << BSON("into"
                                    << "target_collection"
                                    << "on"
                                    << BSON_ARRAY("_id"
                                                  << "shardKey")));
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    auto on = serialized["$merge"][kOnFieldName].getArray();
    ASSERT_EQ(on.size(), 2UL);
    auto comparator = ValueComparator();
    ASSERT_TRUE(comparator.evaluate(on[0] == Value(std::string("_id"))));
    ASSERT_TRUE(comparator.evaluate(on[1] == Value(std::string("shardKey"))));
}

TEST_F(DocumentSourceMergeTest, SerializeDottedPathOnFields) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "on"
                                      << BSON_ARRAY("_id"
                                                    << "a.b")));
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    auto on = serialized["$merge"][kOnFieldName].getArray();
    ASSERT_EQ(on.size(), 2UL);
    auto comparator = ValueComparator();
    ASSERT_TRUE(comparator.evaluate(on[0] == Value(std::string("_id"))));
    ASSERT_TRUE(comparator.evaluate(on[1] == Value(std::string("a.b"))));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "on"
                                 << "_id.a"));
    mergeStage = createMergeStage(spec);
    serialized = mergeStage->serialize().getDocument();
    ASSERT_EQ(serialized["$merge"][kOnFieldName].getStringData(), "_id.a");
}

TEST_F(DocumentSourceMergeTest, SerializeDottedPathOnFieldsSharedPrefix) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "on"
                                      << BSON_ARRAY("_id"
                                                    << "a.b"
                                                    << "a.c")));
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    auto on = serialized["$merge"][kOnFieldName].getArray();
    ASSERT_EQ(on.size(), 3UL);
    auto comparator = ValueComparator();
    ASSERT_TRUE(comparator.evaluate(on[0] == Value(std::string("_id"))));
    ASSERT_TRUE(comparator.evaluate(on[1] == Value(std::string("a.b"))));
    ASSERT_TRUE(comparator.evaluate(on[2] == Value(std::string("a.c"))));
}

TEST_F(DocumentSourceMergeTest, SerializeIntoWhenMergeSpecIsStringNotDotted) {
    const auto aggregationDb = getExpCtx()->ns.db();
    auto spec = BSON("$merge"
                     << "target_collection");
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["db"].getStringData(), aggregationDb);
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["coll"].getStringData(), "target_collection");
}

TEST_F(DocumentSourceMergeTest, SerializeIntoWhenMergeSpecIsStringDotted) {
    const auto aggregationDb = getExpCtx()->ns.db();
    auto spec = BSON("$merge"
                     << "my.target_collection");
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["db"].getStringData(), aggregationDb);
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["coll"].getStringData(), "my.target_collection");
}

TEST_F(DocumentSourceMergeTest, SerializeIntoWhenIntoIsStringNotDotted) {
    const auto aggregationDb = getExpCtx()->ns.db();
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"));
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["db"].getStringData(), aggregationDb);
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["coll"].getStringData(), "target_collection");
}

TEST_F(DocumentSourceMergeTest, SerializeIntoWhenIntoIsStringDotted) {
    const auto aggregationDb = getExpCtx()->ns.db();
    auto spec = BSON("$merge" << BSON("into"
                                      << "my.target_collection"));
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["db"].getStringData(), aggregationDb);
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["coll"].getStringData(), "my.target_collection");
}

TEST_F(DocumentSourceMergeTest, SerializeIntoWhenIntoIsObjectWithCollNotDotted) {
    const auto aggregationDb = getExpCtx()->ns.db();
    auto spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                     << "target_collection")));
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["db"].getStringData(), aggregationDb);
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["coll"].getStringData(), "target_collection");
}

TEST_F(DocumentSourceMergeTest, SerializeIntoWhenIntoIsObjectWithCollDotted) {
    const auto aggregationDb = getExpCtx()->ns.db();
    auto spec = BSON("$merge" << BSON("into" << BSON("coll"
                                                     << "my.target_collection")));
    auto mergeStage = createMergeStage(spec);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["db"].getStringData(), aggregationDb);
    ASSERT_EQ(serialized["$merge"][kIntoFieldName]["coll"].getStringData(), "my.target_collection");
}

TEST_F(DocumentSourceMergeTest, CorrectlyHandlesWhenMatchedAndWhenNotMatchedModes) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "whenMatched"
                                      << "replace"
                                      << "whenNotMatched"
                                      << "insert"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "replace"
                                 << "whenNotMatched"
                                 << "fail"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "replace"
                                 << "whenNotMatched"
                                 << "discard"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "fail"
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "fail"
                                 << "whenNotMatched"
                                 << "fail"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51189);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "fail"
                                 << "whenNotMatched"
                                 << "discard"));
    ASSERT_THROWS_CODE(createMergeStage(spec), DBException, 51189);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "merge"
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "merge"
                                 << "whenNotMatched"
                                 << "fail"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "merge"
                                 << "whenNotMatched"
                                 << "discard"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "keepExisting"
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "keepExisting"
                                 << "whenNotMatched"
                                 << "fail"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51189);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "keepExisting"
                                 << "whenNotMatched"
                                 << "discard"));
    ASSERT_THROWS_CODE(createMergeStage(spec), DBException, 51189);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched" << BSON_ARRAY(BSON("$project" << BSON("x" << 1)))
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched" << BSON_ARRAY(BSON("$project" << BSON("x" << 1)))
                                 << "whenNotMatched"
                                 << "fail"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched" << BSON_ARRAY(BSON("$project" << BSON("x" << 1)))
                                 << "whenNotMatched"
                                 << "discard"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "pipeline"
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::BadValue);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "whenMatched"
                                 << "[{$addFields: {x: 1}}]"
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::BadValue);
}

TEST_F(DocumentSourceMergeTest, LetVariablesCanOnlyBeUsedWithPipelineMode) {
    auto let = BSON("foo"
                    << "bar");
    auto spec =
        BSON("$merge" << BSON("into"
                              << "target_collection"
                              << "let" << let << "whenMatched"
                              << BSON_ARRAY(BSON("$project" << BSON("x" << 1))) << "whenNotMatched"
                              << "insert"));
    ASSERT(createMergeStage(spec));

    spec =
        BSON("$merge" << BSON("into"
                              << "target_collection"
                              << "let" << let << "whenMatched"
                              << BSON_ARRAY(BSON("$project" << BSON("x" << 1))) << "whenNotMatched"
                              << "fail"));
    ASSERT(createMergeStage(spec));

    spec =
        BSON("$merge" << BSON("into"
                              << "target_collection"
                              << "let" << let << "whenMatched"
                              << BSON_ARRAY(BSON("$project" << BSON("x" << 1))) << "whenNotMatched"
                              << "discard"));
    ASSERT(createMergeStage(spec));

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "let" << let << "whenMatched"
                                 << "replace"
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51199);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "let" << let << "whenMatched"
                                 << "replace"
                                 << "whenNotMatched"
                                 << "fail"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51199);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "let" << let << "whenMatched"
                                 << "replace"
                                 << "whenNotMatched"
                                 << "discard"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51199);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "let" << let << "whenMatched"
                                 << "merge"
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51199);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "let" << let << "whenMatched"
                                 << "merge"
                                 << "whenNotMatched"
                                 << "fail"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51199);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "let" << let << "whenMatched"
                                 << "merge"
                                 << "whenNotMatched"
                                 << "discard"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51199);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "let" << let << "whenMatched"
                                 << "keepExisting"
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51199);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "let" << let << "whenMatched"
                                 << "fail"
                                 << "whenNotMatched"
                                 << "insert"));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 51199);
}

// We always serialize the default let variables as {new: "$$ROOT"} if omitted.
TEST_F(DocumentSourceMergeTest, SerializeDefaultLetVariable) {
    for (auto&& whenNotMatched : {"insert", "fail", "discard"}) {
        auto spec =
            BSON("$merge" << BSON("into"
                                  << "target_collection"
                                  << "whenMatched" << BSON_ARRAY(BSON("$project" << BSON("x" << 1)))
                                  << "whenNotMatched" << whenNotMatched));
        auto mergeStage = createMergeStage(spec);
        auto serialized = mergeStage->serialize().getDocument();
        ASSERT_VALUE_EQ(serialized["$merge"]["let"],
                        Value(BSON("new"
                                   << "$$ROOT")));
    }
}

// Test the behaviour of 'let' serialization for each whenNotMatched mode.
TEST_F(DocumentSourceMergeTest, SerializeLetVariables) {
    auto pipeline = BSON_ARRAY(BSON("$project" << BSON("_id" << true << "x"
                                                             << "$$v1"
                                                             << "y"
                                                             << "$$v2"
                                                             << "z"
                                                             << "$$v3")));

    const auto createAndSerializeMergeStage = [this, &pipeline](StringData whenNotMatched) {
        auto spec = BSON("$merge" << BSON("into"
                                          << "target_collection"
                                          << "let"
                                          << BSON("v1" << 10 << "v2"
                                                       << "foo"
                                                       << "v3"
                                                       << BSON("x" << 1 << "y"
                                                                   << BSON("z"
                                                                           << "bar")))
                                          << "whenMatched" << pipeline << "whenNotMatched"
                                          << whenNotMatched));
        auto mergeStage = createMergeStage(spec);
        ASSERT(mergeStage);

        return mergeStage->serialize().getDocument();
    };

    for (auto&& whenNotMatched : {"insert", "fail", "discard"}) {
        const auto serialized = createAndSerializeMergeStage(whenNotMatched);

        // For {whenNotMatched:insert}, we always attach the 'new' document even if the user has
        // already specified a set of variables. This is because a {whenNotMatched: insert} merge
        // generates an upsert, and if no documents in the target collection match the query we must
        // insert the original document. For other 'whenNotMatched' modes, we do not serialize the
        // new document, since neither 'fail' nor 'discard' can result in an upsert.
        ASSERT_VALUE_EQ(serialized["$merge"]["let"]["new"],
                        (whenNotMatched == "insert"_sd ? Value("$$ROOT"_sd) : Value()));

        // The user's variables should be serialized in all cases.
        ASSERT_VALUE_EQ(serialized["$merge"]["let"]["v1"], Value(BSON("$const" << 10)));
        ASSERT_VALUE_EQ(serialized["$merge"]["let"]["v2"],
                        Value(BSON("$const"
                                   << "foo")));
        ASSERT_VALUE_EQ(serialized["$merge"]["let"]["v3"],
                        Value(BSON("x" << BSON("$const" << 1) << "y"
                                       << BSON("z" << BSON("$const"
                                                           << "bar")))));
        ASSERT_VALUE_EQ(serialized["$merge"]["whenMatched"], Value(pipeline));
    }
}

TEST_F(DocumentSourceMergeTest, SerializeLetArrayVariable) {
    for (auto&& whenNotMatched : {"insert", "fail", "discard"}) {
        auto pipeline = BSON_ARRAY(BSON("$project" << BSON("_id" << true << "x"
                                                                 << "$$v1")));
        auto spec = BSON(
            "$merge" << BSON("into"
                             << "target_collection"
                             << "let"
                             << BSON("v1" << BSON_ARRAY(1 << "2" << BSON("x" << 1 << "y" << 2)))
                             << "whenMatched" << pipeline << "whenNotMatched" << whenNotMatched));
        auto mergeStage = createMergeStage(spec);
        ASSERT(mergeStage);
        auto serialized = mergeStage->serialize().getDocument();
        ASSERT_VALUE_EQ(
            serialized["$merge"]["let"]["v1"],
            Value(BSON_ARRAY(BSON("$const" << 1)
                             << BSON("$const"
                                     << "2")
                             << BSON("x" << BSON("$const" << 1) << "y" << BSON("$const" << 2)))));
        ASSERT_VALUE_EQ(serialized["$merge"]["whenMatched"], Value(pipeline));
    }
}

// This test verifies that when the 'let' argument is specified as 'null', the default 'new'
// variable is still available. This is not a desirable behaviour but rather a limitation in the
// IDL parser which cannot differentiate between an optional field specified explicitly as 'null',
// or not specified at all. In both cases it will treat the field like it wasn't specified. So,
// this test ensures that we're aware of this limitation. Once the limitation is addressed in
// SERVER-41272, this test should be updated to accordingly.
TEST_F(DocumentSourceMergeTest, SerializeNullLetVariablesAsDefault) {
    for (auto&& whenNotMatched : {"insert", "fail", "discard"}) {
        auto pipeline = BSON_ARRAY(BSON("$project" << BSON("_id" << true << "x"
                                                                 << BSON("$const"
                                                                         << "1"))));
        auto spec = BSON("$merge" << BSON("into"
                                          << "target_collection"
                                          << "let" << BSONNULL << "whenMatched" << pipeline
                                          << "whenNotMatched" << whenNotMatched));
        auto mergeStage = createMergeStage(spec);
        ASSERT(mergeStage);
        auto serialized = mergeStage->serialize().getDocument();
        ASSERT_VALUE_EQ(serialized["$merge"]["let"],
                        Value(BSON("new"
                                   << "$$ROOT")));
        ASSERT_VALUE_EQ(serialized["$merge"]["whenMatched"], Value(pipeline));
    }
}

TEST_F(DocumentSourceMergeTest, SerializeEmptyLetVariables) {
    for (auto&& whenNotMatched : {"insert", "fail", "discard"}) {
        auto pipeline = BSON_ARRAY(BSON("$project" << BSON("_id" << true << "x"
                                                                 << BSON("$const"
                                                                         << "1"))));
        auto spec = BSON("$merge" << BSON("into"
                                          << "target_collection"
                                          << "let" << BSONObj() << "whenMatched" << pipeline
                                          << "whenNotMatched" << whenNotMatched));
        auto mergeStage = createMergeStage(spec);
        ASSERT(mergeStage);
        auto serialized = mergeStage->serialize().getDocument();
        ASSERT_VALUE_EQ(serialized["$merge"]["let"],
                        (whenNotMatched == "insert"_sd ? Value(BSON("new"
                                                                    << "$$ROOT"))
                                                       : Value(BSONObj())));
        ASSERT_VALUE_EQ(serialized["$merge"]["whenMatched"], Value(pipeline));
    }
}

TEST_F(DocumentSourceMergeTest, SerializeEmptyLetVariableMentionNew) {
    auto pipeline = BSON_ARRAY(fromjson("{$project: {_id: true, x: '$$new'}}"));
    auto spec =
        BSON("$merge" << BSON("into"
                              << "target_collection"
                              << "let" << BSONObj() << "whenMatched" << pipeline << "whenNotMatched"
                              << "insert"));
    auto mergeStage = createMergeStage(spec);
    ASSERT(mergeStage);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$merge": {
                "into": {
                    "db": "unittests",
                    "coll": "target_collection"
                },
                "on": "_id",
                "let": {
                    "new": "$$ROOT"
                },
                "whenMatched": [
                    {
                        "$project": {
                            "_id": true,
                            "x": "$$new"
                        }
                    }
                ],
                "whenNotMatched": "insert"
            }
        })",
        serialized.toBson());
}

TEST_F(DocumentSourceMergeTest, OnlyObjectCanBeUsedAsLetVariables) {
    for (auto&& whenNotMatched : {"insert", "fail", "discard"}) {
        auto pipeline = BSON_ARRAY(BSON("$project" << BSON("x"
                                                           << "1")));
        auto spec = BSON("$merge" << BSON("into"
                                          << "target_collection"
                                          << "let" << 1 << "whenMatched" << pipeline
                                          << "whenNotMatched" << whenNotMatched));
        ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);

        spec = BSON("$merge" << BSON("into"
                                     << "target_collection"
                                     << "let"
                                     << "foo"
                                     << "whenMatched" << pipeline << "whenNotMatched"
                                     << whenNotMatched));
        ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);

        spec = BSON("$merge" << BSON("into"
                                     << "target_collection"
                                     << "let" << BSON_ARRAY(1 << "2") << "whenMatched" << pipeline
                                     << "whenNotMatched" << whenNotMatched));
        ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, ErrorCodes::TypeMismatch);
    }
}

TEST_F(DocumentSourceMergeTest, FailsToParseIfOnFieldHaveDuplicates) {
    auto spec = BSON("$merge" << BSON("into"
                                      << "target_collection"
                                      << "on"
                                      << BSON_ARRAY("x"
                                                    << "y"
                                                    << "x")));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 31465);

    spec = BSON("$merge" << BSON("into"
                                 << "target_collection"
                                 << "on"
                                 << BSON_ARRAY("_id"
                                               << "_id")));
    ASSERT_THROWS_CODE(createMergeStage(spec), AssertionException, 31465);
}

using DocumentSourceMergeServerlessTest = ServerlessAggregationContextFixture;

TEST_F(DocumentSourceMergeServerlessTest,
       LiteParsedDocumentSourceLookupStringContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        auto tenantId = TenantId(OID::gen());
        NamespaceString nss =
            NamespaceString::createNamespaceString_forTest(tenantId, _targetDb, "testColl");

        // Pass collection name as a string.
        auto stageSpec = BSON("$merge" << _targetColl);
        auto liteParsedLookup =
            DocumentSourceMerge::LiteParsed::parse(nss, stageSpec.firstElement());
        auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();
        ASSERT_EQ(1, namespaceSet.size());
        ASSERT_EQ(1ul,
                  namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                      tenantId, _targetDb, _targetColl)));
    }
}

TEST_F(DocumentSourceMergeServerlessTest,
       LiteParsedDocumentSourceLookupObjContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        auto tenantId = TenantId(OID::gen());
        NamespaceString nss =
            NamespaceString::createNamespaceString_forTest(tenantId, _targetDb, "testColl");

        // Pass collection name as a db + coll object.
        auto stageSpec =
            BSON("$merge" << BSON("into" << BSON("db" << _targetDb << "coll" << _targetColl)));
        auto liteParsedLookup =
            DocumentSourceMerge::LiteParsed::parse(nss, stageSpec.firstElement());
        auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();
        ASSERT_EQ(1, namespaceSet.size());
        ASSERT_EQ(1ul,
                  namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                      tenantId, _targetDb, _targetColl)));
    }
}

TEST_F(DocumentSourceMergeServerlessTest,
       LiteParsedDocumentSourceLookupObjContainsExpectedNamespacesInServerlessPrefixed) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);

    auto tenantId = TenantId(OID::gen());
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(tenantId, _targetDb, "testColl");

    // Pass collection name as a db + coll object.
    auto stageSpec =
        BSON("$merge" << BSON("into" << BSON("db" << nss.dbName().toStringWithTenantId() << "coll"
                                                  << _targetColl)));
    auto liteParsedLookup = DocumentSourceMerge::LiteParsed::parse(nss, stageSpec.firstElement());
    auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                  tenantId, _targetDb, _targetColl)));
}

TEST_F(DocumentSourceMergeServerlessTest,
       CreateFromBSONStringContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    ASSERT(expCtx->ns.tenantId());

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        // Pass collection name as a string.
        auto spec = BSON("$merge" << _targetColl);
        auto mergeStage = DocumentSourceMerge::createFromBson(spec.firstElement(), expCtx);
        auto mergeSource = static_cast<DocumentSourceMerge*>(mergeStage.get());
        ASSERT(mergeStage);
        ASSERT(mergeSource->getOutputNs().tenantId());
        ASSERT_EQ(*mergeSource->getOutputNs().tenantId(), *expCtx->ns.tenantId());

        // Assert the tenantId is not included in the serialized namespace.
        auto dbField = flagStatus ? expCtx->ns.dbName().toString()
                                  : expCtx->ns.dbName().toStringWithTenantId();
        auto expectedDoc = Document{{"db", dbField}, {"coll", _targetColl}};

        auto serialized = mergeSource->serialize().getDocument();
        ASSERT_DOCUMENT_EQ(serialized["$merge"][kIntoFieldName].getDocument(), expectedDoc);
    }
}

TEST_F(DocumentSourceMergeServerlessTest,
       CreateFromBSONCollObjContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    ASSERT(expCtx->ns.tenantId());

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        // Pass collection name as a coll object.
        auto spec = BSON("$merge" << BSON("into" << BSON("coll" << _targetColl)));
        auto mergeStage = DocumentSourceMerge::createFromBson(spec.firstElement(), expCtx);
        auto mergeSource = static_cast<DocumentSourceMerge*>(mergeStage.get());
        ASSERT(mergeSource);
        ASSERT(mergeSource->getOutputNs().tenantId());
        ASSERT_EQ(*mergeSource->getOutputNs().tenantId(), *expCtx->ns.tenantId());

        auto dbField = flagStatus ? expCtx->ns.dbName().toString()
                                  : expCtx->ns.dbName().toStringWithTenantId();
        auto expectedDoc = Document{{"db", dbField}, {"coll", _targetColl}};

        auto serialized = mergeSource->serialize().getDocument();
        ASSERT_DOCUMENT_EQ(serialized["$merge"][kIntoFieldName].getDocument(), expectedDoc);
    }
}

TEST_F(DocumentSourceMergeServerlessTest,
       CreateFromBSONDbObjContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    ASSERT(expCtx->ns.tenantId());

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        // Pass collection name as a db + coll object.
        auto spec =
            BSON("$merge" << BSON("into" << BSON("db" << _targetDb << "coll" << _targetColl)));
        auto mergeStage = DocumentSourceMerge::createFromBson(spec.firstElement(), expCtx);
        auto mergeSource = static_cast<DocumentSourceMerge*>(mergeStage.get());
        ASSERT(mergeSource);
        ASSERT(mergeSource->getOutputNs().tenantId());
        ASSERT_EQ(*mergeSource->getOutputNs().tenantId(), *expCtx->ns.tenantId());

        auto dbField = flagStatus
            ? _targetDb
            : str::stream() << (*expCtx).ns.tenantId()->toString() << '_' << _targetDb;
        auto expectedDoc = Document{{"db", dbField}, {"coll", _targetColl}};

        auto serialized = mergeSource->serialize().getDocument();
        ASSERT_DOCUMENT_EQ(serialized["$merge"][kIntoFieldName].getDocument(), expectedDoc);
    }
}

TEST_F(DocumentSourceMergeServerlessTest,
       CreateFromBSONDbObjContainsExpectedNamespacesInServerlessPrefix) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    ASSERT(expCtx->ns.tenantId());

    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);

    // We're expecting a prefix given gFeatureFlagRequireTenantId is false.
    std::string dbField = str::stream() << (*expCtx).ns.tenantId()->toString() << '_' << _targetDb;

    // Pass collection name as a db + coll object.
    auto spec = BSON("$merge" << BSON("into" << BSON("db" << dbField << "coll" << _targetColl)));
    auto mergeStage = DocumentSourceMerge::createFromBson(spec.firstElement(), expCtx);
    auto mergeSource = static_cast<DocumentSourceMerge*>(mergeStage.get());
    ASSERT(mergeSource);
    ASSERT(mergeSource->getOutputNs().tenantId());
    ASSERT_EQ(*mergeSource->getOutputNs().tenantId(), *expCtx->ns.tenantId());

    auto expectedDoc = Document{{"db", dbField}, {"coll", _targetColl}};

    auto serialized = mergeSource->serialize().getDocument();
    ASSERT_DOCUMENT_EQ(serialized["$merge"][kIntoFieldName].getDocument(), expectedDoc);
}

TEST_F(DocumentSourceMergeTest, QueryShape) {
    auto pipeline = BSON_ARRAY(BSON("$project" << BSON("x"
                                                       << "1")));
    auto spec =
        BSON("$merge" << BSON("into"
                              << "target_collection"
                              << "let" << BSONObj() << "whenMatched" << pipeline << "whenNotMatched"
                              << "insert"));
    auto mergeStage = createMergeStage(spec);
    ASSERT(mergeStage);
    auto serialized = mergeStage->serialize().getDocument();
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$merge": {
                "into": {
                    "db": "unittests",
                    "coll": "target_collection"
                },
                "on": "_id",
                "let": {
                    "new": "$$ROOT"
                },
                "whenMatched": [
                    {
                        "$project": {
                            "HASH<_id>": true,
                            "HASH<x>": "?string"
                        }
                    }
                ],
                "whenNotMatched": "insert"
            }
        })",
        redact(*mergeStage));
}

}  // namespace
}  // namespace mongo
