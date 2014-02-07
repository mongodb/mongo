/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/query/parsed_projection.h"

#include <memory>
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::auto_ptr;
    using std::string;
    using std::vector;

    using namespace mongo;

    //
    // creation function
    //

    ParsedProjection* createParsedProjection(const BSONObj& query, const BSONObj& projObj) {
        StatusWithMatchExpression swme = MatchExpressionParser::parse(query);
        ASSERT(swme.isOK());
        MatchExpression* queryMatchExpr = swme.getValue();
        ParsedProjection* out = NULL;
        Status status = ParsedProjection::make(projObj, queryMatchExpr, &out);
        if (!status.isOK()) {
            FAIL(mongoutils::str::stream() << "failed to parse projection " << projObj
                                           << " (query: " << query << "): " << status.toString());
        }
        ASSERT(out);
        return out;
    }

    ParsedProjection* createParsedProjection(const char* queryStr, const char* projStr) {
        BSONObj query = fromjson(queryStr);
        BSONObj projObj = fromjson(projStr);
        return createParsedProjection(query, projObj);
    }

    //
    // Failure to create a parsed projection is expected
    //

    void assertInvalidProjection(const char* queryStr, const char* projStr) {
        BSONObj query = fromjson(queryStr);
        BSONObj projObj = fromjson(projStr);
        StatusWithMatchExpression swme = MatchExpressionParser::parse(query);
        ASSERT(swme.isOK());
        MatchExpression* queryMatchExpr = swme.getValue();
        ParsedProjection* out = NULL;
        Status status = ParsedProjection::make(projObj, queryMatchExpr, &out);
        ASSERT(!status.isOK());
    }

    // canonical_query.cpp will invoke ParsedProjection::make only when
    // the projection spec is non-empty. This test case is included for
    // completeness and do not reflect actual usage.
    TEST(ParsedProjectionTest, MakeId) {
        auto_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{}"));
        ASSERT(parsedProj->requiresDocument());
    }

    TEST(ParsedProjectionTest, MakeEmpty) {
        auto_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{_id: 0}"));
        ASSERT(parsedProj->requiresDocument());
    }

    TEST(ParsedProjectionTest, MakeSingleField) {
        auto_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{a: 1}"));
        ASSERT(!parsedProj->requiresDocument());
        const vector<string>& fields = parsedProj->getRequiredFields();
        ASSERT_EQUALS(fields.size(), 2U);
        ASSERT_EQUALS(fields[0], "_id");
        ASSERT_EQUALS(fields[1], "a");
    }

    TEST(ParsedProjectionTest, MakeSingleFieldCovered) {
        auto_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{_id: 0, a: 1}"));
        ASSERT(!parsedProj->requiresDocument());
        const vector<string>& fields = parsedProj->getRequiredFields();
        ASSERT_EQUALS(fields.size(), 1U);
        ASSERT_EQUALS(fields[0], "a");
    }

    // boolean support is undocumented
    TEST(ParsedProjectionTest, MakeSingleFieldCoveredBoolean) {
        auto_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{_id: 0, a: true}"));
        ASSERT(!parsedProj->requiresDocument());
        const vector<string>& fields = parsedProj->getRequiredFields();
        ASSERT_EQUALS(fields.size(), 1U);
        ASSERT_EQUALS(fields[0], "a");
    }

    // boolean support is undocumented
    TEST(ParsedProjectionTest, MakeSingleFieldCoveredIdBoolean) {
        auto_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{_id: false, a: 1}"));
        ASSERT(!parsedProj->requiresDocument());
        const vector<string>& fields = parsedProj->getRequiredFields();
        ASSERT_EQUALS(fields.size(), 1U);
        ASSERT_EQUALS(fields[0], "a");
    }

    //
    // Positional operator validation
    //

    TEST(ParsedProjectionTest, InvalidPositionalOperatorProjections) {
        assertInvalidProjection("{}", "{'a.$': 1}");
        assertInvalidProjection("{a: 1}", "{'b.$': 1}");
        assertInvalidProjection("{a: 1}", "{'a.$': 0}");
        assertInvalidProjection("{a: 1}", "{'a.$.d.$': 1}");
        assertInvalidProjection("{a: 1}", "{'a.$.$': 1}");
        assertInvalidProjection("{a: 1}", "{'a.$.$': 1}");
        assertInvalidProjection("{a: 1, b: 1, c: 1}", "{'abc.$': 1}");
        assertInvalidProjection("{$or: [{a: 1}, {$or: [{b: 1}, {c: 1}]}]}", "{'d.$': 1}");
        assertInvalidProjection("{a: [1, 2, 3]}", "{'.$': 1}");
    }

    TEST(ParsedProjectionTest, ValidPositionalOperatorProjections) {
        createParsedProjection("{a: 1}", "{'a.$': 1}");
        createParsedProjection("{a: 1}", "{'a.foo.bar.$': 1}");
        createParsedProjection("{a: 1}", "{'a.foo.bar.$.x.y': 1}");
        createParsedProjection("{'a.b.c': 1}", "{'a.b.c.$': 1}");
        createParsedProjection("{'a.b.c': 1}", "{'a.e.f.$': 1}");
        createParsedProjection("{a: {b: 1}}", "{'a.$': 1}");
        createParsedProjection("{a: 1, b: 1}}", "{'a.$': 1}");
        createParsedProjection("{a: 1, b: 1}}", "{'b.$': 1}");
        createParsedProjection("{$and: [{a: 1}, {b: 1}]}", "{'a.$': 1}");
        createParsedProjection("{$and: [{a: 1}, {b: 1}]}", "{'b.$': 1}");
        createParsedProjection("{$or: [{a: 1}, {b: 1}]}", "{'a.$': 1}");
        createParsedProjection("{$or: [{a: 1}, {b: 1}]}", "{'b.$': 1}");
        createParsedProjection("{$and: [{$or: [{a: 1}, {$and: [{b: 1}, {c: 1}]}]}]}",
                               "{'c.d.f.$': 1}");
        // Fields with empty name can be projected using the positional $ operator.
        createParsedProjection("{'': [1, 2, 3]}", "{'.$': 1}");
    }

    // Some match expressions (eg. $where) do not override MatchExpression::path()
    // In this test case, we use an internal match expression implementation ALWAYS_FALSE
    // to achieve the same effect.
    // Projection parser should handle this the same way as an empty path.
    TEST(ParsedProjectionTest, InvalidPositionalProjectionDefaultPathMatchExpression) {
        auto_ptr<MatchExpression> queryMatchExpr(new FalseMatchExpression());
        ASSERT(NULL == queryMatchExpr->path().rawData());

        ParsedProjection* out = NULL;
        BSONObj projObj = fromjson("{'a.$': 1}");
        Status status = ParsedProjection::make(projObj, queryMatchExpr.get(), &out);
        ASSERT(!status.isOK());

        // Projecting onto empty field should fail.
        BSONObj emptyFieldProjObj = fromjson("{'.$': 1}");
        status = ParsedProjection::make(emptyFieldProjObj, queryMatchExpr.get(), &out);
        ASSERT(!status.isOK());
    }

    //
    // DBRef projections
    //

    TEST(ParsedProjectionTest, DBRefProjections) {
        // non-dotted
        createParsedProjection(BSONObj(), BSON( "$ref" <<  1));
        createParsedProjection(BSONObj(), BSON( "$id" <<  1));
        createParsedProjection(BSONObj(), BSON( "$ref" <<  1));
        // dotted before
        createParsedProjection("{}", "{'a.$ref': 1}");
        createParsedProjection("{}", "{'a.$id': 1}");
        createParsedProjection("{}", "{'a.$db': 1}");
        // dotted after
        createParsedProjection("{}", "{'$id.a': 1}");
        // position operator on $id
        // $ref and $db hold the collection and database names respectively,
        // so these fields cannot be arrays.
        createParsedProjection("{'a.$id': {$elemMatch: {x: 1}}}", "{'a.$id.$': 1}");

    }
} // unnamed namespace
