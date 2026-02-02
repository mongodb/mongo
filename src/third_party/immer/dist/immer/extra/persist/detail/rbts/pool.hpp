#pragma once

#include <immer/extra/cereal/immer_map.hpp>
#include <immer/extra/cereal/immer_vector.hpp>
#include <immer/extra/persist/detail/alias.hpp>
#include <immer/extra/persist/detail/cereal/compact_map.hpp>
#include <immer/extra/persist/detail/common/pool.hpp>

#include <immer/array.hpp>
#include <immer/flex_vector.hpp>
#include <immer/map.hpp>
#include <immer/vector.hpp>

#include <cereal/cereal.hpp>

namespace immer::persist::rbts {

struct rbts_info
{
    node_id root;
    node_id tail;

    auto tie() const { return std::tie(root, tail); }

    friend bool operator==(const rbts_info& left, const rbts_info& right)
    {
        return left.tie() == right.tie();
    }

    template <class Archive>
    void serialize(Archive& ar)
    {
        ar(CEREAL_NVP(root), CEREAL_NVP(tail));
    }
};

} // namespace immer::persist::rbts

template <>
struct std::hash<immer::persist::rbts::rbts_info>
{
    auto operator()(const immer::persist::rbts::rbts_info& x) const
    {
        const auto boost_combine = [](std::size_t& seed, std::size_t hash) {
            seed ^= hash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        };

        auto seed = std::size_t{};
        boost_combine(seed,
                      hash<immer::persist::node_id::rep_t>{}(x.root.value));
        boost_combine(seed,
                      hash<immer::persist::node_id::rep_t>{}(x.tail.value));
        return seed;
    }
};

namespace immer::persist::rbts {

struct inner_node
{
    immer::vector<node_id> children;
    bool relaxed = {};

    auto tie() const { return std::tie(children, relaxed); }

    friend bool operator==(const inner_node& left, const inner_node& right)
    {
        return left.tie() == right.tie();
    }

    template <class Archive>
    void serialize(Archive& ar)
    {
        ar(CEREAL_NVP(children), CEREAL_NVP(relaxed));
    }
};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct output_pool
{
    immer::map<node_id, detail::values_save<T>> leaves;
    immer::map<node_id, inner_node> inners;
    immer::vector<rbts_info> vectors;

    immer::map<rbts_info, container_id> rbts_to_id;
    immer::map<const void*, node_id> node_ptr_to_id;

    // Saving the persisted vectors, so that no mutations are allowed to happen.
    immer::vector<immer::vector<T, MemoryPolicy, B, BL>> saved_vectors;
    immer::vector<immer::flex_vector<T, MemoryPolicy, B, BL>>
        saved_flex_vectors;

    auto tie() const { return std::tie(leaves, inners, vectors); }

    friend bool operator==(const output_pool& left, const output_pool& right)
    {
        return left.tie() == right.tie();
    }

    friend bool operator!=(const output_pool& left, const output_pool& right)
    {
        return left.tie() != right.tie();
    }

    template <class Archive>
    void save(Archive& ar) const
    {
        auto leaves_copy = leaves;
        auto inners_copy = inners;
        ar(CEREAL_NVP(B),
           CEREAL_NVP(BL),
           cereal::make_nvp("leaves", detail::make_compact_map(leaves_copy)),
           cereal::make_nvp("inners", detail::make_compact_map(inners_copy)),
           CEREAL_NVP(vectors));
    }
};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
inline auto make_output_pool_for(const immer::vector<T, MemoryPolicy, B, BL>&)
{
    return output_pool<T, MemoryPolicy, B, BL>{};
}

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
inline auto
make_output_pool_for(const immer::flex_vector<T, MemoryPolicy, B, BL>&)
{
    return output_pool<T, MemoryPolicy, B, BL>{};
}

template <typename T>
struct input_pool
{
    immer::detail::rbts::bits_t bits{};
    immer::detail::rbts::bits_t bits_leaf{};
    immer::map<node_id, detail::values_load<T>> leaves;
    immer::map<node_id, inner_node> inners;
    immer::vector<rbts_info> vectors;

    auto tie() const
    {
        return std::tie(bits, bits_leaf, leaves, inners, vectors);
    }

    friend bool operator==(const input_pool& left, const input_pool& right)
    {
        return left.tie() == right.tie();
    }

    template <class Archive>
    void load(Archive& ar)
    {
        auto& B  = bits;
        auto& BL = bits_leaf;
        ar(CEREAL_NVP(B),
           CEREAL_NVP(BL),
           cereal::make_nvp("leaves", detail::make_compact_map(leaves)),
           cereal::make_nvp("inners", detail::make_compact_map(inners)),
           CEREAL_NVP(vectors));
    }

    void merge_previous(const input_pool& other) {}
};

// This is needed to be able to use the pool that was not read from JSON
// because .data is set only while reading from JSON.
template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
input_pool<T> to_input_pool(output_pool<T, MemoryPolicy, B, BL> ar)
{
    auto leaves = immer::map<node_id, detail::values_load<T>>{};
    for (const auto& item : ar.leaves) {
        leaves = std::move(leaves).set(item.first, item.second);
    }

    return {
        .bits      = B,
        .bits_leaf = BL,
        .leaves    = std::move(leaves),
        .inners    = std::move(ar.inners),
        .vectors   = std::move(ar.vectors),
    };
}

} // namespace immer::persist::rbts
