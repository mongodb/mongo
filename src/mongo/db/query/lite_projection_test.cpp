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

#include "mongo/db/query/lite_projection.h"

#include <memory>
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::auto_ptr;
    using std::string;
    using std::vector;
    using mongo::BSONObj;
    using mongo::LiteProjection;
    using mongo::Status;
    using mongo::fromjson;

    //
    // creation function
    //

    LiteProjection* createLiteProjection(const char* queryStr, const char* projStr) {
        BSONObj query = fromjson(queryStr);
        BSONObj projObj = fromjson(projStr);
        LiteProjection* out = NULL;
        Status status = LiteProjection::make(query, projObj, &out);
        ASSERT(status.isOK());
        ASSERT(out);
        LiteProjection* liteProj(out);
        return liteProj;
    }

    TEST(LiteProjectionTest, MakeId) {
        auto_ptr<LiteProjection> liteProj(createLiteProjection("{}", "{}"));
        ASSERT(liteProj->requiresDocument());
        vector<string> fields;
        liteProj->getRequiredFields(&fields);
        ASSERT_EQUALS(fields.size(), 1U);
        ASSERT_EQUALS(fields[0], "_id");
    }

    TEST(LiteProjectionTest, MakeEmpty) {
        auto_ptr<LiteProjection> liteProj(createLiteProjection("{}", "{_id: 0}"));
        ASSERT(liteProj->requiresDocument());
        vector<string> fields;
        liteProj->getRequiredFields(&fields);
        ASSERT_EQUALS(fields.size(), 0U);
    }

    TEST(LiteProjectionTest, MakeSingleField) {
        auto_ptr<LiteProjection> liteProj(createLiteProjection("{}", "{a: 1}"));
        ASSERT(!liteProj->requiresDocument());
        vector<string> fields;
        liteProj->getRequiredFields(&fields);
        ASSERT_EQUALS(fields.size(), 2U);
        ASSERT_EQUALS(fields[0], "_id");
        ASSERT_EQUALS(fields[1], "a");
    }

    TEST(LiteProjectionTest, MakeSingleFieldCovered) {
        auto_ptr<LiteProjection> liteProj(createLiteProjection("{}", "{_id: 0, a: 1}"));
        ASSERT(!liteProj->requiresDocument());
        vector<string> fields;
        liteProj->getRequiredFields(&fields);
        ASSERT_EQUALS(fields.size(), 1U);
        ASSERT_EQUALS(fields[0], "a");
    }

    // boolean support is undocumented
    TEST(LiteProjectionTest, MakeSingleFieldCoveredBoolean) {
        auto_ptr<LiteProjection> liteProj(createLiteProjection("{}", "{_id: 0, a: true}"));
        ASSERT(!liteProj->requiresDocument());
        vector<string> fields;
        liteProj->getRequiredFields(&fields);
        ASSERT_EQUALS(fields.size(), 1U);
        ASSERT_EQUALS(fields[0], "a");
    }

    // boolean support is undocumented
    TEST(LiteProjectionTest, MakeSingleFieldCoveredIdBoolean) {
        auto_ptr<LiteProjection> liteProj(createLiteProjection("{}", "{_id: false, a: 1}"));
        ASSERT(!liteProj->requiresDocument());
        vector<string> fields;
        liteProj->getRequiredFields(&fields);
        ASSERT_EQUALS(fields.size(), 1U);
        ASSERT_EQUALS(fields[0], "a");
    }

} // unnamed namespace
