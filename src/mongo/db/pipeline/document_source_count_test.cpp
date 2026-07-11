// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_count.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
using boost::intrusive_ptr;
using std::list;
using std::vector;

class CountReturnsGroupAndProjectStages : public AggregationContextFixture {
public:
    void testCreateFromBsonResult(BSONObj countSpec) {
        list<intrusive_ptr<DocumentSource>> result =
            DocumentSourceCount::createFromBson(countSpec.firstElement(), getExpCtx());

        ASSERT_EQUALS(result.size(), 2UL);

        const auto* groupStage = dynamic_cast<DocumentSourceGroup*>(result.front().get());
        ASSERT(groupStage);

        const auto* projectStage =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>(result.back().get());
        ASSERT(projectStage);

        vector<Value> explainedStages;
        groupStage->serializeToArray(
            explainedStages,
            query_shape::SerializationOptions{
                .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)});
        projectStage->serializeToArray(
            explainedStages,
            query_shape::SerializationOptions{
                .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)});
        ASSERT_EQUALS(explainedStages.size(), 2UL);

        std::string_view countName = countSpec.firstElement().valueStringData();
        Value expectedGroupExplain =
            Value{Document{{"_id", Document{{"$const", BSONNULL}}},
                           {countName, Document{{"$sum", Document{{"$const", 1}}}}},
                           {"$willBeMerged", false}}};
        auto groupExplain = explainedStages[0];
        ASSERT_VALUE_EQ(groupExplain["$group"], expectedGroupExplain);

        Value expectedProjectExplain = Value{Document{{countName, true}, {"_id", false}}};
        auto projectExplain = explainedStages[1];
        ASSERT_VALUE_EQ(projectExplain["$project"], expectedProjectExplain);
    }
};

TEST_F(CountReturnsGroupAndProjectStages, ValidStringSpec) {
    BSONObj spec = BSON("$count" << "myCount");
    testCreateFromBsonResult(spec);

    spec = BSON("$count" << "quantity");
    testCreateFromBsonResult(spec);
}

class InvalidCountSpec : public AggregationContextFixture {
public:
    list<intrusive_ptr<DocumentSource>> createCount(BSONObj countSpec) {
        auto specElem = countSpec.firstElement();
        return DocumentSourceCount::createFromBson(specElem, getExpCtx());
    }
};

TEST_F(InvalidCountSpec, NonStringSpec) {
    BSONObj spec = BSON("$count" << 1);
    ASSERT_THROWS_CODE(createCount(spec), AssertionException, 40156);

    spec = BSON("$count" << BSON("field1" << "test"));
    ASSERT_THROWS_CODE(createCount(spec), AssertionException, 40156);
}

TEST_F(InvalidCountSpec, EmptyStringSpec) {
    BSONObj spec = BSON("$count" << "");
    ASSERT_THROWS_CODE(createCount(spec), AssertionException, 40157);
}

TEST_F(InvalidCountSpec, FieldPathSpec) {
    BSONObj spec = BSON("$count" << "$x");
    ASSERT_THROWS_CODE(createCount(spec), AssertionException, 40158);
}

TEST_F(InvalidCountSpec, EmbeddedNullByteSpec) {
    BSONObj spec = BSON("$count" << "te\0st"sv);
    ASSERT_THROWS_CODE(createCount(spec), AssertionException, 40159);
}

TEST_F(InvalidCountSpec, PeriodInStringSpec) {
    BSONObj spec = BSON("$count" << "test.string");
    ASSERT_THROWS_CODE(createCount(spec), AssertionException, 40160);
}

TEST_F(InvalidCountSpec, IDAsStringSpec) {
    BSONObj spec = BSON("$count" << "_id");
    ASSERT_THROWS_CODE(createCount(spec), AssertionException, 9039800);
}

}  // namespace
}  // namespace mongo
