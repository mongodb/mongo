#pragma once

#include <immer/extra/persist/detail/cereal/persistable.hpp>
#include <immer/extra/persist/detail/type_traverse.hpp>

// Bring in all known pools to be able to wrap all immer types
#include <immer/extra/persist/detail/array/pool.hpp>
#include <immer/extra/persist/detail/box/pool.hpp>
#include <immer/extra/persist/detail/champ/traits.hpp>
#include <immer/extra/persist/detail/rbts/traits.hpp>

namespace immer::persist::detail {

/**
 * This wrapper is used to load a given container via persistable.
 */
template <class Container>
struct persistable_loader_wrapper
{
    Container& value;

    template <class Archive>
    typename container_traits<Container>::container_id::rep_t
    save_minimal(const Archive&) const
    {
        throw std::logic_error{
            "Should never be called. persistable_loader_wrapper::save_minimal"};
    }

    template <class Archive>
    void load_minimal(
        const Archive& ar,
        const typename container_traits<Container>::container_id::rep_t&
            container_id)
    {
        persistable<Container> arch;
        immer::persist::detail::load_minimal(ar, arch, container_id);
        value = std::move(arch).container;
    }
};

constexpr auto is_persistable = boost::hana::is_valid(
    [](auto&& obj) ->
    typename container_traits<std::decay_t<decltype(obj)>>::output_pool_t {});

/**
 * Make a function that operates conditionally on its single argument, based on
 * the given predicate. If the predicate is not satisfied, the function forwards
 * its argument unchanged.
 */
constexpr auto make_conditional_func = [](auto pred, auto func) {
    return [pred, func](auto&& value) -> decltype(auto) {
        return boost::hana::if_(pred(value), func, boost::hana::id)(
            std::forward<decltype(value)>(value));
    };
};

constexpr auto to_persistable = [](const auto& x) {
    return persistable<std::decay_t<decltype(x)>>(x);
};

constexpr auto to_persistable_loader = [](auto& value) {
    using V = std::decay_t<decltype(value)>;
    return persistable_loader_wrapper<V>{value};
};

/**
 * This function will wrap a value in persistable if possible or will return a
 * reference to its argument.
 */
constexpr auto wrap_for_saving =
    make_conditional_func(is_persistable, to_persistable);

constexpr auto wrap_for_loading =
    make_conditional_func(is_persistable, to_persistable_loader);

/**
 * Returns a wrapping function that wraps only known types.
 */
template <class KnownSet, class WrapF>
auto wrap_known_types(KnownSet types, WrapF wrap)
{
    static_assert(boost::hana::is_a<boost::hana::set_tag, KnownSet>);
    const auto is_known = [](const auto& value) {
        using result_t = decltype(boost::hana::contains(
            KnownSet{}, boost::hana::typeid_(value)));
        return result_t{};
    };
    return make_conditional_func(is_known, std::move(wrap));
}

static_assert(std::is_same_v<
                  decltype(wrap_for_saving(std::declval<const std::string&>())),
                  const std::string&>,
              "wrap must return a reference when it's not wrapping the type");
static_assert(std::is_same_v<decltype(wrap_for_saving(immer::vector<int>{})),
                             persistable<immer::vector<int>>>,
              "and a value when it's wrapping");

/**
 * Generate a hana set of types of persistable members for the given type,
 * recursively. Example: [type_c<immer::map<K, V>>]
 */
template <class T>
auto get_pools_for_hana_type()
{
    namespace hana     = boost::hana;
    auto all_types_set = util::get_inner_types(hana::type_c<T>);
    auto persistable =
        hana::filter(hana::to_tuple(all_types_set), [](auto type) {
            using Type = typename decltype(type)::type;
            return detail::is_persistable(Type{});
        });
    return persistable;
}

/**
 * Generate a hana map of persistable members for the given type, recursively.
 * Example:
 * [(type_c<immer::map<K, V>>, "tracks")]
 */
template <class T>
auto get_named_pools_for_hana_type()
{
    namespace hana     = boost::hana;
    auto all_types_map = util::get_inner_types_map(hana::type_c<T>);
    auto persistable =
        hana::filter(hana::to_tuple(all_types_map), [](auto pair) {
            using Type = typename decltype(+hana::first(pair))::type;
            return detail::is_persistable(Type{});
        });
    return hana::to_map(persistable);
}

} // namespace immer::persist::detail
