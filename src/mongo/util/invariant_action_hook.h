// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/source_location.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Registers the lookup function used by invariant failure paths. Passing nullptr
 * restores the default behavior (abort).
 *
 * When fn returns true, the invariant will abort as usual. Otherwise the invariant will instead
 * throw using tassert.
 */
[[MONGO_MOD_PRIVATE]] void setLookupInvariantAbortsFn(std::function<bool(std::string_view)> fn);

/**
 * Returns the action to take for an invariant failure at the given source location.
 */
#ifdef MONGO_SOURCE_LOCATION_HAVE_STD
[[MONGO_MOD_PRIVATE]] bool shouldInvariantAbort(WrappedStdSourceLocation loc);
#endif
[[MONGO_MOD_PRIVATE]] bool shouldInvariantAbort(SyntheticSourceLocation loc);

}  // namespace mongo
