/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/secure_allocator.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/processinfo.h"

#include <array>
#include <cstdint>

namespace mongo {

TEST(SecureAllocator, SecureVector) {
    SecureAllocatorDefaultDomain::SecureVector<int> vec;

    vec->push_back(1);
    vec->push_back(2);

    ASSERT_EQUALS(1, (*vec)[0]);
    ASSERT_EQUALS(2, (*vec)[1]);

    vec->resize(2000, 3);
    ASSERT_EQUALS(3, (*vec)[2]);
}

TEST(SecureAllocator, SecureString) {
    SecureAllocatorDefaultDomain::SecureString str;

    str->resize(2000, 'x');
    ASSERT_EQUALS(0, str->compare(*SecureAllocatorDefaultDomain::SecureString(2000, 'x')));

    SecureAllocatorDefaultDomain::SecureString str2(str);
    ASSERT_NOT_EQUALS(&*str, &*str2);
    str2 = str;
    ASSERT_NOT_EQUALS(&*str, &*str2);

    auto strPtr = &*str;
    auto str2Ptr = &*str2;
    SecureAllocatorDefaultDomain::SecureString str3(std::move(str));
    ASSERT_EQUALS(strPtr, &*str3);
    str3 = std::move(str2);
    ASSERT_EQUALS(str2Ptr, &*str3);
}

TEST(SecureAllocator, SecureArray) {
    SecureAllocatorDefaultDomain::SecureArray<uint8_t, 100> array1;
    SecureArray<uint8_t, 100> array2;

    (*array1)[0] = 1;
    (*array2)[0] = 1;

    ASSERT_EQUALS(100, array1->size());
    ASSERT_EQUALS(100, array2->size());

    ASSERT_NOT_EQUALS(array1->data(), array2->data());

    ASSERT_EQUALS(*array1->data(), *array2->data());
}

// Verify that we can make a good number of secure objects.  Under the initial secure allocator
// design (page per object), you couldn't make more than 8-50 objects before running out of lockable
// pages.
TEST(SecureAllocator, ManySecureBytes) {
    std::array<SecureAllocatorDefaultDomain::SecureHandle<char>, 4096> chars;
    std::vector<SecureAllocatorDefaultDomain::SecureHandle<char>> e_chars(4096, 'e');
}

TEST(SecureAllocator, NonDefaultConstructibleWorks) {
    struct Foo {
        Foo(int) {}
        Foo() = delete;
    };

    SecureAllocatorDefaultDomain::SecureHandle<Foo> foo(10);
}

TEST(SecureAllocator, allocatorCanBeDisabled) {
    static size_t pegInvokationCountLast;
    static size_t pegInvokationCount;
    pegInvokationCountLast = 0;
    pegInvokationCount = 0;
    struct UnsecureAllocatorTrait {
        static bool peg() {
            pegInvokationCount++;

            return false;
        }
    };
    using UnsecureAllocatorDomain = SecureAllocatorDomain<UnsecureAllocatorTrait>;

    {
        std::vector<UnsecureAllocatorDomain::SecureHandle<char>> more_e_chars(4096, 'e');
        ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
        pegInvokationCountLast = pegInvokationCount;

        UnsecureAllocatorDomain::SecureString str;
        ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
        pegInvokationCountLast = pegInvokationCount;

        str->resize(2000, 'x');
        ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
        pegInvokationCountLast = pegInvokationCount;

        ASSERT_EQUALS(0, str->compare(*UnsecureAllocatorDomain::SecureString(2000, 'x')));
        ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
        pegInvokationCountLast = pegInvokationCount;
    }

    ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
}

/**
 * This class tests the secure allocation by the following steps:
 * Allocating first half a page and checking the correctness of the paged byte accocation count and
 * byte allocation count. Allocating another half a page and checking the correctness of allocation
 * byte counts. Deallocating all allocations and checking the correcness of allocation byte counts.
 */
TEST(SecureAllocator, secureAllocBytesCount) {
    ProcessInfo p;
    auto pageSize = p.getPageSize();
    using namespace mongo::secure_allocator_details;
    // The following allocation is to align the remaining test allocations to the start of a page.
    void* ptr0 = allocate(pageSize, alignof(uint8_t));
    uint32_t initAllocCnt = gSecureAllocCountInfo().getSecureAllocByteCount();
    uint32_t allocCnt = initAllocCnt;
    uint32_t expectedPageBytesCnt;
    uint32_t initPageBytesCnt = gSecureAllocCountInfo().getSecureAllocBytesInPages();

    ASSERT_EQUALS(initAllocCnt, pageSize);

    // The first allocation: allocating half a page.
    auto halfPageSize = pageSize / 2;
    void* ptr1 = allocate(halfPageSize, alignof(uint8_t));

    // initPageBytesCnt is the current count in paged byte allocation.
    // If we have sufficient bytes for the current allocation, we do not need to allocate more
    // pages. If we do not have enough bytes, we should allocate a new page.
    expectedPageBytesCnt = initPageBytesCnt + pageSize;

    // Checking the correctness of the paged byte accocation count and byte allocation count.
    allocCnt = gSecureAllocCountInfo().getSecureAllocByteCount();
    ASSERT_EQUALS(halfPageSize + initAllocCnt, allocCnt);
    ASSERT_EQUALS(expectedPageBytesCnt, gSecureAllocCountInfo().getSecureAllocBytesInPages());

    // The second allocation: allocating half a page.
    void* ptr2 = allocate(halfPageSize, alignof(uint8_t));

    // Checking the correctness of the byte counts after the second allocation.
    ASSERT_EQUALS(pageSize + initAllocCnt, gSecureAllocCountInfo().getSecureAllocByteCount());
    ASSERT_EQUALS(expectedPageBytesCnt, gSecureAllocCountInfo().getSecureAllocBytesInPages());

    deallocate(ptr1, halfPageSize);
    deallocate(ptr2, halfPageSize);

    // Checking the correctness of byte counts after the deallocations.
    ASSERT_EQUALS(initAllocCnt, gSecureAllocCountInfo().getSecureAllocByteCount());
    deallocate(ptr0, pageSize);
}

}  // namespace mongo
