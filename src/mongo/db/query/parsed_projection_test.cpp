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
 */

#include "mongo/db/query/parsed_projection.h"

#include <memory>
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::auto_ptr;
    using std::string;
    using std::vector;
    using mongo::BSONObj;
    using mongo::ParsedProjection;
    using mongo::Status;
    using mongo::fromjson;

    //
    // creation function
    //

    ParsedProjection* createParsedProjection(const char* queryStr, const char* projStr) {
        BSONObj query = fromjson(queryStr);
        BSONObj projObj = fromjson(projStr);
        ParsedProjection* out = NULL;
        Status status = ParsedProjection::make(projObj, query, &out);
        ASSERT(status.isOK());
        ASSERT(out);
        return out;
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

} // unnamed namespace
