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

#include "mongo/dbtests/framework_options.h"

#include "mongo/bson/util/builder.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {
    // This specifies default dbpath for our testing framework
    const std::string default_test_dbpath = "/data/db/framework_options_test";
}

namespace {

    namespace moe = ::mongo::optionenvironment;

    TEST(Registration, RegisterAllOptions) {

        moe::OptionSection options;

        ASSERT_OK(::mongo::addTestFrameworkOptions(&options));

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(options.getAllOptions(&options_vector));

        for(std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
            iterator != options_vector.end(); iterator++) {

            if (iterator->_dottedName == "help") {
                ASSERT_EQUALS(iterator->_singleName, "help,h");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "show this usage information");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "dbpath") {
                ASSERT_EQUALS(iterator->_singleName, "dbpath");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "db data path for this test run. NOTE: the contents of this directory will be overwritten if it already exists");
                ASSERT_EQUALS(iterator->_isVisible, true);
                moe::Value defaultVal(default_test_dbpath);
                ASSERT_TRUE(iterator->_default.equal(defaultVal));
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "debug") {
                ASSERT_EQUALS(iterator->_singleName, "debug");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "run tests with verbose output");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "list") {
                ASSERT_EQUALS(iterator->_singleName, "list,l");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "list available test suites");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "bigfiles") {
                ASSERT_EQUALS(iterator->_singleName, "bigfiles");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "use big datafiles instead of smallfiles which is the default");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "filter") {
                ASSERT_EQUALS(iterator->_singleName, "filter,f");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "string substring filter on test name");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "verbose") {
                ASSERT_EQUALS(iterator->_singleName, "verbose,v");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "dur") {
                ASSERT_EQUALS(iterator->_singleName, "dur");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "enable journaling (currently the default)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "nodur") {
                ASSERT_EQUALS(iterator->_singleName, "nodur");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable journaling");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "seed") {
                ASSERT_EQUALS(iterator->_singleName, "seed");
                ASSERT_EQUALS(iterator->_type, moe::UnsignedLongLong);
                ASSERT_EQUALS(iterator->_description, "random number seed");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "runs") {
                ASSERT_EQUALS(iterator->_singleName, "runs");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "number of times to run each test");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "perfHist") {
                ASSERT_EQUALS(iterator->_singleName, "perfHist");
                ASSERT_EQUALS(iterator->_type, moe::Unsigned);
                ASSERT_EQUALS(iterator->_description, "number of back runs of perf stats to display");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "suites") {
                ASSERT_EQUALS(iterator->_singleName, "suites");
                ASSERT_EQUALS(iterator->_type, moe::StringVector);
                ASSERT_EQUALS(iterator->_description, "test suites to run");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, 1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "nopreallocj") {
                ASSERT_EQUALS(iterator->_singleName, "nopreallocj");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable journal prealloc");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else {
                ::mongo::StringBuilder sb;
                sb << "Found extra option: " << iterator->_dottedName <<
                      " which we did not register";
                FAIL(sb.str());
            }
        }
    }

} // namespace
