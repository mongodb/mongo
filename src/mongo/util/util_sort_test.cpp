/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/util/sort.h"

#include <array>
#include <utility>

namespace mongo {
namespace {

template <typename T, typename Compare = std::less<>>
constexpr bool testIsSorted(const T& arr, Compare comp = {}) {
    using std::begin;
    using std::end;
    return constexprIsSorted(begin(arr), end(arr), comp);
}

template <typename T, typename Compare = std::less<>>
constexpr bool testIsSortedUntil(const T& arr, size_t until, Compare comp = {}) {
    using std::begin;
    using std::next;
    return constexprIsSorted(begin(arr), next(begin(arr), until), comp);
}

template <typename T, typename... As>
constexpr auto mkArr(As... a) {
    return std::array<T, sizeof...(a)>{T{a}...};
}

static_assert(testIsSorted(mkArr<int>()));
static_assert(testIsSorted(mkArr<int>(1)));
static_assert(testIsSorted(mkArr<int>(1, 1)));
static_assert(testIsSorted(mkArr<int>(-1, 0, 1)));
static_assert(!testIsSorted(mkArr<int>(1, 0)));
static_assert(!testIsSorted(mkArr<int>(0, -1)));
static_assert(!testIsSorted(mkArr<int>(3, 4, 3)));
static_assert(!testIsSorted(mkArr<int>(3, 2, 3)));

static_assert(testIsSortedUntil(mkArr<int>(1, 2, 3, 2, 1), 3));
static_assert(!testIsSortedUntil(mkArr<int>(1, 2, 3, 2, 1), 4));

class OnlyHasLess {
public:
    constexpr explicit OnlyHasLess(int i) : i(i) {}
    friend constexpr bool operator<(const OnlyHasLess& lhs, const OnlyHasLess& rhs) {
        return lhs.i < rhs.i;
    }

    int i;
};

static_assert(testIsSorted(mkArr<OnlyHasLess>(1, 2, 3)));
static_assert(!testIsSorted(mkArr<OnlyHasLess>(3, 2, 1)));

static_assert(testIsSortedUntil(mkArr<OnlyHasLess>(1, 2, 3, 2), 3));
static_assert(!testIsSortedUntil(mkArr<OnlyHasLess>(1, 2, 3, 2), 4));

/**
 * Example stateful comparator to change less than to greater than if desired.
 */
class StatefulComp {
public:
    constexpr explicit StatefulComp(bool greater) : _greater(greater) {}
    constexpr bool operator()(const OnlyHasLess& lhs, const OnlyHasLess& rhs) const {
        return _greater ? rhs < lhs : lhs < rhs;
    }

private:
    bool _greater;
};

static_assert(testIsSorted(mkArr<OnlyHasLess>(1), StatefulComp(true)));
static_assert(testIsSorted(mkArr<OnlyHasLess>(1, 1), StatefulComp(true)));
static_assert(testIsSorted(mkArr<OnlyHasLess>(1, 1), StatefulComp(false)));
static_assert(testIsSorted(mkArr<OnlyHasLess>(1, 2, 3), StatefulComp(false)));
static_assert(!testIsSorted(mkArr<OnlyHasLess>(1, 2, 3), StatefulComp(true)));
static_assert(!testIsSorted(mkArr<OnlyHasLess>(1, 2, 3, 2, 1), StatefulComp(true)));
static_assert(testIsSorted(mkArr<OnlyHasLess>(1, 0), StatefulComp(true)));
static_assert(!testIsSorted(mkArr<OnlyHasLess>(1, 0), StatefulComp(false)));
static_assert(testIsSorted(mkArr<OnlyHasLess>(3, 1, 0), StatefulComp(true)));

static_assert(testIsSortedUntil(mkArr<OnlyHasLess>(1, 2, 1), 2, StatefulComp(false)));
static_assert(!testIsSortedUntil(mkArr<OnlyHasLess>(1, 2, 1), 2, StatefulComp(true)));
static_assert(testIsSortedUntil(mkArr<OnlyHasLess>(3, 2, 3), 2, StatefulComp(true)));
static_assert(testIsSortedUntil(mkArr<OnlyHasLess>(1, 0, 3), 2, StatefulComp(true)));

}  // namespace
}  // namespace mongo
