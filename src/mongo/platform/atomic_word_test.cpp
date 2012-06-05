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

#include "mongo/platform/basic.h"

#include <iostream>

#include <boost/static_assert.hpp>

#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
    namespace {

        template <typename _AtomicWordType>
        void testAtomicWordBasicOperations() {
            typedef typename _AtomicWordType::WordType WordType;
            _AtomicWordType w;

            ASSERT_EQUALS(WordType(0), w.load());

            w.store(1);
            ASSERT_EQUALS(WordType(1), w.load());

            ASSERT_EQUALS(WordType(1), w.swap(2));
            ASSERT_EQUALS(WordType(2), w.load());

            ASSERT_EQUALS(WordType(2), w.compareAndSwap(0, 1));
            ASSERT_EQUALS(WordType(2), w.load());
            ASSERT_EQUALS(WordType(2), w.compareAndSwap(2, 1));
            ASSERT_EQUALS(WordType(1), w.load());

            ASSERT_EQUALS(WordType(1), w.fetchAndAdd(14));
            ASSERT_EQUALS(WordType(17), w.addAndFetch(2));
            ASSERT_EQUALS(WordType(16), w.subtractAndFetch(1));
            ASSERT_EQUALS(WordType(16), w.fetchAndSubtract(1));
            ASSERT_EQUALS(WordType(15), w.compareAndSwap(15, 0));
            ASSERT_EQUALS(WordType(0), w.load());
        }

        TEST(AtomicWordTests, BasicOperationsUnsigned32Bit) {
            typedef AtomicUInt32::WordType WordType;
            testAtomicWordBasicOperations<AtomicUInt32>();

            AtomicUInt32 w(0xdeadbeef);
            ASSERT_EQUALS(WordType(0xdeadbeef), w.compareAndSwap(0, 1));
            ASSERT_EQUALS(WordType(0xdeadbeef), w.compareAndSwap(0xdeadbeef, 0xcafe1234));
            ASSERT_EQUALS(WordType(0xcafe1234), w.fetchAndAdd(0xf000));
            ASSERT_EQUALS(WordType(0xcaff0234), w.swap(0));
            ASSERT_EQUALS(WordType(0), w.load());
        }

        TEST(AtomicWordTests, BasicOperationsUnsigned64Bit) {
            typedef AtomicUInt64::WordType WordType;
            testAtomicWordBasicOperations<AtomicUInt64>();

            AtomicUInt64 w(0xdeadbeefcafe1234ULL);
            ASSERT_EQUALS(WordType(0xdeadbeefcafe1234ULL), w.compareAndSwap(0, 1));
            ASSERT_EQUALS(WordType(0xdeadbeefcafe1234ULL),
                          w.compareAndSwap(0xdeadbeefcafe1234ULL, 0xfedcba9876543210ULL));
            ASSERT_EQUALS(WordType(0xfedcba9876543210ULL), w.fetchAndAdd(0xf0000000ULL));
            ASSERT_EQUALS(WordType(0xfedcba9966543210ULL), w.swap(0));
            ASSERT_EQUALS(WordType(0), w.load());
        }

        TEST(AtomicWordTests, BasicOperationsSigned32Bit) {
            typedef AtomicInt32::WordType WordType;
            testAtomicWordBasicOperations<AtomicInt32>();

            AtomicInt32 w(0xdeadbeef);
            ASSERT_EQUALS(WordType(0xdeadbeef), w.compareAndSwap(0, 1));
            ASSERT_EQUALS(WordType(0xdeadbeef), w.compareAndSwap(0xdeadbeef, 0xcafe1234));
            ASSERT_EQUALS(WordType(0xcafe1234), w.fetchAndAdd(0xf000));
            ASSERT_EQUALS(WordType(0xcaff0234), w.swap(0));
            ASSERT_EQUALS(WordType(0), w.load());
        }

        TEST(AtomicWordTests, BasicOperationsSigned64Bit) {
            typedef AtomicInt64::WordType WordType;
            testAtomicWordBasicOperations<AtomicInt64>();

            AtomicInt64 w(0xdeadbeefcafe1234ULL);
            ASSERT_EQUALS(WordType(0xdeadbeefcafe1234LL), w.compareAndSwap(0, 1));
            ASSERT_EQUALS(WordType(0xdeadbeefcafe1234LL),
                          w.compareAndSwap(0xdeadbeefcafe1234LL, 0xfedcba9876543210LL));
            ASSERT_EQUALS(WordType(0xfedcba9876543210LL), w.fetchAndAdd(0xf0000000LL));
            ASSERT_EQUALS(WordType(0xfedcba9966543210LL), w.swap(0));
            ASSERT_EQUALS(WordType(0), w.load());
        }

    }  // namespace
}  // namespace mongo
