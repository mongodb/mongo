#pragma once

#include <immer/extra/persist/detail/cereal/wrap.hpp>
#include <immer/extra/persist/detail/names.hpp>

namespace immer::persist {

/**
 * @defgroup persist-policy
 */

template <class T>
auto get_pools_names(const T&)
{
    return boost::hana::make_map();
}

template <class T>
auto get_pools_types(const T&)
{
    return boost::hana::make_set();
}

/**
 * @brief This struct provides functions that `immer::persist` uses to serialize
 * the user-provided value using `cereal`. In this case, we use `cereal`'s
 * default name, `value0`. It's used in all policies provided by
 * `immer::persist`.
 *
 * Other possible way would be to use a third-party library to serialize the
 * value inline (without the `value0` node) by taking a dependency on
 * <a href="https://github.com/LowCostCustoms/cereal-inline">cereal-inline</a>,
 * for example.
 *
 * @ingroup persist-policy
 */
struct value0_serialize_t
{
    template <class Archive, class T>
    void save(Archive& ar, const T& value0) const
    {
        ar(CEREAL_NVP(value0));
    }

    template <class Archive, class T>
    void load(Archive& ar, T& value0) const
    {
        ar(CEREAL_NVP(value0));
    }
};

/**
 * @brief Policy is a type that describes certain aspects of serialization for
 * `immer::persist`.
 *      - How to call into the `cereal` archive to save and load the
 * user-provided value. Can be used to serealize the value inline (without the
 * `value0` node) by taking a dependency on <a
 * href="https://github.com/LowCostCustoms/cereal-inline">cereal-inline</a>, for
 * example.
 *      - Types of `immer` containers that will be serialized using pools. One
 * pool contains nodes of only one `immer` container type.
 *      - Names for each per-type pool.
 *
 * @ingroup persist-policy
 */
template <class T>
struct via_get_pools_names_policy_t : value0_serialize_t
{
    auto get_pool_types(const T& value) const
    {
        return boost::hana::keys(get_pools_names(value));
    }

    using Map = decltype(get_pools_names(std::declval<T>()));

    template <class Container>
    auto get_pool_name(const Container& container) const
    {
        return detail::name_from_map_fn<Map>{}(container);
    }
};

/**
 * @brief Create an `immer::persist` policy that uses the user-provided
 * `get_pools_names` function (located in the same namespace as the value user
 * serializes) to determine:
 *   - the types of `immer` containers that should be serialized in a pool
 *   - the names of those pools in JSON (and possibly other formats).
 *
 * The `get_pools_names` function is expected to return a `boost::hana::map`
 * where key is a container type and value is the name for this container's pool
 * as a `BOOST_HANA_STRING`.
 *
 * @param value Value that is going to be serialized, only type of the value
 * matters.
 *
 * @ingroup persist-policy
 */
template <class T>
auto via_get_pools_names_policy(const T& value)
{
    return via_get_pools_names_policy_t<std::decay_t<T>>{};
}

/**
 * @brief This struct is used in some policies to provide names to each pool
 * by using a demangled name of the `immer` container corresponding to the pool.
 *
 * @ingroup persist-policy
 */
struct demangled_names_t
{
    template <class T>
    auto get_pool_name(const T& value) const
    {
        return detail::get_demangled_name(value);
    }
};

/**
 * @brief An `immer::persist` policy that uses the user-provided
 * `get_pools_types` function to determine the types of `immer` containers that
 * should be serialized in a pool.
 *
 * The `get_pools_types` function is expected to return a `boost::hana::set` of
 * types of the desired containers.
 *
 * The names for the pools are determined via `demangled_names_t`.
 *
 * @ingroup persist-policy
 */
struct via_get_pools_types_policy
    : demangled_names_t
    , value0_serialize_t
{
    template <class T>
    auto get_pool_types(const T& value) const
    {
        return get_pools_types(value);
    }
};

/**
 * @brief An `immer::persist` policy that recursively finds all `immer`
 * containers in a serialized value. The value must be a `boost::hana::Struct`.
 *
 * The names for the pools are determined via `demangled_names_t`.
 *
 * @ingroup persist-policy
 */
struct hana_struct_auto_policy : demangled_names_t
{
    template <class T>
    auto get_pool_types(const T& value) const
    {
        return detail::get_pools_for_hana_type<T>();
    }
};

template <class T>
struct hana_struct_auto_member_name_policy_t : value0_serialize_t
{
    auto get_pool_types(const T& value) const
    {
        return detail::get_pools_for_hana_type<T>();
    }

    using map_t = decltype(detail::get_named_pools_for_hana_type<T>());

    template <class Container>
    auto get_pool_name(const Container& container) const
    {
        return detail::name_from_map_fn<map_t>{}(container);
    }
};

/**
 * @brief Create an `immer::persist` policy that recursively finds all `immer`
 * containers in a serialized value and uses member names to name the pools.
 * The value must be a `boost::hana::Struct`.
 *
 * @param value Value that is going to be serialized, only type of the value
 * matters.
 *
 * @ingroup persist-policy
 */
template <class T>
auto hana_struct_auto_member_name_policy(const T& value)
{
    return hana_struct_auto_member_name_policy_t<std::decay_t<T>>{};
}

using default_policy = via_get_pools_types_policy;

template <class Policy>
struct get_pool_name_fn_t
{
    template <class T>
    auto operator()(const T& value) const
    {
        return Policy{}.get_pool_name(value);
    }
};

} // namespace immer::persist
