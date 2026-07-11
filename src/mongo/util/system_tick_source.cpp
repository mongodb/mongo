// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/system_tick_source.h"

#include "mongo/util/tick_source.h"

#include <chrono>  // NOLINT
#include <memory>

namespace mongo {

std::unique_ptr<TickSource> makeSystemTickSource() {
    class Steady : public TickSource {
        using C = std::chrono::steady_clock;  // NOLINT
        Tick getTicksPerSecond() override {
            static_assert(C::period::num == 1, "Fractional frequency disallowed");
            return C::period::den;
        }
        Tick getTicks() override {
            return C::now().time_since_epoch().count();
        }
    };
    return std::make_unique<Steady>();
}

TickSource* globalSystemTickSource() {
    static const auto p = makeSystemTickSource().release();
    return p;
}

}  // namespace mongo
