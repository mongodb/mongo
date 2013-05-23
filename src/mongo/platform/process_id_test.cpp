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

#include "mongo/platform/basic.h"

#include "mongo/platform/process_id.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

    TEST(ProcessId, Comparison) {
        const ProcessId p1 = ProcessId::fromNative(NativeProcessId(1));
        const ProcessId p2 = ProcessId::fromNative(NativeProcessId(2));

        ASSERT_FALSE(p1 == p2);
        ASSERT_TRUE(p1 == p1);

        ASSERT_TRUE(p1 != p2);
        ASSERT_FALSE(p1 != p1);

        ASSERT_TRUE(p1 < p2);
        ASSERT_FALSE(p1 < p1);
        ASSERT_FALSE(p2 < p1);

        ASSERT_TRUE(p1 <= p2);
        ASSERT_TRUE(p1 <= p1);
        ASSERT_FALSE(p2 <= p1);

        ASSERT_TRUE(p2 > p1);
        ASSERT_FALSE(p2 > p2);
        ASSERT_FALSE(p1 > p2);

        ASSERT_TRUE(p2 >= p1);
        ASSERT_TRUE(p2 >= p2);
        ASSERT_FALSE(p1 >= p2);
    }

    TEST(ProcessId, GetCurrentEqualsSelf) {
        ASSERT_EQUALS(ProcessId::getCurrent(), ProcessId::getCurrent());
    }

}  // namespace
}  // namespace mongo
