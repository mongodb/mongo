#pragma once

#include <immer/extra/persist/detail/champ/pool.hpp>

namespace immer::persist::champ {

template <class T, immer::detail::hamts::bits_t B>
struct output_pool_builder
{
    nodes_save<T, B> pool;

    template <class Node, class Depth>
    void visit_inner(const Node* node, Depth depth)
    {
        auto id = get_node_id(node);
        if (pool.inners.count(id)) {
            return;
        }

        auto node_info = inner_node_save<T, B>{
            .nodemap = node->nodemap(),
            .datamap = node->datamap(),
        };

        if (node->datamap()) {
            node_info.values = {node->values(),
                                node->values() + node->data_count()};
        }
        if (node->nodemap()) {
            auto fst = node->children();
            auto lst = fst + node->children_count();
            for (; fst != lst; ++fst) {
                node_info.children =
                    std::move(node_info.children).push_back(get_node_id(*fst));
                visit(*fst, depth + 1);
            }
        }

        pool.inners = std::move(pool.inners).set(id, node_info);
    }

    template <class Node>
    void visit_collision(const Node* node)
    {
        auto id = get_node_id(node);
        if (pool.inners.count(id)) {
            return;
        }

        pool.inners = std::move(pool.inners)
                          .set(id,
                               inner_node_save<T, B>{
                                   .values     = {node->collisions(),
                                                  node->collisions() +
                                                      node->collision_count()},
                                   .collisions = true,
                               });
    }

    template <class Node>
    void visit(const Node* node, immer::detail::hamts::count_t depth)
    {
        using immer::detail::hamts::max_depth;
        using hash_t = typename Node::hash_t;

        if (depth < max_depth<hash_t, B>) {
            visit_inner(node, depth);
        } else {
            visit_collision(node);
        }
    }

    template <class Node>
    node_id get_node_id(Node* ptr)
    {
        auto [pool2, id] =
            immer::persist::champ::get_node_id(std::move(pool), ptr);
        pool = std::move(pool2);
        return id;
    }
};

template <typename T,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B,
          class Pool>
auto save_nodes(
    const immer::detail::hamts::champ<T, Hash, Equal, MemoryPolicy, B>& champ,
    Pool pool)
{
    using champ_t = std::decay_t<decltype(champ)>;
    using node_t  = typename champ_t::node_t;

    auto save = output_pool_builder<typename node_t::value_t, B>{
        .pool = std::move(pool),
    };
    save.visit(champ.root, 0);

    return std::move(save.pool);
}

} // namespace immer::persist::champ
