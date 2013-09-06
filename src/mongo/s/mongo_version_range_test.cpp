/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/s/mongo_version_range.h"

#include <string>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/unittest/unittest.h"

/**
 * Basic tests for config version parsing.
 */

namespace mongo {
namespace {

    using std::string;
    using std::vector;

    //
    // Test mongo version ranges
    //

    TEST(Excludes, Empty) {

        //
        // Tests basic empty range
        //

        BSONArray includeArr;
        vector<MongoVersionRange> includes;

        string errMsg;
        MongoVersionRange::parseBSONArray(includeArr, &includes, &errMsg);
        ASSERT_EQUALS(errMsg, "");

        // Make sure nothing is included
        ASSERT(!isInMongoVersionRanges("1.2.3", includes));
        ASSERT(!isInMongoVersionRanges("1.2.3-pre", includes));
        ASSERT(!isInMongoVersionRanges("1.2.3-rc0", includes));
    }

    TEST(Excludes, SinglePointRange) {

        //
        // Tests single string range
        //

        BSONArrayBuilder bab;
        bab << "1.2.3";
        BSONArray includeArr = bab.arr();

        vector<MongoVersionRange> includes;

        string errMsg;
        MongoVersionRange::parseBSONArray(includeArr, &includes, &errMsg);
        ASSERT_EQUALS(errMsg, "");

        ASSERT(isInMongoVersionRanges("1.2.3", includes));

        ASSERT(!isInMongoVersionRanges("1.2.2-rc0", includes));
        ASSERT(!isInMongoVersionRanges("1.2.2", includes));

        ASSERT(isInMongoVersionRanges("1.2.3-pre", includes));
        ASSERT(isInMongoVersionRanges("1.2.3-rc0", includes));

        ASSERT(!isInMongoVersionRanges("1.2.4-rc0", includes));
        ASSERT(!isInMongoVersionRanges("1.2.4", includes));
    }

    TEST(Excludes, BetweenRange) {

        //
        // Tests range with two endpoints
        //

        BSONArrayBuilder bab;
        bab << BSON_ARRAY("7.8.9" << "10.11.12");
        BSONArray includeArr = bab.arr();

        vector<MongoVersionRange> includes;

        string errMsg;
        MongoVersionRange::parseBSONArray(includeArr, &includes, &errMsg);
        ASSERT_EQUALS(errMsg, "");

        ASSERT(isInMongoVersionRanges("7.8.9", includes));
        ASSERT(isInMongoVersionRanges("10.11.12", includes));

        // Before
        ASSERT(!isInMongoVersionRanges("7.8.8-rc0", includes));
        ASSERT(!isInMongoVersionRanges("7.8.8", includes));

        // Boundary
        ASSERT(isInMongoVersionRanges("7.8.9-pre", includes));
        ASSERT(isInMongoVersionRanges("7.8.9-rc0", includes));

        ASSERT(isInMongoVersionRanges("7.8.10-rc0", includes));
        ASSERT(isInMongoVersionRanges("7.8.10", includes));

        // Between
        ASSERT(isInMongoVersionRanges("8.9.10", includes));
        ASSERT(isInMongoVersionRanges("9.10.11", includes));

        // Boundary
        ASSERT(isInMongoVersionRanges("10.11.11-rc0", includes));
        ASSERT(isInMongoVersionRanges("10.11.11", includes));

        ASSERT(isInMongoVersionRanges("10.11.12-pre", includes));
        ASSERT(isInMongoVersionRanges("10.11.12-rc0", includes));

        // After
        ASSERT(!isInMongoVersionRanges("10.11.13-rc0", includes));
        ASSERT(!isInMongoVersionRanges("10.11.13", includes));

    }

    TEST(Excludes, WeirdRange) {

        //
        // Tests range with rc/pre endpoints
        //

        BSONArrayBuilder bab;
        bab << BSON_ARRAY("7.8.9-rc0" << "10.11.12-pre");
        BSONArray includeArr = bab.arr();

        vector<MongoVersionRange> includes;

        string errMsg;
        MongoVersionRange::parseBSONArray(includeArr, &includes, &errMsg);
        ASSERT_EQUALS(errMsg, "");

        // Near endpoints
        ASSERT(isInMongoVersionRanges("7.8.9", includes));
        ASSERT(!isInMongoVersionRanges("10.11.12", includes));

        // Before
        ASSERT(!isInMongoVersionRanges("7.8.8-rc0", includes));
        ASSERT(!isInMongoVersionRanges("7.8.8", includes));

        // Boundary
        ASSERT(!isInMongoVersionRanges("7.8.9-pre", includes));
        ASSERT(isInMongoVersionRanges("7.8.9-rc0", includes));

        ASSERT(isInMongoVersionRanges("7.8.10-rc0", includes));
        ASSERT(isInMongoVersionRanges("7.8.10", includes));

        // Between
        ASSERT(isInMongoVersionRanges("8.9.10", includes));
        ASSERT(isInMongoVersionRanges("9.10.11", includes));

        // Boundary
        ASSERT(isInMongoVersionRanges("10.11.11-rc0", includes));
        ASSERT(isInMongoVersionRanges("10.11.11", includes));

        ASSERT(isInMongoVersionRanges("10.11.12-pre", includes));
        ASSERT(!isInMongoVersionRanges("10.11.12-rc0", includes));

        // After
        ASSERT(!isInMongoVersionRanges("10.11.13-rc0", includes));
        ASSERT(!isInMongoVersionRanges("10.11.13", includes));
    }

    TEST(Excludes, BadRangeType) {

        //
        // Tests range with bad endpoint types
        //

        BSONArrayBuilder bab;
        bab << "1.2.3";
        bab << 2; // integer is not version
        BSONArray includeArr = bab.arr();

        vector<MongoVersionRange> includes;

        string errMsg;
        MongoVersionRange::parseBSONArray(includeArr, &includes, &errMsg);
        ASSERT_NOT_EQUALS(errMsg, "");
    }

    TEST(Excludes, BadRangeArray) {

        //
        // Tests range with bad array
        //

        BSONArrayBuilder bab;
        bab << BSON_ARRAY("" << "1.2.3"); // empty bound
        BSONArray includeArr = bab.arr();

        vector<MongoVersionRange> includes;

        string errMsg;
        MongoVersionRange::parseBSONArray(includeArr, &includes, &errMsg);
        ASSERT_NOT_EQUALS(errMsg, "");
    }

}
} // namespace mongo

