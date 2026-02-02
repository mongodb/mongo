#pragma once

#include <immer/extra/persist/cereal/policy.hpp>
#include <immer/extra/persist/detail/cereal/input_archive_util.hpp>
#include <immer/extra/persist/detail/cereal/pools.hpp>
#include <immer/extra/persist/detail/cereal/wrap.hpp>

namespace immer::persist {

/**
 * @brief Load a value of the given type `T` from the provided stream using
 * pools. By default, `cereal::JSONInputArchive` is used but a different
 * `cereal` input archive can be provided.
 *
 * @ingroup persist-api
 */
template <class T,
          class Archive = cereal::JSONInputArchive,
          class Policy  = default_policy,
          class... Args>
T cereal_load_with_pools(std::istream& is,
                         const Policy& policy = Policy{},
                         Args&&... args)
{
    using TypesSet =
        decltype(boost::hana::to_set(policy.get_pool_types(std::declval<T>())));
    using Pools = decltype(detail::generate_input_pools(TypesSet{}));

    using PoolNameFn = get_pool_name_fn_t<Policy>;

    const auto wrap =
        detail::wrap_known_types(TypesSet{}, detail::wrap_for_loading);
    auto pools = detail::load_pools<Pools, Archive, PoolNameFn>(is, wrap);

    auto ar = immer::persist::input_pools_cereal_archive_wrapper<Archive,
                                                                 Pools,
                                                                 decltype(wrap),
                                                                 PoolNameFn>{
        std::move(pools), wrap, is, std::forward<Args>(args)...};
    auto value0 = T{};
    policy.load(ar, value0);
    return value0;
}

/**
 * @brief Load a value of the given type `T` from the provided string using
 * pools. By default, `cereal::JSONInputArchive` is used but a different
 * `cereal` input archive can be provided.
 *
 * @ingroup persist-api
 */
template <class T,
          class Archive = cereal::JSONInputArchive,
          class Policy  = default_policy>
T cereal_load_with_pools(const std::string& input,
                         const Policy& policy = Policy{})
{
    auto is = std::istringstream{input};
    return cereal_load_with_pools<T, Archive>(is, policy);
}

} // namespace immer::persist
