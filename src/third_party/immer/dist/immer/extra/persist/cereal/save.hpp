#pragma once

#include <immer/extra/persist/cereal/policy.hpp>
#include <immer/extra/persist/detail/cereal/pools.hpp>
#include <immer/extra/persist/detail/cereal/wrap.hpp>

namespace immer::persist {

/**
 * @defgroup persist-api
 */

/**
 * @brief Serialize the provided value with pools using the provided policy
 * outputting into the provided stream. By default, `cereal::JSONOutputArchive`
 * is used but a different `cereal` output archive can be provided.
 *
 * @see Policy
 * @ingroup persist-api
 */
template <class Archive = cereal::JSONOutputArchive,
          class T,
          class Policy = default_policy,
          class... Args>
void cereal_save_with_pools(std::ostream& os,
                            const T& value0,
                            const Policy& policy = Policy{},
                            Args&&... args)
{
    const auto types = boost::hana::to_set(policy.get_pool_types(value0));
    auto pools       = detail::generate_output_pools(types);
    const auto wrap  = detail::wrap_known_types(types, detail::wrap_for_saving);
    using Pools      = std::decay_t<decltype(pools)>;
    auto ar          = immer::persist::output_pools_cereal_archive_wrapper<
        Archive,
        Pools,
        decltype(wrap),
        get_pool_name_fn_t<Policy>>{
        pools, wrap, os, std::forward<Args>(args)...};
    policy.save(ar, value0);
    // Calling finalize explicitly, as it might throw on saving the pools,
    // for example if pool names are not unique.
    ar.finalize();
}

/**
 * @brief Serialize the provided value with pools using the provided policy. By
 * default, `cereal::JSONOutputArchive` is used but a different `cereal` output
 * archive can be provided.
 *
 * @return std::string The resulting JSON.
 * @ingroup persist-api
 */
template <
    class Archive = cereal::JSONOutputArchive,
    class T,
    class Policy = default_policy,
    class = decltype(std::declval<Policy>().get_pool_types(std::declval<T>())),
    class... Args>
std::string cereal_save_with_pools(const T& value0,
                                   const Policy& policy = Policy{},
                                   Args&&... args)
{
    auto os = std::ostringstream{};
    cereal_save_with_pools<Archive, T, Policy>(
        os, value0, policy, std::forward<Args>(args)...);
    return os.str();
}

} // namespace immer::persist
