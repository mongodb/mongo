#pragma once

#include <immer/extra/io.hpp>
#include <immer/extra/persist/cereal/archives.hpp>

namespace immer::persist::detail {

template <class Pools, class Archive, class PoolNameFn, class WrapF>
auto load_pools(std::istream& is, const WrapF& wrap)
{
    const auto reload_pool =
        [wrap](std::istream& is, Pools pools, bool ignore_pool_exceptions) {
            auto restore              = immer::util::istream_snapshot{is};
            const auto original_pools = pools;
            auto ar = input_pools_cereal_archive_wrapper<Archive,
                                                         Pools,
                                                         decltype(wrap),
                                                         PoolNameFn>{
                std::move(pools), wrap, is};
            ar.ignore_pool_exceptions = ignore_pool_exceptions;
            /**
             * NOTE: Critical to clear the pools before loading into it
             * again. I hit a bug when pools contained a vector and every
             * load would append to it, instead of replacing the contents.
             */
            pools = {};
            ar(CEREAL_NVP(pools));
            pools.merge_previous(original_pools);
            return pools;
        };

    auto pools = Pools{};
    if constexpr (detail::is_pool_empty<Pools>()) {
        return pools;
    }

    auto prev = pools;
    while (true) {
        // Keep reloading until everything is loaded.
        // Reloading of the pool might trigger validation of some containers
        // (hash-based, for example) because the elements actually come from
        // other pools that are not yet loaded.
        constexpr bool ignore_pool_exceptions = true;
        pools = reload_pool(is, std::move(pools), ignore_pool_exceptions);
        if (prev == pools) {
            // Looks like we're done, reload one more time but do not ignore the
            // exceptions, for the final validation.
            pools = reload_pool(is, std::move(pools), false);
            break;
        }
        prev = pools;
    }

    return pools;
}

} // namespace immer::persist::detail
