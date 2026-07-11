// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

#if defined(MONGO_CONFIG_DEBUG_BUILD)
inline constexpr bool kDebugBuild = true;
#else
inline constexpr bool kDebugBuild = false;
#endif

template <unsigned period>
class SampleEveryNth {
public:
    // Increment, returning true on first call and each subsequent 'period'.
    bool tick() {
        return _count.fetchAndAddRelaxed(1) % period == 0;
    }

private:
    Atomic<long long> _count{0};
};

struct Occasionally : SampleEveryNth<16> {};
struct Rarely : SampleEveryNth<128> {};

}  // namespace mongo
