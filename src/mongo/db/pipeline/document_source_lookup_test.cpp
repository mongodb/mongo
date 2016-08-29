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
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
namespace {
using boost::intrusive_ptr;
using std::vector;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceLookUpTest = AggregationContextFixture;

TEST_F(DocumentSourceLookUpTest, ShouldTruncateOutputSortOnAsField) {
    intrusive_ptr<DocumentSourceMock> source = DocumentSourceMock::create();
    source->sorts = {BSON("a" << 1 << "d.e" << 1 << "c" << 1)};
    auto lookup = DocumentSourceLookUp::createFromBson(
        Document{
            {"$lookup",
             Document{{"from", "a"}, {"localField", "b"}, {"foreignField", "c"}, {"as", "d.e"}}}}
            .toBson()
            .firstElement(),
        getExpCtx());
    lookup->setSource(source.get());

    BSONObjSet outputSort = lookup->getOutputSorts();

    ASSERT_EQUALS(outputSort.count(BSON("a" << 1)), 1U);
    ASSERT_EQUALS(outputSort.size(), 1U);
}

TEST_F(DocumentSourceLookUpTest, ShouldTruncateOutputSortOnSuffixOfAsField) {
    intrusive_ptr<DocumentSourceMock> source = DocumentSourceMock::create();
    source->sorts = {BSON("a" << 1 << "d.e" << 1 << "c" << 1)};
    auto lookup = DocumentSourceLookUp::createFromBson(
        Document{{"$lookup",
                  Document{{"from", "a"}, {"localField", "b"}, {"foreignField", "c"}, {"as", "d"}}}}
            .toBson()
            .firstElement(),
        getExpCtx());
    lookup->setSource(source.get());

    BSONObjSet outputSort = lookup->getOutputSorts();

    ASSERT_EQUALS(outputSort.count(BSON("a" << 1)), 1U);
    ASSERT_EQUALS(outputSort.size(), 1U);
}

TEST(MakeMatchStageFromInput, NonArrayValueUsesEqQuery) {
    auto input = Document{{"local", 1}};
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(matchStage, fromjson("{$match: {$and: [{foreign: {$eq: 1}}, {}]}}"));
}

TEST(MakeMatchStageFromInput, RegexValueUsesEqQuery) {
    BSONRegEx regex("^a");
    Document input = DOC("local" << Value(regex));
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(
        matchStage,
        BSON("$match" << BSON(
                 "$and" << BSON_ARRAY(BSON("foreign" << BSON("$eq" << regex)) << BSONObj()))));
}

TEST(MakeMatchStageFromInput, ArrayValueUsesInQuery) {
    vector<Value> inputArray = {Value(1), Value(2)};
    Document input = DOC("local" << Value(inputArray));
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(matchStage, fromjson("{$match: {$and: [{foreign: {$in: [1, 2]}}, {}]}}"));
}

TEST(MakeMatchStageFromInput, ArrayValueWithRegexUsesOrQuery) {
    BSONRegEx regex("^a");
    vector<Value> inputArray = {Value(1), Value(regex), Value(2)};
    Document input = DOC("local" << Value(inputArray));
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(
        matchStage,
        BSON("$match" << BSON(
                 "$and" << BSON_ARRAY(
                     BSON("$or" << BSON_ARRAY(BSON("foreign" << BSON("$eq" << Value(1)))
                                              << BSON("foreign" << BSON("$eq" << regex))
                                              << BSON("foreign" << BSON("$eq" << Value(2)))))
                     << BSONObj()))));
}

}  // namespace
}  // namespace mongo
