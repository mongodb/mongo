/*    Copyright 2012 10gen Inc.
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

#include "mongo/platform/basic.h"

#include <iostream>


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
