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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
namespace {
using std::vector;
using boost::intrusive_ptr;

class CountReturnsGroupAndProjectStages : public AggregationContextFixture {
public:
    void testCreateFromBsonResult(BSONObj countSpec) {
        vector<intrusive_ptr<DocumentSource>> result =
            DocumentSourceCount::createFromBson(countSpec.firstElement(), getExpCtx());

        ASSERT_EQUALS(result.size(), 2UL);

        const auto* groupStage = dynamic_cast<DocumentSourceGroup*>(result[0].get());
        ASSERT(groupStage);

        const auto* projectStage =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>(result[1].get());
        ASSERT(projectStage);

        const bool explain = true;
        vector<Value> explainedStages;
        groupStage->serializeToArray(explainedStages, explain);
        projectStage->serializeToArray(explainedStages, explain);
        ASSERT_EQUALS(explainedStages.size(), 2UL);

        StringData countName = countSpec.firstElement().valueStringData();
        Value expectedGroupExplain =
            Value{Document{{"_id", Document{{"$const", BSONNULL}}},
                           {countName, Document{{"$sum", Document{{"$const", 1}}}}}}};
        auto groupExplain = explainedStages[0];
        ASSERT_VALUE_EQ(groupExplain["$group"], expectedGroupExplain);

        Value expectedProjectExplain = Value{Document{{"_id", false}, {countName, true}}};
        auto projectExplain = explainedStages[1];
        ASSERT_VALUE_EQ(projectExplain["$project"], expectedProjectExplain);
    }
};

TEST_F(CountReturnsGroupAndProjectStages, ValidStringSpec) {
    BSONObj spec = BSON("$count"
                        << "myCount");
    testCreateFromBsonResult(spec);

    spec = BSON("$count"
                << "quantity");
    testCreateFromBsonResult(spec);
}

class InvalidCountSpec : public AggregationContextFixture {
public:
    vector<intrusive_ptr<DocumentSource>> createCount(BSONObj countSpec) {
        auto specElem = countSpec.firstElement();
        return DocumentSourceCount::createFromBson(specElem, getExpCtx());
    }
};

TEST_F(InvalidCountSpec, NonStringSpec) {
    BSONObj spec = BSON("$count" << 1);
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40156);

    spec = BSON("$count" << BSON("field1"
                                 << "test"));
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40156);
}

TEST_F(InvalidCountSpec, EmptyStringSpec) {
    BSONObj spec = BSON("$count"
                        << "");
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40157);
}

TEST_F(InvalidCountSpec, FieldPathSpec) {
    BSONObj spec = BSON("$count"
                        << "$x");
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40158);
}

TEST_F(InvalidCountSpec, EmbeddedNullByteSpec) {
    BSONObj spec = BSON("$count"
                        << "te\0st"_sd);
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40159);
}

TEST_F(InvalidCountSpec, PeriodInStringSpec) {
    BSONObj spec = BSON("$count"
                        << "test.string");
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40160);
}
}  // namespace
}  // namespace mongo
