// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/stack_locator.h"

#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

boost::optional<std::size_t> StackLocator::available() const {
    if (!begin() || !end())
        return boost::none;

    // Technically, it is undefined behavior to compare or subtract
    // two pointers that do not point into the same
    // aggregate. However, we know that these are both pointers within
    // the same stack, and it seems unlikely that the compiler will
    // see that it can elide the comparison here.

    const auto cbegin = reinterpret_cast<const char*>(begin());
    const auto cend = reinterpret_cast<const char*>(end());
    const auto csp = reinterpret_cast<const char*>(_capturedStackPointer);

    // TODO: Assumes that stack grows downward
    invariant(csp <= cbegin);
    invariant(csp > cend);

    std::size_t avail = csp - cend;

    return avail;
}

boost::optional<size_t> StackLocator::size() const {
    if (!begin() || !end())
        return boost::none;

    const auto cbegin = reinterpret_cast<const char*>(begin());
    const auto cend = reinterpret_cast<const char*>(end());

    // TODO: Assumes that stack grows downward
    invariant(cbegin > cend);

    return static_cast<size_t>(cbegin - cend);
}

}  // namespace mongo
