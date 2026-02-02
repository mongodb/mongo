#pragma once

#include <immer/extra/persist/detail/rbts/pool.hpp>

#include <immer/detail/rbts/rrbtree.hpp>

namespace immer::persist::rbts::detail {

struct regular_pos_tag
{};
struct leaf_pos_tag
{};
struct relaxed_pos_tag
{};

template <class T>
struct position_tag : std::false_type
{};

template <class... Rest>
struct position_tag<immer::detail::rbts::regular_sub_pos<Rest...>>
{
    using type = regular_pos_tag;
};

template <class... Rest>
struct position_tag<immer::detail::rbts::full_pos<Rest...>>
{
    using type = regular_pos_tag;
};
template <class... Rest>
struct position_tag<immer::detail::rbts::regular_pos<Rest...>>
{
    using type = regular_pos_tag;
};
template <class... Rest>
struct position_tag<immer::detail::rbts::empty_regular_pos<Rest...>>
{
    using type = regular_pos_tag;
};

template <class... Rest>
struct position_tag<immer::detail::rbts::leaf_sub_pos<Rest...>>
{
    using type = leaf_pos_tag;
};
template <class... Rest>
struct position_tag<immer::detail::rbts::full_leaf_pos<Rest...>>
{
    using type = leaf_pos_tag;
};
template <class... Rest>
struct position_tag<immer::detail::rbts::leaf_pos<Rest...>>
{
    using type = leaf_pos_tag;
};
template <class... Rest>
struct position_tag<immer::detail::rbts::empty_leaf_pos<Rest...>>
{
    using type = leaf_pos_tag;
};

template <class... Rest>
struct position_tag<immer::detail::rbts::relaxed_pos<Rest...>>
{
    using type = relaxed_pos_tag;
};

struct visitor_helper
{
    template <class T, class F>
    static void visit_regular(T&& pos, F&& fn)
    {
        visit_by_tag(std::forward<T>(pos), std::forward<F>(fn));
    }

    template <class T, class F>
    static void visit_relaxed(T&& pos, F&& fn)
    {
        visit_by_tag(std::forward<T>(pos), std::forward<F>(fn));
    }

    template <class T, class F>
    static void visit_leaf(T&& pos, F&& fn)
    {
        visit_by_tag(std::forward<T>(pos), std::forward<F>(fn));
    }

    template <class T, class F>
    static void visit_by_tag(T&& pos, F&& fn)
    {
        using Tag = typename position_tag<std::decay_t<T>>::type;
        fn(Tag{}, std::forward<T>(pos), [&fn](auto&& other_pos) {
            visit_by_tag(other_pos, fn);
        });
    }
};

} // namespace immer::persist::rbts::detail
