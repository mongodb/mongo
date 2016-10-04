/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/base/secure_allocator.h"

#include <array>

#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(SecureAllocator, SecureVector) {
    SecureVector<int> vec;

    vec->push_back(1);
    vec->push_back(2);

    ASSERT_EQUALS(1, (*vec)[0]);
    ASSERT_EQUALS(2, (*vec)[1]);

    vec->resize(2000, 3);
    ASSERT_EQUALS(3, (*vec)[2]);
}

TEST(SecureAllocator, SecureString) {
    SecureString str;

    str->resize(2000, 'x');
    ASSERT_EQUALS(0, str->compare(*SecureString(2000, 'x')));

    SecureString str2(str);
    ASSERT_NOT_EQUALS(&*str, &*str2);
    str2 = str;
    ASSERT_NOT_EQUALS(&*str, &*str2);

    auto strPtr = &*str;
    auto str2Ptr = &*str2;
    SecureString str3(std::move(str));
    ASSERT_EQUALS(strPtr, &*str3);
    str3 = std::move(str2);
    ASSERT_EQUALS(str2Ptr, &*str3);
}

// Verify that we can make a good number of secure objects.  Under the initial secure allocator
// design (page per object), you couldn't make more than 8-50 objects before running out of lockable
// pages.
TEST(SecureAllocator, ManySecureBytes) {
    std::array<SecureHandle<char>, 4096> chars;
    std::vector<SecureHandle<char>> e_chars(4096, 'e');
}

TEST(SecureAllocator, NonDefaultConstructibleWorks) {
    struct Foo {
        Foo(int) {}
        Foo() = delete;
    };

    SecureHandle<Foo> foo(10);
}

}  // namespace mongo
