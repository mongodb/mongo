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

    TEST(Environment, DefaultValueIterateExplicit) {
        moe::Environment environment;
        ASSERT_OK(environment.setDefault(moe::Key("val1"), moe::Value(5)));
        ASSERT_OK(environment.setDefault(moe::Key("val2"), moe::Value(5)));
        ASSERT_OK(environment.set(moe::Key("val1"), moe::Value(6)));
        int val1;
        ASSERT_OK(environment.get(moe::Key("val1"), &val1));
        ASSERT_EQUALS(val1, 6);
        int val2;
        ASSERT_OK(environment.get(moe::Key("val2"), &val2));
        ASSERT_EQUALS(val2, 5);

        const std::map<moe::Key, moe::Value> values = environment.getExplicitlySet();
        ASSERT_EQUALS((static_cast<std::map<moe::Key, moe::Value>::size_type>(1)), values.size());

        typedef std::map<moe::Key, moe::Value>::const_iterator it_type;
        for(it_type iterator = values.begin();
            iterator != values.end(); iterator++) {
            ASSERT_EQUALS(moe::Key("val1"), iterator->first);
            int val1;
            ASSERT_OK(iterator->second.get(&val1));
            ASSERT_EQUALS(6, val1);
        }
    }

} // unnamed namespace
