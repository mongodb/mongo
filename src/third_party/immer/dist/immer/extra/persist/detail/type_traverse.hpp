#pragma once

#include <immer/box.hpp>
#include <immer/flex_vector.hpp>
#include <immer/map.hpp>
#include <immer/set.hpp>
#include <immer/table.hpp>
#include <immer/vector.hpp>

#include <boost/hana.hpp>

#include <variant>

namespace immer::persist::util {

namespace detail {

namespace hana = boost::hana;

template <class T>
struct single_type_t
{
    static auto apply()
    {
        return hana::make_tuple(
            hana::make_pair(hana::type_c<T>, BOOST_HANA_STRING("")));
    }
};

template <class T, class = void>
struct get_inner_types_t : single_type_t<T>
{};

template <class T>
struct get_inner_types_t<T, std::enable_if_t<hana::Struct<T>::value>>
{
    static auto apply()
    {
        auto value = T{};
        return hana::transform(hana::keys(value), [&](auto key) {
            const auto& member = hana::at_key(value, key);
            return hana::make_pair(hana::typeid_(member), key);
        });
    }
};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct get_inner_types_t<immer::vector<T, MemoryPolicy, B, BL>>
    : single_type_t<T>
{};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct get_inner_types_t<immer::flex_vector<T, MemoryPolicy, B, BL>>
    : single_type_t<T>
{};

template <typename T, typename MemoryPolicy>
struct get_inner_types_t<immer::box<T, MemoryPolicy>> : single_type_t<T>
{};

template <typename T,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct get_inner_types_t<immer::set<T, Hash, Equal, MemoryPolicy, B>>
    : single_type_t<T>
{};

template <typename K,
          typename T,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct get_inner_types_t<immer::map<K, T, Hash, Equal, MemoryPolicy, B>>
{
    static auto apply()
    {
        return hana::concat(single_type_t<K>::apply(),
                            single_type_t<T>::apply());
    }
};

template <typename T,
          typename KeyFn,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct get_inner_types_t<immer::table<T, KeyFn, Hash, Equal, MemoryPolicy, B>>
    : single_type_t<T>
{};

template <class... Types>
struct get_inner_types_t<std::variant<Types...>>
{
    static auto apply()
    {
        return hana::make_tuple(
            hana::make_pair(hana::type_c<Types>, BOOST_HANA_STRING(""))...);
    }
};

constexpr auto insert_conditionally = [](auto map, auto pair) {
    namespace hana = hana;
    using contains_t =
        decltype(hana::is_just(hana::find(map, hana::first(pair))));
    if constexpr (contains_t::value) {
        const auto empty_string = BOOST_HANA_STRING("");
        using is_empty_t        = decltype(hana::second(pair) == empty_string);
        if constexpr (is_empty_t::value) {
            // Do not replace with empty string
            return map;
        } else {
            return hana::insert(map, pair);
        }
    } else {
        return hana::insert(map, pair);
    }
};

template <class T>
auto get_inner_types_map_with_empty_strings(const T& type)
{
    namespace hana = boost::hana;

    constexpr auto get_for_one_type = [](auto type) {
        using type_t = typename decltype(type)::type;
        return detail::get_inner_types_t<type_t>::apply();
    };

    constexpr auto get_for_many = [get_for_one_type](const auto& map) {
        auto new_pairs = hana::to_tuple(map) | [get_for_one_type](auto pair) {
            return get_for_one_type(hana::first(pair));
        };

        return hana::fold_left(
            hana::to_map(new_pairs), map, detail::insert_conditionally);
    };

    constexpr auto can_expand = [get_for_many](const auto& set) {
        return set != get_for_many(set);
    };

    auto expanded = hana::while_(
        can_expand, hana::to_map(get_for_one_type(type)), get_for_many);
    return expanded;
}

} // namespace detail

/**
 * Generate a map (type, member_name) for all members of a given type,
 * recursively.
 */
template <class T>
auto get_inner_types_map(const T& type)
{
    namespace hana = boost::hana;

    auto with_empty_strings =
        detail::get_inner_types_map_with_empty_strings(type);

    // Throw away types we don't know names for
    const auto empty_string = BOOST_HANA_STRING("");
    auto result             = hana::filter(hana::to_tuple(with_empty_strings),
                               [empty_string](auto pair) {
                                   return hana::second(pair) != empty_string;
                               });
    return hana::to_map(result);
}

template <class T>
auto get_inner_types(const T& type)
{
    return boost::hana::keys(
        detail::get_inner_types_map_with_empty_strings(type));
}

} // namespace immer::persist::util
