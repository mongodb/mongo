#pragma once

#include <immer/extra/persist/cereal/archives.hpp>
#include <immer/extra/persist/detail/cereal/pools.hpp>
#include <immer/extra/persist/detail/traits.hpp>
#include <immer/extra/persist/errors.hpp>

#include <fmt/format.h>

#include <boost/core/demangle.hpp>

namespace immer::persist::detail {

/**
 * @brief A wrapper that allows the library to serialize the wrapped container
 * using a corresponding pool.
 *
 * When saving, it saves the container into the pool by performing the following
 * steps:
 *   - request the output pool corresponding to the type of the ``Container``
 *   - save the container to the pool by calling ``add_to_pool``
 *   - acquire the ID from the pool for the just saved container
 *   - save the ID into the output archive.
 *
 * Similarly, the steps for loading are:
 *   - container ID is loaded from the input archive by ``cereal``
 *   - request the input pool corresponding to the type of the ``Container``
 *   - load the container with the required ID from the input pool.
 *
 * Consequently, instead of the container's actual data, ``persistable`` would
 * serialize only the ID of the wrapped container.
 *
 * @tparam Container ``immer`` container that should be serialized using a pool.
 *
 * @ingroup persist-impl
 */
template <class Container>
struct persistable
{
    Container container;

    persistable() = default;

    persistable(std::initializer_list<typename Container::value_type> values)
        : container{std::move(values)}
    {
    }

    persistable(Container container_)
        : container{std::move(container_)}
    {
    }

    friend bool operator==(const persistable& left, const persistable& right)
    {
        return left.container == right.container;
    }

    friend bool operator!=(const persistable& left, const persistable& right)
    {
        return left.container != right.container;
    }
};

template <class Previous,
          class Storage,
          class WrapFn,
          class PoolNameFn,
          class Container>
auto save_minimal(
    const output_pools_cereal_archive_wrapper<Previous,
                                              detail::output_pools<Storage>,
                                              WrapFn,
                                              PoolNameFn>& ar,
    const persistable<Container>& value)
{
    auto& pool =
        const_cast<
            output_pools_cereal_archive_wrapper<Previous,
                                                detail::output_pools<Storage>,
                                                WrapFn,
                                                PoolNameFn>&>(ar)
            .get_output_pools()
            .template get_output_pool<Container>();
    auto [pool2, id] = add_to_pool(value.container, std::move(pool));
    pool             = std::move(pool2);
    return id.value;
}

// This function must exist because cereal does some checks and it's not
// possible to have only load_minimal for a type without having save_minimal.
template <class Previous,
          class Storage,
          class WrapFn,
          class PoolNameFn,
          class Container>
auto save_minimal(
    const output_pools_cereal_archive_wrapper<Previous,
                                              detail::input_pools<Storage>,
                                              WrapFn,
                                              PoolNameFn>& ar,
    const persistable<Container>& value) ->
    typename container_traits<Container>::container_id::rep_t
{
    throw std::logic_error{"Should never be called"};
}

template <class Previous,
          class Pools,
          class WrapFn,
          class PoolNameFn,
          class Container>
void load_minimal(
    const input_pools_cereal_archive_wrapper<Previous,
                                             Pools,
                                             WrapFn,
                                             PoolNameFn>& ar,
    persistable<Container>& value,
    const typename container_traits<Container>::container_id::rep_t& id)
{
    auto& loader =
        const_cast<input_pools_cereal_archive_wrapper<Previous,
                                                      Pools,
                                                      WrapFn,
                                                      PoolNameFn>&>(ar)
            .template get_loader<Container>();

    // Have to be specific because for vectors container_id is different from
    // node_id, but for hash-based containers, a container is identified just by
    // its root node.
    using container_id_ = typename container_traits<Container>::container_id;

    try {
        value.container = loader.load(container_id_{id});
    } catch (const pool_exception& ex) {
        if (!ar.ignore_pool_exceptions) {
            throw ::cereal::Exception{fmt::format(
                "Failed to load a container ID {} from the pool of {}: {}",
                id,
                boost::core::demangle(typeid(Container).name()),
                ex.what())};
        }
    }
}

// This function must exist because cereal does some checks and it's not
// possible to have only load_minimal for a type without having save_minimal.
template <class Archive, class Container>
auto save_minimal(const Archive& ar, const persistable<Container>& value) ->
    typename container_traits<Container>::container_id::rep_t
{
    throw std::logic_error{
        "Should never be called. save_minimal(const Archive& ar..."};
}

template <class Archive, class Container>
void load_minimal(
    const Archive& ar,
    persistable<Container>& value,
    const typename container_traits<Container>::container_id::rep_t& id)
{
    // This one is actually called while loading with not-yet-fully-loaded
    // pool.
}

} // namespace immer::persist::detail
