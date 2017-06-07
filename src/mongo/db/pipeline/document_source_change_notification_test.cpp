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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_change_notification.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::vector;
using boost::intrusive_ptr;

using ChangeNotificationStageTest = AggregationContextFixture;

TEST_F(ChangeNotificationStageTest, Basic) {
    const auto spec = fromjson("{$changeNotification: {}}");

    vector<intrusive_ptr<DocumentSource>> result =
        DocumentSourceChangeNotification::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_EQUALS(result.size(), 3UL);

    const auto* matchStage = dynamic_cast<DocumentSourceMatch*>(result[0].get());
    ASSERT(matchStage);
    const std::string target = "unittests.pipeline_test";
    ASSERT_BSONOBJ_EQ(matchStage->getQuery(),
                      BSON("op" << BSON("$ne"
                                        << "n")
                                << "ts"
                                << BSON("$gt" << Timestamp())
                                << "$or"
                                << BSON_ARRAY(BSON("ns" << target) << BSON(
                                                  "op"
                                                  << "c"
                                                  << "$or"
                                                  << BSON_ARRAY(BSON("o.renameCollection" << target)
                                                                << BSON("o.to" << target))))));

    auto* sortStage = dynamic_cast<DocumentSourceSort*>(result[1].get());
    ASSERT(sortStage);
    BSONObjSet outputSort = sortStage->getOutputSorts();
    ASSERT_EQUALS(outputSort.count(BSON("ts" << -1)), 1U);
    ASSERT_EQUALS(outputSort.size(), 1U);

    const auto* limitStage = dynamic_cast<DocumentSourceLimit*>(result[2].get());
    ASSERT(limitStage);
    ASSERT_EQUALS(limitStage->getLimit(), 1);

    // TODO: Check explain result.
}

}  // namespace
}  // namespace mongo
