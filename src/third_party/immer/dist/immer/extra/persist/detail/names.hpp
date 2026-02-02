#pragma once

#include <boost/core/demangle.hpp>
#include <boost/hana.hpp>

namespace immer::persist::detail {

template <class T>
auto get_demangled_name(const T&)
{
    return boost::core::demangle(typeid(std::decay_t<T>).name());
}

template <class T>
class error_duplicate_pool_name_found;

/**
 * @brief This function ensures that all the names are unique for the given map
 * of types to names. Otherwise, it triggers a compile-time error.
 *
 * @ingroup persist-impl
 */
template <class T>
auto are_type_names_unique(T type_names)
{
    namespace hana = boost::hana;
    auto names_set =
        hana::fold_left(type_names, hana::make_set(), [](auto set, auto pair) {
            return hana::if_(
                hana::contains(set, hana::second(pair)),
                [](auto pair) {
                    return error_duplicate_pool_name_found<
                        decltype(hana::second(pair))>{};
                },
                [&set](auto pair) {
                    return hana::insert(set, hana::second(pair));
                })(pair);
        });
    return hana::length(type_names) == hana::length(names_set);
}

template <class Map>
struct name_from_map_fn
{
    static_assert(decltype(are_type_names_unique(Map{}))::value,
                  "Pool names in the map must be unique");

    template <class T>
    auto operator()(const T& container) const
    {
        return Map{}[boost::hana::typeid_(container)].c_str();
    }
};

} // namespace immer::persist::detail
