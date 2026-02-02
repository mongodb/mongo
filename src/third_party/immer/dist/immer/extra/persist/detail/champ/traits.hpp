#pragma once

#include <immer/extra/persist/detail/champ/champ.hpp>
#include <immer/extra/persist/detail/champ/pool.hpp>
#include <immer/extra/persist/detail/traits.hpp>
#include <immer/extra/persist/hash_container_conversion.hpp>

#include <boost/hana/functional/id.hpp>

namespace immer::persist::detail {

template <class Container>
struct champ_traits
{
    using output_pool_t =
        immer::persist::champ::container_output_pool<Container>;
    using input_pool_t = immer::persist::champ::container_input_pool<Container>;
    using container_id = immer::persist::node_id;

    template <typename Pool       = input_pool_t,
              typename TransformF = boost::hana::id_t>
    using loader_t =
        immer::persist::champ::container_loader<Container, Pool, TransformF>;

    template <class F>
    static auto transform(F&& func)
    {
        // We need this special target_container_type_request because we can't
        // determine the hash and equality operators for the new key any other
        // way.
        using NewContainer =
            std::decay_t<decltype(func(target_container_type_request{}))>;
        return NewContainer{};
    }
};

template <typename K,
          typename T,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct container_traits<immer::map<K, T, Hash, Equal, MemoryPolicy, B>>
    : champ_traits<immer::map<K, T, Hash, Equal, MemoryPolicy, B>>
{};

template <typename T,
          typename KeyFn,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct container_traits<immer::table<T, KeyFn, Hash, Equal, MemoryPolicy, B>>
    : champ_traits<immer::table<T, KeyFn, Hash, Equal, MemoryPolicy, B>>
{};

template <typename T,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct container_traits<immer::set<T, Hash, Equal, MemoryPolicy, B>>
    : champ_traits<immer::set<T, Hash, Equal, MemoryPolicy, B>>
{};

template <class Container>
struct container_traits<incompatible_hash_wrapper<Container>>
    : champ_traits<Container>
{
    using base_t = champ_traits<Container>;

    // Everything stays the same as for normal container, except that we tell
    // the loader to do something special.
    static constexpr bool enable_incompatible_hash_mode = true;

    template <typename Pool       = typename base_t::input_pool_t,
              typename TransformF = boost::hana::id_t>
    using loader_t =
        immer::persist::champ::container_loader<Container,
                                                Pool,
                                                TransformF,
                                                enable_incompatible_hash_mode>;
};

} // namespace immer::persist::detail
