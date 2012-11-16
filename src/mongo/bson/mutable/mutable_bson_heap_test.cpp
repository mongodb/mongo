/* Copyright 2010 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/mutable_bson_heap.h"

#include <string>

#include "mongo/unittest/unittest.h"

namespace {

    TEST(BasicHeap, PutAndGetString) {
        mongo::mutablebson::BasicHeap heap;
        const std::string str = "my string";
        const char* anotherStr = "another string";

        ASSERT_EQUALS(heap.putString(str), 0U);
        ASSERT_EQUALS(heap.putString(anotherStr), str.size()+1);
        ASSERT_EQUALS(heap.getString(0), str);
        ASSERT_EQUALS(heap.getString(str.size()+1), std::string(anotherStr));
    }

} // unnamed namespace
