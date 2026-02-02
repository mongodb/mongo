#pragma once

#include <immer/extra/persist/cereal/policy.hpp>
#include <immer/extra/persist/detail/transform.hpp>

namespace immer::persist {

/**
 * @defgroup persist-transform
 */

/**
 * @brief Return just the pools of all the containers of the provided value
 * serialized using the provided policy.
 *
 * @ingroup persist-transform
 * @see convert_container
 */
template <typename T, class Policy = hana_struct_auto_policy>
auto get_output_pools(const T& value0, const Policy& policy = Policy{})
{
    const auto types = boost::hana::to_set(policy.get_pool_types(value0));
    auto pools       = detail::generate_output_pools(types);
    const auto wrap  = detail::wrap_known_types(types, detail::wrap_for_saving);
    using Pools      = std::decay_t<decltype(pools)>;

    {
        auto ar = output_pools_cereal_archive_wrapper<
            detail::blackhole_output_archive,
            Pools,
            decltype(wrap),
            detail::empty_name_fn>{pools, wrap};
        ar(CEREAL_NVP(value0));
        ar.finalize();
        pools = std::move(ar).get_output_pools();
    }
    return pools;
}

/**
 * Given output_pools and a map of transformations, produce a new type of
 * input pools with those transformations applied.
 *
 * `conversion_map` is a `boost::hana::map` where keys are types of `immer`
 * containers and values are the transforming functions.
 *
 * @ingroup persist-transform
 * @see get_output_pools
 * @rst
 * :ref:`transformations-with-pools`
 * :ref:`transforming-nested-containers`
 * @endrst
 */
template <class Storage, class ConversionMap>
inline auto
transform_output_pool(const detail::output_pools<Storage>& old_pools,
                      const ConversionMap& conversion_map)
{
    const auto old_load_pools = to_input_pools(old_pools);
    // NOTE: We have to copy old_pools here because the get_id function will
    // be called later, as the conversion process is lazy.
    const auto get_id = [old_pools](const auto& immer_container) {
        return detail::get_container_id(old_pools, immer_container);
    };
    return old_load_pools.transform_recursive(conversion_map, get_id);
}

/**
 * Given output_pools and new (transformed) input_pools, effectively
 * convert the given container.
 *
 * @ingroup persist-transform
 * @see get_output_pools
 * @rst
 * :ref:`transformations-with-pools`
 * :ref:`transforming-nested-containers`
 * @endrst
 */
template <class SaveStorage, class LoadStorage, class Container>
auto convert_container(const detail::output_pools<SaveStorage>& output_pools,
                       detail::input_pools<LoadStorage>& new_input_pools,
                       const Container& container)
{
    const auto container_id = detail::get_container_id(output_pools, container);
    auto& loader =
        new_input_pools
            .template get_loader_by_old_container<std::decay_t<Container>>();
    auto result = loader.load(container_id);
    return result;
}

} // namespace immer::persist
