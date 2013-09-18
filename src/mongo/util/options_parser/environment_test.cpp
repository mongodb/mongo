/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/bson/util/builder.h"

#include "mongo/util/options_parser/constraints.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::ErrorCodes;
    using mongo::Status;

    namespace moe = mongo::optionenvironment;

    TEST(Environment, EmptyValue) {
        moe::Environment environment;
        ASSERT_NOT_OK(environment.set(moe::Key("empty"), moe::Value()));
    }

    TEST(Environment, Immutable) {
        moe::Environment environment;
        environment.addKeyConstraint(new moe::ImmutableKeyConstraint(moe::Key("port")));
        ASSERT_OK(environment.set(moe::Key("port"), moe::Value(5)));
        ASSERT_OK(environment.validate());
        ASSERT_NOT_OK(environment.set(moe::Key("port"), moe::Value(0)));
    }

    TEST(Environment, OutOfRange) {
        moe::Environment environment;
        environment.addKeyConstraint(new moe::NumericKeyConstraint(moe::Key("port"), 1000, 65535));
        ASSERT_OK(environment.validate());
        ASSERT_NOT_OK(environment.set(moe::Key("port"), moe::Value(0)));
    }

    TEST(Environment, NonNumericRangeConstraint) {
        moe::Environment environment;
        environment.addKeyConstraint(new moe::NumericKeyConstraint(moe::Key("port"), 1000, 65535));
        ASSERT_OK(environment.validate());
        ASSERT_NOT_OK(environment.set(moe::Key("port"), moe::Value("string")));
    }

    TEST(Environment, BadType) {
        moe::Environment environment;
        environment.addKeyConstraint(new moe::TypeKeyConstraint<int>(moe::Key("port")));
        ASSERT_OK(environment.set(moe::Key("port"), moe::Value("string")));
        ASSERT_NOT_OK(environment.validate());
    }

    TEST(Environment, AllowNumeric) {
        moe::Environment environment;
        environment.addKeyConstraint(new moe::TypeKeyConstraint<long>(moe::Key("port")));
        environment.addKeyConstraint(new moe::TypeKeyConstraint<int>(moe::Key("port")));
        ASSERT_OK(environment.set(moe::Key("port"), moe::Value(1)));
        ASSERT_OK(environment.validate());
    }

    TEST(Environment, DirectTypeAccess) {
        moe::Environment environment;
        ASSERT_OK(environment.set(moe::Key("number"), moe::Value(5)));
        std::string notNumber;
        ASSERT_NOT_OK(environment.get(moe::Key("number"), &notNumber));
        int number;
        ASSERT_OK(environment.get(moe::Key("number"), &number));
        ASSERT_EQUALS(number, 5);
    }

    TEST(ToBSONTests, NormalValues) {
        moe::Environment environment;
        ASSERT_OK(environment.set(moe::Key("val1"), moe::Value(6)));
        ASSERT_OK(environment.set(moe::Key("val2"), moe::Value(std::string("string"))));
        mongo::BSONObj obj = BSON( "val1" << 6 << "val2" << "string" );
        // TODO: Put a comparison here that doesn't depend on the field order.  Right now it is
        // based on the sort order of keys in a std::map.
        ASSERT_EQUALS(obj, environment.toBSON());
    }

    TEST(ToBSONTests, DottedValues) {
        moe::Environment environment;
        ASSERT_OK(environment.set(moe::Key("val1.dotted1"), moe::Value(6)));
        ASSERT_OK(environment.set(moe::Key("val2"), moe::Value(true)));
        ASSERT_OK(environment.set(moe::Key("val1.dotted2"), moe::Value(std::string("string"))));
        mongo::BSONObj obj = BSON("val1" << BSON( "dotted1" << 6 << "dotted2" << "string") <<
                                  "val2" << true );
        // TODO: Put a comparison here that doesn't depend on the field order.  Right now it is
        // based on the sort order of keys in a std::map.
        ASSERT_EQUALS(obj, environment.toBSON());
    }

    TEST(ToBSONTests, DeepDottedValues) {
        moe::Environment environment;
        ASSERT_OK(environment.set(moe::Key("val1.first1.second1.third1"), moe::Value(6)));
        ASSERT_OK(environment.set(moe::Key("val1.first1.second2.third1"), moe::Value(false)));
        ASSERT_OK(environment.set(moe::Key("val1.first2"), moe::Value(std::string("string"))));
        ASSERT_OK(environment.set(moe::Key("val1.first1.second1.third2"), moe::Value(true)));
        ASSERT_OK(environment.set(moe::Key("val2"), moe::Value(6.0)));
        mongo::BSONObj obj = BSON("val1" << BSON("first1" <<
                                                 BSON("second1" <<
                                                      BSON("third1" << 6 << "third2" << true) <<
                                                      "second2" <<
                                                 BSON("third1" << false)) <<
                                                 "first2" << "string") <<
                                  "val2" << 6.0);
        // TODO: Put a comparison here that doesn't depend on the field order.  Right now it is
        // based on the sort order of keys in a std::map.
        ASSERT_EQUALS(obj, environment.toBSON());
    }
} // unnamed namespace
