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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/query/parsed_distinct.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

static const NamespaceString testns("testdb.testcoll");
static const bool isExplain = true;

TEST(ParsedDistinctTest, ConvertToAggregationNoQuery) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* txn = uniqueTxn.get();

    auto pd = ParsedDistinct::parse(txn,
                                    testns,
                                    fromjson("{distinct: 'testcoll', key: 'x'}"),
                                    ExtensionsCallbackDisallowExtensions(),
                                    !isExplain);
    ASSERT_OK(pd.getStatus());

    auto agg = pd.getValue().asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().isExplain());
    ASSERT(ar.getValue().isCursorCommand());
    ASSERT_EQ(ar.getValue().getNamespaceString(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation(), BSONObj());

    std::vector<BSONObj> expectedPipeline{
        BSON("$group" << BSON("_id" << BSONNULL << "distinct" << BSON("$addToSet"
                                                                      << "$x")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(ParsedDistinctTest, ConvertToAggregationWithQuery) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* txn = uniqueTxn.get();

    auto pd = ParsedDistinct::parse(txn,
                                    testns,
                                    fromjson("{distinct: 'testcoll', key: 'y', query: {z: 7}}"),
                                    ExtensionsCallbackDisallowExtensions(),
                                    !isExplain);
    ASSERT_OK(pd.getStatus());

    auto agg = pd.getValue().asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().isExplain());
    ASSERT(ar.getValue().isCursorCommand());
    ASSERT_EQ(ar.getValue().getNamespaceString(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation(), BSONObj());

    std::vector<BSONObj> expectedPipeline{
        BSON("$match" << BSON("z" << 7)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct" << BSON("$addToSet"
                                                                      << "$y")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(ParsedDistinctTest, ConvertToAggregationWithExplain) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* txn = uniqueTxn.get();

    auto pd = ParsedDistinct::parse(txn,
                                    testns,
                                    fromjson("{distinct: 'testcoll', key: 'x'}"),
                                    ExtensionsCallbackDisallowExtensions(),
                                    isExplain);
    ASSERT_OK(pd.getStatus());

    auto agg = pd.getValue().asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT(ar.getValue().isExplain());
    ASSERT(ar.getValue().isCursorCommand());
    ASSERT_EQ(ar.getValue().getNamespaceString(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation(), BSONObj());

    std::vector<BSONObj> expectedPipeline{
        BSON("$group" << BSON("_id" << BSONNULL << "distinct" << BSON("$addToSet"
                                                                      << "$x")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

}  // namespace
}  // namespace mongo
