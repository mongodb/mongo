// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/sort.h"

#include <array>
#include <cstddef>
#include <functional>
#include <iterator>

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
