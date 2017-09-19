/**
 *    Copyright (C) 2015 MongoDB Inc.
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
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/query/count_request.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

static const NamespaceString testns("TestDB.TestColl");

TEST(CountRequest, ParseDefaults) {
    const bool isExplain = false;
    const auto countRequestStatus =
        CountRequest::parseFromBSON(testns,
                                    BSON("count"
                                         << "TestColl"
                                         << "query"
                                         << BSON("a" << BSON("$lte" << 10))),
                                    isExplain);

    ASSERT_OK(countRequestStatus.getStatus());

    const CountRequest& countRequest = countRequestStatus.getValue();

    ASSERT_EQ(countRequest.getNs().ns(), "TestDB.TestColl");
    ASSERT_BSONOBJ_EQ(countRequest.getQuery(), fromjson("{ a : { '$lte' : 10 } }"));

    // Defaults
    ASSERT_EQUALS(countRequest.getLimit(), 0);
    ASSERT_EQUALS(countRequest.getSkip(), 0);
    ASSERT_EQUALS(countRequest.getMaxTimeMS(), 0u);
    ASSERT(countRequest.getHint().isEmpty());
    ASSERT(countRequest.getCollation().isEmpty());
    ASSERT(countRequest.getReadConcern().isEmpty());
    ASSERT(countRequest.getUnwrappedReadPref().isEmpty());
    ASSERT(countRequest.getComment().empty());
}

TEST(CountRequest, ParseComplete) {
    const bool isExplain = false;
    const auto countRequestStatus =
        CountRequest::parseFromBSON(testns,
                                    BSON("count"
                                         << "TestColl"
                                         << "query"
                                         << BSON("a" << BSON("$gte" << 11))
                                         << "limit"
                                         << 100
                                         << "skip"
                                         << 1000
                                         << "hint"
                                         << BSON("b" << 5)
                                         << "collation"
                                         << BSON("locale"
                                                 << "en_US")
                                         << "readConcern"
                                         << BSON("level"
                                                 << "linearizable")
                                         << "$queryOptions"
                                         << BSON("$readPreference"
                                                 << "secondary")
                                         << "comment"
                                         << "aComment"
                                         << "maxTimeMS"
                                         << 10000),
                                    isExplain);

    ASSERT_OK(countRequestStatus.getStatus());

    const CountRequest& countRequest = countRequestStatus.getValue();

    ASSERT_EQ(countRequest.getNs().ns(), "TestDB.TestColl");
    ASSERT_BSONOBJ_EQ(countRequest.getQuery(), fromjson("{ a : { '$gte' : 11 } }"));
    ASSERT_EQUALS(countRequest.getLimit(), 100);
    ASSERT_EQUALS(countRequest.getSkip(), 1000);
    ASSERT_EQUALS(countRequest.getMaxTimeMS(), 10000u);
    ASSERT_EQUALS(countRequest.getComment(), "aComment");
    ASSERT_BSONOBJ_EQ(countRequest.getHint(), fromjson("{ b : 5 }"));
    ASSERT_BSONOBJ_EQ(countRequest.getCollation(), fromjson("{ locale : 'en_US' }"));
    ASSERT_BSONOBJ_EQ(countRequest.getReadConcern(), fromjson("{ level: 'linearizable' }"));
    ASSERT_BSONOBJ_EQ(countRequest.getUnwrappedReadPref(),
                      fromjson("{ $readPreference: 'secondary' }"));
}

TEST(CountRequest, ParseWithExplain) {
    const bool isExplain = true;
    const auto countRequestStatus =
        CountRequest::parseFromBSON(testns,
                                    BSON("count"
                                         << "TestColl"
                                         << "query"
                                         << BSON("a" << BSON("$lte" << 10))),
                                    isExplain);

    ASSERT_OK(countRequestStatus.getStatus());

    const CountRequest& countRequest = countRequestStatus.getValue();

    ASSERT_EQ(countRequest.getNs().ns(), "TestDB.TestColl");
    ASSERT_BSONOBJ_EQ(countRequest.getQuery(), fromjson("{ a : { '$lte' : 10 } }"));

    // Defaults
    ASSERT_EQUALS(countRequest.getLimit(), 0);
    ASSERT_EQUALS(countRequest.getSkip(), 0);
    ASSERT_EQUALS(countRequest.isExplain(), true);
    ASSERT(countRequest.getHint().isEmpty());
    ASSERT(countRequest.getCollation().isEmpty());
}

TEST(CountRequest, ParseNegativeLimit) {
    const auto countRequestStatus =
        CountRequest::parseFromBSON(testns,
                                    BSON("count"
                                         << "TestColl"
                                         << "query"
                                         << BSON("a" << BSON("$gte" << 11))
                                         << "limit"
                                         << -100
                                         << "skip"
                                         << 1000
                                         << "hint"
                                         << BSON("b" << 5)
                                         << "collation"
                                         << BSON("locale"
                                                 << "en_US")),
                                    false);

    ASSERT_OK(countRequestStatus.getStatus());

    const CountRequest& countRequest = countRequestStatus.getValue();

    ASSERT_EQ(countRequest.getNs().ns(), "TestDB.TestColl");
    ASSERT_BSONOBJ_EQ(countRequest.getQuery(), fromjson("{ a : { '$gte' : 11 } }"));
    ASSERT_EQUALS(countRequest.getLimit(), 100);
    ASSERT_EQUALS(countRequest.getSkip(), 1000);
    ASSERT_BSONOBJ_EQ(countRequest.getHint(), fromjson("{ b : 5 }"));
    ASSERT_BSONOBJ_EQ(countRequest.getCollation(), fromjson("{ locale : 'en_US' }"));
}

TEST(CountRequest, FailParseBadSkipValue) {
    const bool isExplain = false;
    const auto countRequestStatus =
        CountRequest::parseFromBSON(testns,
                                    BSON("count"
                                         << "TestColl"
                                         << "query"
                                         << BSON("a" << BSON("$gte" << 11))
                                         << "skip"
                                         << -1000),
                                    isExplain);

    ASSERT_EQUALS(countRequestStatus.getStatus(), ErrorCodes::BadValue);
}

TEST(CountRequest, FailParseBadCollationValue) {
    const bool isExplain = false;
    const auto countRequestStatus =
        CountRequest::parseFromBSON(testns,
                                    BSON("count"
                                         << "TestColl"
                                         << "query"
                                         << BSON("a" << BSON("$gte" << 11))
                                         << "collation"
                                         << "en_US"),
                                    isExplain);

    ASSERT_EQUALS(countRequestStatus.getStatus(), ErrorCodes::BadValue);
}

TEST(CountRequest, ConvertToAggregationWithHint) {
    CountRequest countRequest(testns, BSONObj());
    countRequest.setHint(BSON("x" << 1));
    auto agg = countRequest.asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT_BSONOBJ_EQ(ar.getValue().getHint(), BSON("x" << 1));

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountRequest, ConvertToAggregationSucceeds) {
    CountRequest countRequest(testns, BSONObj());
    auto agg = countRequest.asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT_EQ(ar.getValue().getBatchSize(), AggregationRequest::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespaceString(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation(), BSONObj());

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountRequest, ConvertToAggregationWithQueryAndFilterAndLimit) {
    CountRequest countRequest(testns, BSON("x" << 7));
    countRequest.setLimit(200);
    countRequest.setSkip(300);
    auto agg = countRequest.asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT_EQ(ar.getValue().getBatchSize(), AggregationRequest::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespaceString(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation(), BSONObj());

    std::vector<BSONObj> expectedPipeline{BSON("$match" << BSON("x" << 7)),
                                          BSON("$skip" << 300),
                                          BSON("$limit" << 200),
                                          BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountRequest, ConvertToAggregationWithMaxTimeMS) {
    CountRequest countRequest(testns, BSONObj());
    countRequest.setMaxTimeMS(100);
    auto agg = countRequest.asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT_EQ(ar.getValue().getMaxTimeMS(), 100u);

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountRequest, ConvertToAggregationWithQueryOptions) {
    CountRequest countRequest(testns, BSONObj());
    countRequest.setUnwrappedReadPref(BSON("readPreference"
                                           << "secondary"));
    auto agg = countRequest.asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT_BSONOBJ_EQ(ar.getValue().getUnwrappedReadPref(),
                      BSON("readPreference"
                           << "secondary"));

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountRequest, ConvertToAggregationWithComment) {
    CountRequest countRequest(testns, BSONObj());
    countRequest.setComment("aComment");
    auto agg = countRequest.asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT_EQ(ar.getValue().getComment(), "aComment");

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountRequest, ConvertToAggregationWithReadConcern) {
    CountRequest countRequest(testns, BSONObj());
    countRequest.setReadConcern(BSON("level"
                                     << "linearizable"));
    auto agg = countRequest.asAggregationCommand();
    ASSERT_OK(agg);

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT_BSONOBJ_EQ(ar.getValue().getReadConcern(),
                      BSON("level"
                           << "linearizable"));

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountRequest, ConvertToAggregationOmitsExplain) {
    CountRequest countRequest(testns, BSONObj());
    countRequest.setExplain(true);
    auto agg = countRequest.asAggregationCommand();
    ASSERT_OK(agg);

    ASSERT_FALSE(agg.getValue().hasField("explain"));

    auto ar = AggregationRequest::parseFromBSON(testns, agg.getValue());
    ASSERT_OK(ar.getStatus());
    ASSERT_FALSE(ar.getValue().getExplain());
    ASSERT_EQ(ar.getValue().getNamespaceString(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation(), BSONObj());

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

}  // namespace
}  // namespace mongo
