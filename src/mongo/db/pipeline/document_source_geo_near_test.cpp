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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceGeoNearTest = AggregationContextFixture;

TEST_F(DocumentSourceGeoNearTest, ShouldAbsorbSubsequentLimitStage) {
    auto geoNear = DocumentSourceGeoNear::create(getExpCtx());

    Pipeline::SourceContainer container;
    container.push_back(geoNear);

    ASSERT_EQUALS(geoNear->getLimit(), DocumentSourceGeoNear::kDefaultLimit);

    container.push_back(DocumentSourceLimit::create(getExpCtx(), 200));
    geoNear->optimizeAt(container.begin(), &container);

    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_EQUALS(geoNear->getLimit(), DocumentSourceGeoNear::kDefaultLimit);

    container.push_back(DocumentSourceLimit::create(getExpCtx(), 50));
    geoNear->optimizeAt(container.begin(), &container);

    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_EQUALS(geoNear->getLimit(), 50);

    container.push_back(DocumentSourceLimit::create(getExpCtx(), 30));
    geoNear->optimizeAt(container.begin(), &container);

    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_EQUALS(geoNear->getLimit(), 30);
}

TEST_F(DocumentSourceGeoNearTest, ShouldReportOutputsAreSortedByDistanceField) {
    BSONObj queryObj = fromjson(
        "{geoNear: { near: {type: 'Point', coordinates: [0, 0]}, distanceField: 'dist', "
        "maxDistance: 2}}");
    auto geoNear = DocumentSourceGeoNear::createFromBson(queryObj.firstElement(), getExpCtx());

    BSONObjSet outputSort = geoNear->getOutputSorts();

    ASSERT_EQUALS(outputSort.count(BSON("dist" << -1)), 1U);
    ASSERT_EQUALS(outputSort.size(), 1U);
}

}  // namespace
}  // namespace mongo
