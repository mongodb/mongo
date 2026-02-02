#pragma once

#include <immer/extra/persist/detail/rbts/input.hpp>
#include <immer/extra/persist/detail/rbts/output.hpp>
#include <immer/extra/persist/detail/traits.hpp>

namespace immer::persist::detail {

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct container_traits<immer::vector<T, MemoryPolicy, B, BL>>
{
    using output_pool_t = rbts::output_pool<T, MemoryPolicy, B, BL>;
    using input_pool_t  = rbts::input_pool<T>;
    using container_id  = immer::persist::container_id;

    template <typename Pool       = input_pool_t,
              typename TransformF = boost::hana::id_t>
    using loader_t =
        rbts::vector_loader<T, MemoryPolicy, B, BL, Pool, TransformF>;

    // This function is used to determine the type of the container after
    // applying some transformation.
    template <class F>
    static auto transform(F&& func)
    {
        using U = std::decay_t<decltype(func(std::declval<T>()))>;
        return immer::vector<U, MemoryPolicy, B, BL>{};
    }
};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct container_traits<immer::flex_vector<T, MemoryPolicy, B, BL>>
{
    using output_pool_t = rbts::output_pool<T, MemoryPolicy, B, BL>;
    using input_pool_t  = rbts::input_pool<T>;
    using container_id  = immer::persist::container_id;

    template <typename Pool       = input_pool_t,
              typename TransformF = boost::hana::id_t>
    using loader_t =
        rbts::flex_vector_loader<T, MemoryPolicy, B, BL, Pool, TransformF>;

    template <class F>
    static auto transform(F&& func)
    {
        using U = std::decay_t<decltype(func(std::declval<T>()))>;
        return immer::flex_vector<U, MemoryPolicy, B, BL>{};
    }
};

} // namespace immer::persist::detail
