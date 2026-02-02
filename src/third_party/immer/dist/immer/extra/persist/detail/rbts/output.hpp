#pragma once

#include <immer/extra/persist/detail/rbts/traverse.hpp>

namespace immer::persist::rbts {

namespace detail {

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
std::pair<output_pool<T, MemoryPolicy, B, BL>, node_id>
get_node_id(output_pool<T, MemoryPolicy, B, BL> pool,
            const immer::detail::rbts::node<T, MemoryPolicy, B, BL>* ptr)
{
    auto* ptr_void = static_cast<const void*>(ptr);
    if (auto* maybe_id = pool.node_ptr_to_id.find(ptr_void)) {
        auto id = *maybe_id;
        return {std::move(pool), id};
    }

    const auto id       = node_id{pool.node_ptr_to_id.size()};
    pool.node_ptr_to_id = std::move(pool.node_ptr_to_id).set(ptr_void, id);
    return {std::move(pool), id};
}

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct pool_builder
{
    output_pool<T, MemoryPolicy, B, BL> pool;

    template <class Pos, class VisitF>
    void operator()(regular_pos_tag, Pos& pos, VisitF&& visit)
    {
        auto id = get_node_id(pos.node());
        if (pool.inners.count(id)) {
            return;
        }

        auto node_info = inner_node{};
        // Explicit this-> call to workaround an "unused this" warning.
        pos.each(visitor_helper{},
                 [&node_info, &visit, this](
                     auto any_tag, auto& child_pos, auto&&) mutable {
                     node_info.children =
                         std::move(node_info.children)
                             .push_back(this->get_node_id(child_pos.node()));
                     visit(child_pos);
                 });
        pool.inners = std::move(pool.inners).set(id, node_info);
    }

    template <class Pos, class VisitF>
    void operator()(relaxed_pos_tag, Pos& pos, VisitF&& visit)
    {
        auto id = get_node_id(pos.node());
        if (pool.inners.count(id)) {
            return;
        }

        auto node_info = inner_node{
            .relaxed = true,
        };

        pos.each(visitor_helper{}, [&](auto any_tag, auto& child_pos, auto&&) {
            node_info.children = std::move(node_info.children)
                                     .push_back(get_node_id(child_pos.node()));

            visit(child_pos);
        });

        assert(node_info.children.size() == pos.node()->relaxed()->d.count);

        pool.inners = std::move(pool.inners).set(id, node_info);
    }

    template <class Pos, class VisitF>
    void operator()(leaf_pos_tag, Pos& pos, VisitF&& visit)
    {
        T* first = pos.node()->leaf();
        auto id  = get_node_id(pos.node());
        if (pool.leaves.count(id)) {
            return;
        }

        auto info = persist::detail::values_save<T>{
            .begin = first,
            .end   = first + pos.count(),
        };
        pool.leaves = std::move(pool.leaves).set(id, std::move(info));
    }

    node_id get_node_id(immer::detail::rbts::node<T, MemoryPolicy, B, BL>* ptr)
    {
        auto [pool2, id] =
            immer::persist::rbts::detail::get_node_id(std::move(pool), ptr);
        pool = std::move(pool2);
        return id;
    }
};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL,
          class Pool>
auto save_nodes(const immer::detail::rbts::rbtree<T, MemoryPolicy, B, BL>& tree,
                Pool pool)
{
    auto save = pool_builder<T, MemoryPolicy, B, BL>{
        .pool = std::move(pool),
    };
    tree.traverse(visitor_helper{}, save);
    return std::move(save.pool);
}

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL,
          class Pool>
auto save_nodes(
    const immer::detail::rbts::rrbtree<T, MemoryPolicy, B, BL>& tree, Pool pool)
{
    auto save = pool_builder<T, MemoryPolicy, B, BL>{
        .pool = std::move(pool),
    };
    tree.traverse(visitor_helper{}, save);
    return std::move(save.pool);
}

} // namespace detail

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
std::pair<output_pool<T, MemoryPolicy, B, BL>, container_id>
add_to_pool(immer::vector<T, MemoryPolicy, B, BL> vec,
            output_pool<T, MemoryPolicy, B, BL> pool)
{
    const auto& impl        = vec.impl();
    auto root_id            = node_id{};
    auto tail_id            = node_id{};
    std::tie(pool, root_id) = detail::get_node_id(std::move(pool), impl.root);
    std::tie(pool, tail_id) = detail::get_node_id(std::move(pool), impl.tail);
    const auto tree_id      = rbts_info{
             .root = root_id,
             .tail = tail_id,
    };

    if (auto* p = pool.rbts_to_id.find(tree_id)) {
        // Already been saved
        auto vector_id = *p;
        return {std::move(pool), vector_id};
    }

    pool = detail::save_nodes(impl, std::move(pool));

    assert(pool.inners.count(root_id));
    assert(pool.leaves.count(tail_id));

    const auto vector_id = container_id{pool.vectors.size()};

    pool.rbts_to_id = std::move(pool.rbts_to_id).set(tree_id, vector_id);
    pool.vectors    = std::move(pool.vectors).push_back(tree_id);
    pool.saved_vectors =
        std::move(pool.saved_vectors).push_back(std::move(vec));

    return {std::move(pool), vector_id};
}

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
std::pair<output_pool<T, MemoryPolicy, B, BL>, container_id>
add_to_pool(immer::flex_vector<T, MemoryPolicy, B, BL> vec,
            output_pool<T, MemoryPolicy, B, BL> pool)
{
    const auto& impl        = vec.impl();
    auto root_id            = node_id{};
    auto tail_id            = node_id{};
    std::tie(pool, root_id) = detail::get_node_id(std::move(pool), impl.root);
    std::tie(pool, tail_id) = detail::get_node_id(std::move(pool), impl.tail);
    const auto tree_id      = rbts_info{
             .root = root_id,
             .tail = tail_id,
    };

    if (auto* p = pool.rbts_to_id.find(tree_id)) {
        // Already been saved
        auto vector_id = *p;
        return {std::move(pool), vector_id};
    }

    pool = detail::save_nodes(impl, std::move(pool));

    assert(pool.inners.count(root_id));
    assert(pool.leaves.count(tail_id));

    const auto vector_id = container_id{pool.vectors.size()};

    pool.rbts_to_id = std::move(pool.rbts_to_id).set(tree_id, vector_id);
    pool.vectors    = std::move(pool.vectors).push_back(tree_id);
    pool.saved_flex_vectors =
        std::move(pool.saved_flex_vectors).push_back(std::move(vec));

    return {std::move(pool), vector_id};
}

} // namespace immer::persist::rbts
