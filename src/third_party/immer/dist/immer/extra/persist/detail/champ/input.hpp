#pragma once

#include <immer/extra/persist/detail/champ/pool.hpp>
#include <immer/extra/persist/detail/node_ptr.hpp>
#include <immer/extra/persist/errors.hpp>

#include <immer/flex_vector.hpp>

#include <boost/hana/functional/id.hpp>
#include <boost/range/adaptor/indexed.hpp>

namespace immer::persist::champ {

class children_count_corrupted_exception : public pool_exception
{
public:
    children_count_corrupted_exception(node_id id,
                                       std::uint64_t nodemap,
                                       std::size_t expected_count,
                                       std::size_t real_count)
        : pool_exception{fmt::format(
              "Loaded container is corrupted. Inner "
              "node ID {} has nodemap {} which means it should have {} "
              "children but it has {}",
              id,
              nodemap,
              expected_count,
              real_count)}
    {
    }
};

class data_count_corrupted_exception : public pool_exception
{
public:
    data_count_corrupted_exception(node_id id,
                                   std::uint64_t datamap,
                                   std::size_t expected_count,
                                   std::size_t real_count)
        : pool_exception{fmt::format(
              "Loaded container is corrupted. Inner "
              "node ID {} has datamap {} which means it should contain {} "
              "values but it has {}",
              id,
              datamap,
              expected_count,
              real_count)}
    {
    }
};

template <class T,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B,
          typename NodesLoad  = nodes_load<T, B>,
          typename TransformF = boost::hana::id_t>
class nodes_loader
{
public:
    using champ_t =
        immer::detail::hamts::champ<T, Hash, Equal, MemoryPolicy, B>;
    using node_t   = typename champ_t::node_t;
    using node_ptr = immer::persist::detail::node_ptr<node_t>;

    using values_t = immer::flex_vector<immer::array<T>>;

    explicit nodes_loader(NodesLoad pool)
        : pool_{std::move(pool)}
    {
    }

    explicit nodes_loader(NodesLoad pool, TransformF transform)
        : pool_{std::move(pool)}
        , transform_{std::move(transform)}
    {
    }

    std::pair<node_ptr, values_t> load_collision(node_id id)
    {
        if (auto* p = collisions_.find(id)) {
            return *p;
        }

        if (id.value >= pool_.size()) {
            throw invalid_node_id{id};
        }

        const auto& node_info = pool_[id.value];
        const auto values     = get_values(node_info.values.data);

        const auto n = values.size();
        auto node    = node_ptr{node_t::make_collision_n(n),
                             [](auto* ptr) { node_t::delete_collision(ptr); }};
        immer::detail::uninitialized_copy(
            values.begin(), values.end(), node.get()->collisions());
        auto result = std::make_pair(std::move(node), values_t{values});
        collisions_ = std::move(collisions_).set(id, result);
        return result;
    }

    std::pair<node_ptr, values_t> load_inner(node_id id)
    {
        if (auto* p = inners_.find(id)) {
            return *p;
        }

        if (id.value >= pool_.size()) {
            throw invalid_node_id{id};
        }

        const auto& node_info = pool_[id.value];

        const auto children_count = node_info.children.size();
        const auto values_count   = node_info.values.data.size();

        // Loading validation
        {
            const auto expected_count =
                immer::detail::hamts::popcount(node_info.nodemap);
            if (expected_count != children_count) {
                throw children_count_corrupted_exception{
                    id, node_info.nodemap, expected_count, children_count};
            }
        }

        {
            const auto expected_count =
                immer::detail::hamts::popcount(node_info.datamap);
            if (expected_count != values_count) {
                throw data_count_corrupted_exception{
                    id, node_info.datamap, expected_count, values_count};
            }
        }

        const auto node_values = get_values(node_info.values.data);

        auto values = values_t{};

        // Load children
        const auto children = [&values, &node_info, this] {
            auto [children_ptrs, children_values] =
                load_children(node_info.children);

            if (!children_values.empty()) {
                values = std::move(values) + children_values;
            }

            /**
             * NOTE: Be careful with release_full and exceptions, nodes will not
             * be freed automatically.
             */
            auto result = immer::vector<detail::ptr_with_deleter<node_t>>{};
            for (auto& child : children_ptrs) {
                result = std::move(result).push_back(
                    std::move(child).release_full());
            }
            return result;
        }();
        const auto delete_children = [children]() {
            for (const auto& ptr : children) {
                ptr.dec();
            }
        };

        auto inner =
            node_ptr{node_t::make_inner_n(children_count, values_count),
                     [delete_children](auto* ptr) {
                         node_t::delete_inner(ptr);
                         delete_children();
                     }};
        inner.get()->impl.d.data.inner.nodemap = node_info.nodemap;
        inner.get()->impl.d.data.inner.datamap = node_info.datamap;

        // Values
        if (values_count) {
            immer::detail::uninitialized_copy(
                node_values.begin(), node_values.end(), inner.get()->values());
            values = std::move(values).push_back(node_values);
        }

        // Set children
        for (const auto& [index, child_ptr] :
             boost::adaptors::index(children)) {
            inner.get()->children()[index] = child_ptr.ptr;
        }

        inners_ = std::move(inners_).set(id, std::make_pair(inner, values));
        return {std::move(inner), std::move(values)};
    }

    std::pair<node_ptr, values_t> load_some_node(node_id id)
    {
        if (id.value >= pool_.size()) {
            throw invalid_node_id{id};
        }

        if (pool_[id.value].collisions) {
            return load_collision(id);
        } else {
            return load_inner(id);
        }
    }

    std::pair<std::vector<node_ptr>, values_t>
    load_children(const immer::vector<node_id>& children_ids)
    {
        auto children = std::vector<node_ptr>{};
        auto values   = values_t{};
        for (const auto& child_node_id : children_ids) {
            auto [child, child_values] = load_some_node(child_node_id);
            if (!child) {
                throw pool_exception{
                    fmt::format("Failed to load node ID {}", child_node_id)};
            }

            if (!child_values.empty()) {
                values = std::move(values) + child_values;
            }

            children.push_back(std::move(child));
        }
        return {std::move(children), std::move(values)};
    }

private:
    template <class Array>
    immer::array<T> get_values(const Array& array) const
    {
        if constexpr (std::is_same_v<TransformF, boost::hana::id_t>) {
            return array;
        } else {
            auto transformed_values = std::vector<T>{};
            for (const auto& item : array) {
                transformed_values.push_back(transform_(item));
            }
            return immer::array<T>{transformed_values.begin(),
                                   transformed_values.end()};
        }
    }

private:
    const NodesLoad pool_{};
    const TransformF transform_{};
    immer::map<node_id, std::pair<node_ptr, values_t>> collisions_;
    immer::map<node_id, std::pair<node_ptr, values_t>> inners_;
};

} // namespace immer::persist::champ
