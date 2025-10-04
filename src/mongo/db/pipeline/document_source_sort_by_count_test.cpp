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

#include "mongo/db/pipeline/document_source_sort_by_count.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using boost::intrusive_ptr;
using std::list;
using std::vector;

/**
 * Fixture to test that $sortByCount returns a DocumentSourceGroup and DocumentSourceSort.
 */
class SortByCountReturnsGroupAndSort : public AggregationContextFixture {
public:
    void testCreateFromBsonResult(BSONObj sortByCountSpec, Value expectedGroupExplain) {
        list<intrusive_ptr<DocumentSource>> result =
            DocumentSourceSortByCount::createFromBson(sortByCountSpec.firstElement(), getExpCtx());

        ASSERT_EQUALS(result.size(), 2UL);

        const auto* groupStage = dynamic_cast<DocumentSourceGroup*>(result.front().get());
        ASSERT(groupStage);

        const auto* sortStage = dynamic_cast<DocumentSourceSort*>(result.back().get());
        ASSERT(sortStage);

        // Serialize the DocumentSourceGroup and DocumentSourceSort from $sortByCount so that we can
        // check the explain output to make sure $group and $sort have the correct fields.
        const auto explain = SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};
        vector<Value> explainedStages;
        groupStage->serializeToArray(explainedStages, explain);
        sortStage->serializeToArray(explainedStages, explain);
        ASSERT_EQUALS(explainedStages.size(), 2UL);

        auto groupExplain = explainedStages[0];
        ASSERT_VALUE_EQ(groupExplain["$group"], expectedGroupExplain);

        auto sortExplain = explainedStages[1];
        auto expectedSortExplain = Value{Document{{"sortKey", Document{{"count", -1}}}}};
        ASSERT_VALUE_EQ(sortExplain["$sort"], expectedSortExplain);
    }
};

TEST_F(SortByCountReturnsGroupAndSort, ExpressionFieldPathSpec) {
    BSONObj spec = BSON("$sortByCount" << "$x");
    Value expectedGroupExplain =
        Value{Document{{"_id", "$x"_sd},
                       {"count", Document{{"$sum", Document{{"$const", 1}}}}},
                       {"$willBeMerged", false}}};
    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(SortByCountReturnsGroupAndSort, ExpressionInObjectSpec) {
    BSONObj spec = BSON("$sortByCount" << BSON("$floor" << "$x"));
    Value expectedGroupExplain =
        Value{Document{{"_id", Document{{"$floor", Value{BSON_ARRAY("$x")}}}},
                       {"count", Document{{"$sum", Document{{"$const", 1}}}}},
                       {"$willBeMerged", false}}};
    testCreateFromBsonResult(spec, expectedGroupExplain);

    spec = BSON("$sortByCount" << BSON("$eq" << BSON_ARRAY("$x" << 15)));
    expectedGroupExplain =
        Value{Document{{"_id", Document{{"$eq", Value{BSON_ARRAY("$x" << BSON("$const" << 15))}}}},
                       {"count", Document{{"$sum", Document{{"$const", 1}}}}},
                       {"$willBeMerged", false}}};
    testCreateFromBsonResult(spec, expectedGroupExplain);
}

/**
 * Fixture to test error cases of the $sortByCount stage.
 */
class InvalidSortByCountSpec : public AggregationContextFixture {
public:
    list<intrusive_ptr<DocumentSource>> createSortByCount(BSONObj sortByCountSpec) {
        auto specElem = sortByCountSpec.firstElement();
        return DocumentSourceSortByCount::createFromBson(specElem, getExpCtx());
    }
};

TEST_F(InvalidSortByCountSpec, NonObjectNonStringSpec) {
    BSONObj spec = BSON("$sortByCount" << 1);
    ASSERT_THROWS_CODE(createSortByCount(spec), AssertionException, 40149);

    spec = BSON("$sortByCount" << BSONNULL);
    ASSERT_THROWS_CODE(createSortByCount(spec), AssertionException, 40149);
}

TEST_F(InvalidSortByCountSpec, NonExpressionInObjectSpec) {
    BSONObj spec = BSON("$sortByCount" << BSON("field1" << "$x"));
    ASSERT_THROWS_CODE(createSortByCount(spec), AssertionException, 40147);
}

TEST_F(InvalidSortByCountSpec, NonFieldPathStringSpec) {
    BSONObj spec = BSON("$sortByCount" << "test");
    ASSERT_THROWS_CODE(createSortByCount(spec), AssertionException, 40148);
}

}  // namespace
}  // namespace mongo
