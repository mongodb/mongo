#pragma once

#include <immer/extra/cereal/immer_array.hpp>
#include <immer/extra/cereal/immer_vector.hpp>
#include <immer/extra/persist/detail/common/pool.hpp>
#include <immer/extra/persist/detail/traits.hpp>
#include <immer/extra/persist/errors.hpp>

#include <boost/hana/functional/id.hpp>

namespace immer::persist::array {

template <typename T, typename MemoryPolicy>
struct output_pool
{
    immer::map<container_id, immer::array<T, MemoryPolicy>> arrays;

    immer::map<const void*, container_id> ids;

    friend bool operator==(const output_pool& left, const output_pool& right)
    {
        return left.arrays == right.arrays;
    }

    friend bool operator!=(const output_pool& left, const output_pool& right)
    {
        return left.arrays != right.arrays;
    }

    template <class Archive>
    void save(Archive& ar) const
    {
        ar(cereal::make_size_tag(
            static_cast<cereal::size_type>(arrays.size())));
        for_each_ordered([&](const auto& array) { ar(array); });
    }

    template <typename F>
    void for_each_ordered(F&& f) const
    {
        for (auto index = std::size_t{}; index < arrays.size(); ++index) {
            auto* p = arrays.find(container_id{index});
            assert(p);
            f(*p);
        }
    }
};

template <typename T, typename MemoryPolicy>
std::pair<output_pool<T, MemoryPolicy>, container_id>
get_id(output_pool<T, MemoryPolicy> pool,
       const immer::array<T, MemoryPolicy>& array)
{
    auto* ptr_void = static_cast<const void*>(array.identity());
    if (auto* maybe_id = pool.ids.find(ptr_void)) {
        return {std::move(pool), *maybe_id};
    }

    const auto id = container_id{pool.ids.size()};
    pool.ids      = std::move(pool.ids).set(ptr_void, id);
    return {std::move(pool), id};
}

template <typename T, typename MemoryPolicy>
std::pair<output_pool<T, MemoryPolicy>, container_id>
add_to_pool(immer::array<T, MemoryPolicy> array,
            output_pool<T, MemoryPolicy> pool)
{
    auto id            = container_id{};
    std::tie(pool, id) = get_id(std::move(pool), array);

    if (pool.arrays.count(id)) {
        // Already been saved
        return {std::move(pool), id};
    }

    pool.arrays = std::move(pool.arrays).set(id, std::move(array));
    return {std::move(pool), id};
}

template <typename T, typename MemoryPolicy>
struct input_pool
{
    immer::vector<immer::array<T, MemoryPolicy>> arrays;

    friend bool operator==(const input_pool& left, const input_pool& right)
    {
        return left.arrays == right.arrays;
    }

    template <class Archive>
    void load(Archive& ar)
    {
        cereal::load(ar, arrays);
    }

    void merge_previous(const input_pool& other)
    {
        if (arrays.size() != other.arrays.size()) {
            return;
        }

        auto result = immer::vector<immer::array<T, MemoryPolicy>>{};
        for (auto i = std::size_t{}; i < arrays.size(); ++i) {
            if (arrays[i] == other.arrays[i]) {
                // While it's the same, prefer the old one
                result = std::move(result).push_back(other.arrays[i]);
            } else {
                result = std::move(result).push_back(arrays[i]);
            }
        }
        arrays = std::move(result);
    }
};

template <typename T, typename MemoryPolicy>
input_pool<T, MemoryPolicy>
to_input_pool(const output_pool<T, MemoryPolicy>& pool)
{
    auto result = input_pool<T, MemoryPolicy>{};
    pool.for_each_ordered([&](const auto& array) {
        result.arrays = std::move(result.arrays).push_back(array);
    });
    return result;
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

    immer::array<T, MemoryPolicy> load(container_id id)
    {
        if (id.value >= pool_.arrays.size()) {
            throw invalid_container_id{id};
        }
        if constexpr (std::is_same_v<TransformF, boost::hana::id_t>) {
            return pool_.arrays[id.value];
        } else {
            if (auto* b = arrays_.find(id)) {
                return *b;
            }
            const auto& old_array = pool_.arrays[id.value];
            auto new_array        = immer::array<T, MemoryPolicy>{};
            for (const auto& item : old_array) {
                new_array = std::move(new_array).push_back(transform_(item));
            }
            arrays_ = std::move(arrays_).set(id, new_array);
            return new_array;
        }
    }

private:
    const Pool pool_{};
    const TransformF transform_{};
    immer::map<container_id, immer::array<T, MemoryPolicy>> arrays_;
};

template <typename T, typename MemoryPolicy>
loader<T, MemoryPolicy> make_loader_for(const immer::array<T, MemoryPolicy>&,
                                        input_pool<T, MemoryPolicy> pool)
{
    return loader<T, MemoryPolicy>{std::move(pool)};
}

} // namespace immer::persist::array

namespace immer::persist::detail {
template <typename T, typename MemoryPolicy>
struct container_traits<immer::array<T, MemoryPolicy>>
{
    using output_pool_t = array::output_pool<T, MemoryPolicy>;
    using input_pool_t  = array::input_pool<T, MemoryPolicy>;

    template <typename Pool       = input_pool_t,
              typename TransformF = boost::hana::id_t>
    using loader_t = array::loader<T, MemoryPolicy, Pool, TransformF>;

    using container_id = immer::persist::container_id;

    template <class F>
    static auto transform(F&& func)
    {
        using U = std::decay_t<decltype(func(std::declval<T>()))>;
        return immer::array<U, MemoryPolicy>{};
    }
};

} // namespace immer::persist::detail
