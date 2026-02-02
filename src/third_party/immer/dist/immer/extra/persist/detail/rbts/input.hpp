#pragma once

#include <immer/extra/persist/detail/node_ptr.hpp>
#include <immer/extra/persist/detail/rbts/pool.hpp>
#include <immer/extra/persist/detail/rbts/traverse.hpp>
#include <immer/extra/persist/errors.hpp>

#include <boost/hana.hpp>
#include <boost/range/adaptor/indexed.hpp>
#include <immer/set.hpp>
#include <immer/vector.hpp>
#include <optional>

namespace immer::persist::rbts {

inline constexpr auto get_shift_for_depth(immer::detail::rbts::bits_t b,
                                          immer::detail::rbts::bits_t bl,
                                          immer::detail::rbts::count_t depth)
{
    auto bits      = immer::detail::rbts::shift_t{b};
    auto bits_leaf = immer::detail::rbts::shift_t{bl};
    return bits_leaf + bits * (immer::detail::rbts::shift_t{depth} - 1);
}

class vector_corrupted_exception : public pool_exception
{
public:
    vector_corrupted_exception(node_id id_,
                               std::size_t expected_count_,
                               std::size_t real_count_)
        : pool_exception{fmt::format("Loaded vector is corrupted. Inner "
                                     "node ID {} should "
                                     "have {} children but it has {}",
                                     id_,
                                     expected_count_,
                                     real_count_)}
        , id{id_}
        , expected_count{expected_count_}
        , real_count{real_count_}
    {
    }

    node_id id;
    std::size_t expected_count;
    std::size_t real_count;
};

class relaxed_node_not_allowed_exception : public pool_exception
{
public:
    relaxed_node_not_allowed_exception(node_id id_)
        : pool_exception{fmt::format("Node ID {} can't be a relaxed node", id_)}
        , id{id_}
    {
    }

    node_id id;
};

class same_depth_children_exception : public pool_exception
{
public:
    same_depth_children_exception(node_id id,
                                  std::size_t expected_depth,
                                  node_id child_id,
                                  std::size_t real_depth)
        : pool_exception{
              fmt::format("All children of node {} must have the same depth "
                          "{}, but the child {} has depth {}",
                          id,
                          expected_depth,
                          child_id,
                          real_depth)}
    {
    }
};

class incompatible_bits_parameters : public pool_exception
{
public:
    incompatible_bits_parameters(immer::detail::rbts::bits_t loader_bits,
                                 immer::detail::rbts::bits_t loader_bits_leaf,
                                 immer::detail::rbts::bits_t pool_bits,
                                 immer::detail::rbts::bits_t pool_bits_leaf)
        : pool_exception{
              fmt::format("B and BL parameters must be the same. Loader "
                          "expects {} and {} but the pool has {} and {}",
                          loader_bits,
                          loader_bits_leaf,
                          pool_bits,
                          pool_bits_leaf)}
    {
    }
};

template <class T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL,
          class Pool       = input_pool<T>,
          class TransformF = boost::hana::id_t>
class loader
{
public:
    using rbtree      = immer::detail::rbts::rbtree<T, MemoryPolicy, B, BL>;
    using rrbtree     = immer::detail::rbts::rrbtree<T, MemoryPolicy, B, BL>;
    using node_t      = typename rbtree::node_t;
    using node_ptr    = immer::persist::detail::node_ptr<node_t>;
    using nodes_set_t = immer::set<node_id>;

    explicit loader(Pool pool)
        : pool_{std::move(pool)}
    {
    }

    explicit loader(Pool pool, TransformF transform)
        : pool_{std::move(pool)}
        , transform_{std::move(transform)}
    {
    }

    immer::vector<T, MemoryPolicy, B, BL> load_vector(container_id id)
    {
        if (id.value >= pool_.vectors.size()) {
            throw invalid_container_id{id};
        }

        validate_bits_params();

        const auto& info = pool_.vectors[id.value];

        const auto relaxed_allowed = false;
        auto root                  = load_inner(info.root, {}, relaxed_allowed);
        auto tail                  = load_leaf(info.tail);

        const auto tree_size =
            get_node_size(info.root) + get_node_size(info.tail);
        const auto depth = get_node_depth(info.root);
        const auto shift = get_shift_for_depth(B, BL, depth);

        auto impl = rbtree{tree_size,
                           shift,
                           std::move(root).release(),
                           std::move(tail).release()};

        verify_tree(impl);
        return impl;
    }

    immer::flex_vector<T, MemoryPolicy, B, BL> load_flex_vector(container_id id)
    {
        if (id.value >= pool_.vectors.size()) {
            throw invalid_container_id{id};
        }

        validate_bits_params();

        const auto& info = pool_.vectors[id.value];

        const auto relaxed_allowed = true;
        auto root                  = load_inner(info.root, {}, relaxed_allowed);
        auto tail                  = load_leaf(info.tail);

        const auto tree_size =
            get_node_size(info.root) + get_node_size(info.tail);
        const auto depth = get_node_depth(info.root);
        const auto shift = get_shift_for_depth(B, BL, depth);

        auto impl = rrbtree{tree_size,
                            shift,
                            std::move(root).release(),
                            std::move(tail).release()};

        verify_tree(impl);

        return impl;
    }

private:
    node_ptr load_leaf(node_id id)
    {
        if (auto* p = leaves_.find(id)) {
            return *p;
        }

        auto* node_info = pool_.leaves.find(id);
        if (!node_info) {
            throw invalid_node_id{id};
        }

        const auto n         = node_info->data.size();
        constexpr auto max_n = immer::detail::rbts::branches<BL>;
        if (n > max_n) {
            throw invalid_children_count{id};
        }

        if constexpr (std::is_same_v<TransformF, boost::hana::id_t>) {
            auto leaf =
                node_ptr{n ? node_t::make_leaf_n(n) : rbtree::empty_tail(),
                         [n](auto* ptr) { node_t::delete_leaf(ptr, n); }};
            immer::detail::uninitialized_copy(node_info->data.begin(),
                                              node_info->data.end(),
                                              leaf.get()->leaf());
            leaves_        = std::move(leaves_).set(id, leaf);
            loaded_leaves_ = std::move(loaded_leaves_).set(leaf.get(), id);
            return leaf;
        } else {
            auto values = std::vector<T>{};
            for (const auto& item : node_info->data) {
                values.push_back(transform_(item));
            }
            auto leaf =
                node_ptr{n ? node_t::make_leaf_n(n) : rbtree::empty_tail(),
                         [n](auto* ptr) { node_t::delete_leaf(ptr, n); }};
            immer::detail::uninitialized_copy(
                values.begin(), values.end(), leaf.get()->leaf());
            leaves_        = std::move(leaves_).set(id, leaf);
            loaded_leaves_ = std::move(loaded_leaves_).set(leaf.get(), id);
            return leaf;
        }
    }

    node_ptr
    load_inner(node_id id, nodes_set_t loading_nodes, bool relaxed_allowed)
    {
        if (loading_nodes.count(id)) {
            throw pool_has_cycles{id};
        }

        if (auto* p = inners_.find(id)) {
            return *p;
        }

        auto* node_info = pool_.inners.find(id);
        if (!node_info) {
            throw invalid_node_id{id};
        }

        const auto children_ids = get_node_children(*node_info);

        const auto n         = children_ids.size();
        constexpr auto max_n = immer::detail::rbts::branches<B>;
        if (n > max_n) {
            throw invalid_children_count{id};
        }

        // The code doesn't handle very well a relaxed node with size zero.
        // Pretend it's a strict node.
        const bool is_relaxed = node_info->relaxed && n > 0;

        if (!is_relaxed) {
            // Children of a non-relaxed node are not allowed to be relaxed.
            relaxed_allowed = false;
        }

        if (is_relaxed && !relaxed_allowed) {
            throw relaxed_node_not_allowed_exception{id};
        }

        /**
         * We have to have the same behavior of inner nodes deallocating
         * children as vectors themselves, otherwise memory leaks appear.
         *
         * Specifically this: children's ref-counts must be decreased only when
         * the inner node that references them is deleted.
         */
        const auto children = [&] {
            /**
             * NOTE: load_children may throw an exception if same-depth
             * validation doesn't pass. Be careful with release_full, nodes will
             * not be freed automatically.
             */
            auto children_ptrs =
                load_children(id,
                              children_ids,
                              std::move(loading_nodes).insert(id),
                              relaxed_allowed);
            auto result = immer::vector<
                immer::persist::detail::ptr_with_deleter<node_t>>{};
            for (auto& item : children_ptrs) {
                result =
                    std::move(result).push_back(std::move(item).release_full());
            }
            return result;
        }();
        const auto delete_children = [children]() {
            for (const auto& ptr : children) {
                ptr.dec();
            }
        };

        auto inner = is_relaxed ? node_ptr{node_t::make_inner_r_n(n),
                                           [n, delete_children](auto* ptr) {
                                               node_t::delete_inner_r(ptr, n);
                                               delete_children();
                                           }}
                                : node_ptr{n ? node_t::make_inner_n(n)
                                             : rbtree::empty_root(),
                                           [n, delete_children](auto* ptr) {
                                               node_t::delete_inner(ptr, n);
                                               delete_children();
                                           }};
        if (is_relaxed) {
            inner.get()->relaxed()->d.count = n;
        }

        {
            auto running_size = std::size_t{};
            for (const auto& [index, child_node_id] :
                 boost::adaptors::index(children_ids)) {
                inner.get()->inner()[index] = children[index].ptr;
                if (is_relaxed) {
                    running_size += get_node_size(child_node_id);
                    inner.get()->relaxed()->d.sizes[index] = running_size;
                }
            }
        }

        inners_        = std::move(inners_).set(id, inner);
        loaded_inners_ = std::move(loaded_inners_).set(inner.get(), id);
        return inner;
    }

    node_ptr
    load_some_node(node_id id, nodes_set_t loading_nodes, bool relaxed_allowed)
    {
        // Unknown type: leaf, inner or relaxed
        if (pool_.leaves.count(id)) {
            return load_leaf(id);
        }
        if (pool_.inners.count(id)) {
            return load_inner(id, std::move(loading_nodes), relaxed_allowed);
        }
        throw invalid_node_id{id};
    }

    immer::vector<node_id> get_node_children(const inner_node& node_info)
    {
        // Ignore empty children
        auto result = immer::vector<node_id>{};
        for (const auto& child_node_id : node_info.children) {
            const auto child_size = get_node_size(child_node_id);
            if (child_size) {
                result = std::move(result).push_back(child_node_id);
            }
        }
        return result;
    }

    std::size_t get_node_size(node_id id, nodes_set_t loading_nodes = {})
    {
        if (auto* p = sizes_.find(id)) {
            return *p;
        }
        auto size = [&] {
            if (auto* p = pool_.leaves.find(id)) {
                return p->data.size();
            }
            if (auto* p = pool_.inners.find(id)) {
                auto result   = std::size_t{};
                loading_nodes = std::move(loading_nodes).insert(id);
                for (const auto& child_id : p->children) {
                    if (loading_nodes.count(child_id)) {
                        throw pool_has_cycles{child_id};
                    }
                    result += get_node_size(child_id, loading_nodes);
                }
                return result;
            }
            throw invalid_node_id{id};
        }();
        sizes_ = std::move(sizes_).set(id, size);
        return size;
    }

    immer::detail::rbts::count_t get_node_depth(node_id id)
    {
        if (auto* p = depths_.find(id)) {
            return *p;
        }
        auto depth = [&]() -> immer::detail::rbts::count_t {
            if (auto* p = pool_.leaves.find(id)) {
                return 0;
            }
            if (auto* p = pool_.inners.find(id)) {
                if (p->children.empty()) {
                    return 1;
                } else {
                    return 1 + get_node_depth(p->children.front());
                }
            }
            throw invalid_node_id{id};
        }();
        depths_ = std::move(depths_).set(id, depth);
        return depth;
    }

    std::vector<node_ptr>
    load_children(node_id id,
                  const immer::vector<node_id>& children_ids,
                  const nodes_set_t& loading_nodes,
                  bool relaxed_allowed)
    {
        auto children_depth = immer::detail::rbts::count_t{};
        auto result         = std::vector<node_ptr>{};
        for (const auto& child_node_id : children_ids) {
            // Better to load the node first and then check the depth, because
            // loading has extra protections against loops.
            auto child =
                load_some_node(child_node_id, loading_nodes, relaxed_allowed);

            const auto depth = get_node_depth(child_node_id);
            if (result.empty()) {
                children_depth = depth;
            } else if (depth != children_depth) {
                throw same_depth_children_exception{
                    id, children_depth, child_node_id, depth};
            }

            result.emplace_back(std::move(child));
        }
        return result;
    }

    template <class Tree>
    void verify_tree(const Tree& impl)
    {
        const auto check_inner = [&](auto&& pos,
                                     auto&& visit,
                                     bool visiting_relaxed) {
            const auto* id = loaded_inners_.find(pos.node());
            if (!id) {
                if (loaded_leaves_.find(pos.node())) {
                    throw std::logic_error{"A node is expected to be an inner "
                                           "node but it's actually a leaf"};
                }

                throw std::logic_error{"Inner node of a freshly loaded "
                                       "vector is unknown"};
            }
            const auto* info = pool_.inners.find(*id);
            assert(info);
            if (!info) {
                throw std::logic_error{
                    "No info for the just loaded inner node"};
            }

            /**
             * NOTE: Not sure how useful this check is.
             */
            // if (visiting_relaxed != info->relaxed) {
            //     throw std::logic_error{fmt::format(
            //         "Node {} is expected to be relaxed == {} but it is {}",
            //         *id,
            //         info->relaxed,
            //         visiting_relaxed)};
            // }

            const auto expected_count = pos.count();
            const auto real_count     = get_node_children(*info).size();
            if (expected_count != real_count) {
                throw vector_corrupted_exception{
                    *id, expected_count, real_count};
            }

            pos.each(detail::visitor_helper{},
                     [&visit](auto any_tag, auto& child_pos, auto&&) {
                         visit(child_pos);
                     });
        };

        impl.traverse(
            detail::visitor_helper{},
            boost::hana::overload(
                [&](detail::regular_pos_tag, auto&& pos, auto&& visit) {
                    const bool visiting_relaxed = false;
                    check_inner(pos, visit, visiting_relaxed);
                },
                [&](detail::relaxed_pos_tag, auto&& pos, auto&& visit) {
                    const bool visiting_relaxed = true;
                    check_inner(pos, visit, visiting_relaxed);
                },
                [&](detail::leaf_pos_tag, auto&& pos, auto&& visit) {
                    const auto* id = loaded_leaves_.find(pos.node());
                    assert(id);
                    if (!id) {
                        throw std::logic_error{
                            "Leaf of a freshly loaded vector is unknown"};
                    }
                    const auto* info = pool_.leaves.find(*id);
                    assert(info);
                    if (!info) {
                        throw std::logic_error{
                            "No info for the just loaded leaf"};
                    }

                    const auto expected_count = pos.count();
                    const auto real_count     = info->data.size();
                    if (expected_count != real_count) {
                        throw vector_corrupted_exception{
                            *id, expected_count, real_count};
                    }
                }));
    }

    void validate_bits_params() const
    {
        auto good = pool_.bits == B && pool_.bits_leaf == BL;
        if (!good) {
            throw incompatible_bits_parameters{
                B, BL, pool_.bits, pool_.bits_leaf};
        }
    }

private:
    const Pool pool_{};
    const TransformF transform_{};
    immer::map<node_id, node_ptr> leaves_;
    immer::map<node_id, node_ptr> inners_;
    immer::map<node_t*, node_id> loaded_leaves_;
    immer::map<node_t*, node_id> loaded_inners_;
    immer::map<node_id, std::size_t> sizes_;
    immer::map<node_id, immer::detail::rbts::count_t> depths_;
};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL,
          class Pool       = input_pool<T>,
          class TransformF = boost::hana::id_t>
class vector_loader
{
public:
    explicit vector_loader(Pool pool)
        : loader{std::move(pool)}
    {
    }

    explicit vector_loader(Pool pool, TransformF transform)
        : loader{std::move(pool), std::move(transform)}
    {
    }

    auto load(container_id id) { return loader.load_vector(id); }

private:
    loader<T, MemoryPolicy, B, BL, Pool, TransformF> loader;
};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
vector_loader<T, MemoryPolicy, B, BL>
make_loader_for(const immer::vector<T, MemoryPolicy, B, BL>&,
                input_pool<T> pool)
{
    return vector_loader<T, MemoryPolicy, B, BL>{std::move(pool)};
}

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL,
          class Pool       = input_pool<T>,
          class TransformF = boost::hana::id_t>
class flex_vector_loader
{
public:
    explicit flex_vector_loader(Pool pool)
        : loader{std::move(pool)}
    {
    }

    explicit flex_vector_loader(Pool pool, TransformF transform)
        : loader{std::move(pool), std::move(transform)}
    {
    }

    auto load(container_id id) { return loader.load_flex_vector(id); }

private:
    loader<T, MemoryPolicy, B, BL, Pool, TransformF> loader;
};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
flex_vector_loader<T, MemoryPolicy, B, BL>
make_loader_for(const immer::flex_vector<T, MemoryPolicy, B, BL>&,
                input_pool<T> pool)
{
    return flex_vector_loader<T, MemoryPolicy, B, BL>{std::move(pool)};
}

} // namespace immer::persist::rbts
