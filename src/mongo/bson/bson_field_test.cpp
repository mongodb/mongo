/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONField;
    using mongo::BSONObj;

    TEST(Assignment, Simple) {
        BSONField<int> x("x");
        BSONObj o = BSON(x << 5);
        ASSERT_EQUALS(BSON("x" << 5), o);
    }

    TEST(Make, Simple) {
        BSONField<int> x("x");
        BSONObj o = BSON(x.make(5));
        ASSERT_EQUALS(BSON("x" << 5), o);
    }

    TEST(Query, GreaterThan) {
        BSONField<int> x("x");
        BSONObj o = BSON(x(5));
        ASSERT_EQUALS(BSON("x" << 5), o);

        o = BSON(x.gt(5));
        ASSERT_EQUALS(BSON("x" << BSON("$gt" << 5)), o);
    }

    TEST(Query, NotEqual) {
        BSONField<int> x("x");
        BSONObj o = BSON(x(10));
        ASSERT_EQUALS(BSON("x" << 10), o);

        o = BSON(x.ne(5));
        ASSERT_EQUALS(BSON("x" << BSON("$ne" << 5)), o);
    }

} // unnamed namespace
