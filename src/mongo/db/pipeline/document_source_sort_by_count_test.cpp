/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_sort_by_count.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using std::vector;
using boost::intrusive_ptr;

/**
 * Fixture to test that $sortByCount returns a DocumentSourceGroup and DocumentSourceSort.
 */
class SortByCountReturnsGroupAndSort : public AggregationContextFixture {
public:
    void testCreateFromBsonResult(BSONObj sortByCountSpec, Value expectedGroupExplain) {
        vector<intrusive_ptr<DocumentSource>> result =
            DocumentSourceSortByCount::createFromBson(sortByCountSpec.firstElement(), getExpCtx());

        ASSERT_EQUALS(result.size(), 2UL);

        const auto* groupStage = dynamic_cast<DocumentSourceGroup*>(result[0].get());
        ASSERT(groupStage);

        const auto* sortStage = dynamic_cast<DocumentSourceSort*>(result[1].get());
        ASSERT(sortStage);

        // Serialize the DocumentSourceGroup and DocumentSourceSort from $sortByCount so that we can
        // check the explain output to make sure $group and $sort have the correct fields.
        const auto explain = ExplainOptions::Verbosity::kQueryPlanner;
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
    BSONObj spec = BSON("$sortByCount"
                        << "$x");
    Value expectedGroupExplain =
        Value{Document{{"_id", "$x"_sd}, {"count", Document{{"$sum", Document{{"$const", 1}}}}}}};
    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(SortByCountReturnsGroupAndSort, ExpressionInObjectSpec) {
    BSONObj spec = BSON("$sortByCount" << BSON("$floor"
                                               << "$x"));
    Value expectedGroupExplain =
        Value{Document{{"_id", Document{{"$floor", Value{BSON_ARRAY("$x")}}}},
                       {"count", Document{{"$sum", Document{{"$const", 1}}}}}}};
    testCreateFromBsonResult(spec, expectedGroupExplain);

    spec = BSON("$sortByCount" << BSON("$eq" << BSON_ARRAY("$x" << 15)));
    expectedGroupExplain =
        Value{Document{{"_id", Document{{"$eq", Value{BSON_ARRAY("$x" << BSON("$const" << 15))}}}},
                       {"count", Document{{"$sum", Document{{"$const", 1}}}}}}};
    testCreateFromBsonResult(spec, expectedGroupExplain);
}

/**
 * Fixture to test error cases of the $sortByCount stage.
 */
class InvalidSortByCountSpec : public AggregationContextFixture {
public:
    vector<intrusive_ptr<DocumentSource>> createSortByCount(BSONObj sortByCountSpec) {
        auto specElem = sortByCountSpec.firstElement();
        return DocumentSourceSortByCount::createFromBson(specElem, getExpCtx());
    }
};

TEST_F(InvalidSortByCountSpec, NonObjectNonStringSpec) {
    BSONObj spec = BSON("$sortByCount" << 1);
    ASSERT_THROWS_CODE(createSortByCount(spec), UserException, 40149);

    spec = BSON("$sortByCount" << BSONNULL);
    ASSERT_THROWS_CODE(createSortByCount(spec), UserException, 40149);
}

TEST_F(InvalidSortByCountSpec, NonExpressionInObjectSpec) {
    BSONObj spec = BSON("$sortByCount" << BSON("field1"
                                               << "$x"));
    ASSERT_THROWS_CODE(createSortByCount(spec), UserException, 40147);
}

TEST_F(InvalidSortByCountSpec, NonFieldPathStringSpec) {
    BSONObj spec = BSON("$sortByCount"
                        << "test");
    ASSERT_THROWS_CODE(createSortByCount(spec), UserException, 40148);
}

}  // namespace
}  // namespace mongo
