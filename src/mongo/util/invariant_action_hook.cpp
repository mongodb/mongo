// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/invariant_action_hook.h"

#include "mongo/platform/rwmutex.h"

#include <shared_mutex>

namespace mongo {
namespace {

std::function<bool(std::string_view)> gLookupInvariantAbortsFn{};
RWMutex lookupInvariantAbortsFnMutex;

auto getLookupInvariantAbortsFn() {
    auto _ = std::shared_lock{lookupInvariantAbortsFnMutex};
    return gLookupInvariantAbortsFn;
}

}  // namespace

#ifdef MONGO_SOURCE_LOCATION_HAVE_STD
bool shouldInvariantAbort(WrappedStdSourceLocation loc) {
    if (auto fn = getLookupInvariantAbortsFn()) {
        return fn(fmt::format("{}:{}", loc.file_name(), loc.line()));
    }
    return true;
}
#endif

bool shouldInvariantAbort(SyntheticSourceLocation loc) {
    if (auto fn = getLookupInvariantAbortsFn()) {
        return fn(fmt::format("{}:{}", loc.file_name(), loc.line()));
    }
    return true;
}

void setLookupInvariantAbortsFn(std::function<bool(std::string_view)> fn) {
    auto _ = std::unique_lock{lookupInvariantAbortsFnMutex};
    gLookupInvariantAbortsFn = std::move(fn);
}

}  // namespace mongo
