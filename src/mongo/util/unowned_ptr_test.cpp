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

#include "mongo/util/unowned_ptr.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
static int* const aNullPtr = nullptr;

TEST(UnownedPtr, Construction) {
    // non-const
    std::unique_ptr<int> p1(new int(1));
    std::shared_ptr<int> p2(new int(2));
    std::unique_ptr<int> p3(new int(3));
    std::shared_ptr<int> p4(new int(4));

    ASSERT_EQUALS(aNullPtr, unowned_ptr<int>());
    ASSERT_EQUALS(aNullPtr, unowned_ptr<int>({}));
    ASSERT_EQUALS(aNullPtr, unowned_ptr<int>(nullptr));
    ASSERT_EQUALS(aNullPtr, unowned_ptr<int>(NULL));
    ASSERT_EQUALS(p1.get(), unowned_ptr<int>(p1.get()));
    ASSERT_EQUALS(p1.get(), unowned_ptr<int>(p1));
    ASSERT_EQUALS(p2.get(), unowned_ptr<int>(p2));
    ASSERT_EQUALS(p2.get(), unowned_ptr<int>(unowned_ptr<int>(p2)));

    // const
    std::unique_ptr<const int> cp1(new int(11));
    std::shared_ptr<const int> cp2(new int(12));

    ASSERT_EQUALS(aNullPtr, unowned_ptr<const int>());
    ASSERT_EQUALS(aNullPtr, unowned_ptr<const int>({}));
    ASSERT_EQUALS(aNullPtr, unowned_ptr<const int>(nullptr));
    ASSERT_EQUALS(aNullPtr, unowned_ptr<const int>(NULL));
    ASSERT_EQUALS(p1.get(), unowned_ptr<const int>(p1.get()));
    ASSERT_EQUALS(cp1.get(), unowned_ptr<const int>(cp1.get()));
    ASSERT_EQUALS(p1.get(), unowned_ptr<const int>(p1));
    ASSERT_EQUALS(p2.get(), unowned_ptr<const int>(p2));
    ASSERT_EQUALS(p3.get(), unowned_ptr<const int>(p3));
    ASSERT_EQUALS(p4.get(), unowned_ptr<const int>(p4));
    ASSERT_EQUALS(cp1.get(), unowned_ptr<const int>(cp1));
    ASSERT_EQUALS(cp2.get(), unowned_ptr<const int>(cp2));
    ASSERT_EQUALS(p2.get(), unowned_ptr<const int>(unowned_ptr<const int>(p2)));
    ASSERT_EQUALS(p2.get(), unowned_ptr<const int>(unowned_ptr<int>(p2)));

    // These shouldn't compile since they'd drop constness:
    //(void)unowned_ptr<int>(cp1);
    //(void)unowned_ptr<int>(cp2);
    //(void)unowned_ptr<int>(unowned_ptr<const int>(p2));
}

TEST(UnownedPtr, Assignment) {
    // non-const
    std::unique_ptr<int> p1(new int(1));
    std::shared_ptr<int> p2(new int(2));
    std::unique_ptr<int> p3(new int(3));
    std::shared_ptr<int> p4(new int(4));

    ASSERT_EQUALS(aNullPtr, (unowned_ptr<int>() = {}));
    ASSERT_EQUALS(aNullPtr, (unowned_ptr<int>() = nullptr));
    ASSERT_EQUALS(aNullPtr, (unowned_ptr<int>() = NULL));
    ASSERT_EQUALS(p1.get(), (unowned_ptr<int>() = p1.get()));
    ASSERT_EQUALS(p1.get(), (unowned_ptr<int>() = p1));
    ASSERT_EQUALS(p2.get(), (unowned_ptr<int>() = p2));
    ASSERT_EQUALS(p2.get(), (unowned_ptr<int>() = unowned_ptr<int>(p2)));

    // const
    std::unique_ptr<const int> cp1(new int(11));
    std::shared_ptr<const int> cp2(new int(12));

    ASSERT_EQUALS(aNullPtr, (unowned_ptr<const int>() = {}));
    ASSERT_EQUALS(aNullPtr, (unowned_ptr<const int>() = nullptr));
    ASSERT_EQUALS(aNullPtr, (unowned_ptr<const int>() = NULL));
    ASSERT_EQUALS(p1.get(), (unowned_ptr<const int>() = p1.get()));
    ASSERT_EQUALS(cp1.get(), (unowned_ptr<const int>() = cp1.get()));
    ASSERT_EQUALS(p1.get(), (unowned_ptr<const int>() = p1));
    ASSERT_EQUALS(p2.get(), (unowned_ptr<const int>() = p2));
    ASSERT_EQUALS(p3.get(), (unowned_ptr<const int>() = p3));
    ASSERT_EQUALS(p4.get(), (unowned_ptr<const int>() = p4));
    ASSERT_EQUALS(cp1.get(), (unowned_ptr<const int>() = cp1));
    ASSERT_EQUALS(cp2.get(), (unowned_ptr<const int>() = cp2));
    ASSERT_EQUALS(p2.get(), (unowned_ptr<const int>() = unowned_ptr<const int>(p2)));
    ASSERT_EQUALS(p2.get(), (unowned_ptr<const int>() = unowned_ptr<int>(p2)));

    // These shouldn't compile since they'd drop constness:
    // unowned_ptr<int>() = cp1;
    // unowned_ptr<int>() = cp2;
    // unowned_ptr<int>() = unowned_ptr<const int>(p2);
}

TEST(UnownedPtr, ArgumentOverloading) {
    struct Base {
    } base;
    struct Derived : Base {
    } derived;
    struct Other {
    } other;

    struct {
        StringData operator()(unowned_ptr<Base>) {
            return "base";
        }
        StringData operator()(unowned_ptr<Other>) {
            return "other";
        }
        // Unfortunately unowned_ptr<Derived> would be ambiguous. You can only overload on
        // unrelated types.
    } overloadedFunction;

    ASSERT_EQ(overloadedFunction(&base), "base");
    ASSERT_EQ(overloadedFunction(&derived), "base");
    ASSERT_EQ(overloadedFunction(&other), "other");
}

TEST(UnownedPtr, Boolishness) {
    ASSERT_FALSE(unowned_ptr<const char>());
    ASSERT_TRUE(unowned_ptr<const char>(""));
}

TEST(UnownedPtr, Equality) {
    int i = 0;
    int j = 0;

    ASSERT_EQ(unowned_ptr<int>(), unowned_ptr<int>());      // NULL
    ASSERT_EQ(unowned_ptr<int>(&i), unowned_ptr<int>(&i));  // non-NULL

    ASSERT_NE(unowned_ptr<int>(), unowned_ptr<int>(&i));    // NULL != non-NULL
    ASSERT_NE(unowned_ptr<int>(&i), unowned_ptr<int>(&j));  // two distinct non-NULLs
}
}
