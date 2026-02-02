#pragma once

#include <immer/extra/persist/detail/common/pool.hpp>
#include <immer/extra/persist/detail/traits.hpp>
#include <immer/extra/persist/errors.hpp>

#include <immer/box.hpp>
#include <immer/map.hpp>
#include <immer/vector.hpp>

#include <cereal/cereal.hpp>

#include <boost/hana/functional/id.hpp>

namespace immer::persist::box {

template <typename T, typename MemoryPolicy>
struct output_pool
{
    immer::map<container_id, immer::box<T, MemoryPolicy>> boxes;

    immer::map<const void*, container_id> ids;

    friend bool operator==(const output_pool& left, const output_pool& right)
    {
        return left.boxes == right.boxes;
    }

    friend bool operator!=(const output_pool& left, const output_pool& right)
    {
        return left.boxes != right.boxes;
    }

    template <class Archive>
    void save(Archive& ar) const
    {
        ar(cereal::make_size_tag(static_cast<cereal::size_type>(boxes.size())));
        for_each_ordered([&](const auto& box) { ar(box.get()); });
    }

    template <typename F>
    void for_each_ordered(F&& f) const
    {
        for (auto index = std::size_t{}; index < boxes.size(); ++index) {
            auto* p = boxes.find(container_id{index});
            assert(p);
            f(*p);
        }
    }
};

template <typename T, typename MemoryPolicy>
std::pair<output_pool<T, MemoryPolicy>, container_id>
get_box_id(output_pool<T, MemoryPolicy> pool,
           const immer::box<T, MemoryPolicy>& box)
{
    auto* ptr_void = static_cast<const void*>(box.impl());
    if (auto* maybe_id = pool.ids.find(ptr_void)) {
        return {std::move(pool), *maybe_id};
    }

    const auto id = container_id{pool.ids.size()};
    pool.ids      = std::move(pool.ids).set(ptr_void, id);
    return {std::move(pool), id};
}

template <typename T, typename MemoryPolicy>
struct input_pool
{
    immer::vector<immer::box<T, MemoryPolicy>> boxes;

    friend bool operator==(const input_pool& left, const input_pool& right)
    {
        return left.boxes == right.boxes;
    }

    template <class Archive>
    void load(Archive& ar)
    {
        cereal::size_type size;
        ar(cereal::make_size_tag(size));

        for (auto i = cereal::size_type{}; i < size; ++i) {
            T x;
            ar(x);
            boxes = std::move(boxes).push_back(std::move(x));
        }
    }

    /**
     * merge_previous is a function where some processing might be done while
     * reloading the pools. In the case of boxes, while we reload the pool
     * continuously (it might take several passes to fully load), we carry over
     * the older boxes (when they're equal) in order to achieve better
     * structural sharing.
     */
    void merge_previous(const input_pool& other)
    {
        if (boxes.size() != other.boxes.size()) {
            return;
        }

        auto result = immer::vector<immer::box<T, MemoryPolicy>>{};
        for (auto i = std::size_t{}; i < boxes.size(); ++i) {
            if (boxes[i] == other.boxes[i]) {
                // While it's the same, prefer the old one
                result = std::move(result).push_back(other.boxes[i]);
            } else {
                result = std::move(result).push_back(boxes[i]);
            }
        }
        boxes = std::move(result);
    }
};

template <typename T, typename MemoryPolicy>
input_pool<T, MemoryPolicy>
to_input_pool(const output_pool<T, MemoryPolicy>& pool)
{
    auto result = input_pool<T, MemoryPolicy>{};
    pool.for_each_ordered([&](const auto& box) {
        result.boxes = std::move(result.boxes).push_back(box);
    });
    return result;
}

template <typename T, typename MemoryPolicy>
std::pair<output_pool<T, MemoryPolicy>, container_id>
add_to_pool(immer::box<T, MemoryPolicy> box, output_pool<T, MemoryPolicy> pool)
{
    auto id            = container_id{};
    std::tie(pool, id) = get_box_id(std::move(pool), box);

    if (pool.boxes.count(id)) {
        // Already been saved
        return {std::move(pool), id};
    }

    pool.boxes = std::move(pool.boxes).set(id, std::move(box));
    return {std::move(pool), id};
}

template <typename T,
          typename MemoryPolicy,
          typename Pool       = input_pool<T, MemoryPolicy>,
          typename TransformF = boost::hana::id_t>
class loader
{
public:
    explicit loader(Pool pool)
        : pool_{std::move(pool)}
    {
    }

    explicit loader(Pool pool, TransformF transform)
        : pool_{std::move(pool)}
        , transform_{std::move(transform)}
    {
    }

    immer::box<T, MemoryPolicy> load(container_id id)
    {
        if (id.value >= pool_.boxes.size()) {
            throw invalid_container_id{id};
        }
        if constexpr (std::is_same_v<TransformF, boost::hana::id_t>) {
            return pool_.boxes[id.value];
        } else {
            if (auto* b = boxes_.find(id)) {
                return *b;
            }
            const auto& old_box = pool_.boxes[id.value];
            auto new_box =
                immer::box<T, MemoryPolicy>{transform_(old_box.get())};
            boxes_ = std::move(boxes_).set(id, new_box);
            return new_box;
        }
    }

private:
    const Pool pool_{};
    const TransformF transform_{};
    immer::map<container_id, immer::box<T, MemoryPolicy>> boxes_;
};

template <typename T, typename MemoryPolicy>
loader<T, MemoryPolicy> make_loader_for(const immer::box<T, MemoryPolicy>&,
                                        input_pool<T, MemoryPolicy> pool)
{
    return loader<T, MemoryPolicy>{std::move(pool)};
}

} // namespace immer::persist::box

namespace immer::persist::detail {

template <typename T, typename MemoryPolicy>
struct container_traits<immer::box<T, MemoryPolicy>>
{
    using output_pool_t = box::output_pool<T, MemoryPolicy>;
    using input_pool_t  = box::input_pool<T, MemoryPolicy>;

    template <typename Pool       = input_pool_t,
              typename TransformF = boost::hana::id_t>
    using loader_t = box::loader<T, MemoryPolicy, Pool, TransformF>;

    using container_id = immer::persist::container_id;

    template <class F>
    static auto transform(F&& func)
    {
        using U = std::decay_t<decltype(func(std::declval<T>()))>;
        return immer::box<U, MemoryPolicy>{};
    }
};

} // namespace immer::persist::detail
