/*    Copyright 2013 10gen Inc.
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
#include "mongo/util/version.h"

namespace {

    using mongo::toVersionArray;

    TEST(VersionTest, NormalCase) {
        ASSERT_EQUALS( toVersionArray("1.2.3"), BSON_ARRAY(1 << 2 << 3 << 0) );
        ASSERT_EQUALS( toVersionArray("1.2.0"), BSON_ARRAY(1 << 2 << 0 << 0) );
        ASSERT_EQUALS( toVersionArray("2.0.0"), BSON_ARRAY(2 << 0 << 0 << 0) );
    }

    TEST(VersionTest, PreCase) {
        ASSERT_EQUALS( toVersionArray("1.2.3-pre-"), BSON_ARRAY(1 << 2 << 3 << -100) );
        ASSERT_EQUALS( toVersionArray("1.2.0-pre-"), BSON_ARRAY(1 << 2 << 0 << -100) );
        ASSERT_EQUALS( toVersionArray("2.0.0-pre-"), BSON_ARRAY(2 << 0 << 0 << -100) );
    }

    TEST(VersionTest, RcCase) {
        ASSERT_EQUALS( toVersionArray("1.2.3-rc0"), BSON_ARRAY(1 << 2 << 3 << -10) );
        ASSERT_EQUALS( toVersionArray("1.2.0-rc1"), BSON_ARRAY(1 << 2 << 0 << -9) );
        ASSERT_EQUALS( toVersionArray("2.0.0-rc2"), BSON_ARRAY(2 << 0 << 0 << -8) );
    }

    TEST(VersionTest, RcPreCase) {
        // Note that the pre of an rc is the same as the rc itself
        ASSERT_EQUALS( toVersionArray("1.2.3-rc3-pre-"), BSON_ARRAY(1 << 2 << 3 << -7) );
        ASSERT_EQUALS( toVersionArray("1.2.0-rc4-pre-"), BSON_ARRAY(1 << 2 << 0 << -6) );
        ASSERT_EQUALS( toVersionArray("2.0.0-rc5-pre-"), BSON_ARRAY(2 << 0 << 0 << -5) );
    }

} // namespace
