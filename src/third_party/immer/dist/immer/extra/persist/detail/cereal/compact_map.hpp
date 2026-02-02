#pragma once

#include <cereal/cereal.hpp>

#include <immer/map.hpp>

namespace immer::persist::detail {

template <class K, class T>
struct compact_pair
{
    K key;
    T value;
};

template <class K, class T>
struct compact_map
{
    immer::map<K, T>& map;
};

template <class K, class T>
compact_map<K, T> make_compact_map(immer::map<K, T>& map)
{
    return compact_map<K, T>{map};
}

} // namespace immer::persist::detail

namespace cereal {

template <typename Archive, typename K, typename T>
void CEREAL_LOAD_FUNCTION_NAME(Archive& ar,
                               immer::persist::detail::compact_pair<K, T>& m)
{
    size_type size;
    ar(make_size_tag(size));
    if (size != 2) {
        throw Exception{"A pair must be a list of 2 elements"};
    }

    ar(m.key);
    ar(m.value);
}

template <typename Archive, typename K, typename T>
void CEREAL_SAVE_FUNCTION_NAME(
    Archive& ar, const immer::persist::detail::compact_pair<K, T>& m)
{
    ar(make_size_tag(static_cast<size_type>(2)));
    ar(m.key);
    ar(m.value);
}

template <typename Archive, typename K, typename T>
void CEREAL_LOAD_FUNCTION_NAME(Archive& ar,
                               immer::persist::detail::compact_map<K, T>& m)
{
    size_type size;
    ar(make_size_tag(size));
    m.map = {};

    for (auto i = size_type{}; i < size; ++i) {
        auto pair = immer::persist::detail::compact_pair<K, T>{};
        ar(pair);

        m.map =
            std::move(m.map).set(std::move(pair.key), std::move(pair.value));
    }
    if (size != m.map.size())
        throw Exception{"duplicate ids?"};
}

template <typename Archive, typename K, typename T>
void CEREAL_SAVE_FUNCTION_NAME(
    Archive& ar, const immer::persist::detail::compact_map<K, T>& m)
{
    ar(make_size_tag(static_cast<size_type>(m.map.size())));
    for (auto&& v : m.map) {
        ar(immer::persist::detail::compact_pair<K, T>{v.first, v.second});
    }
}

} // namespace cereal
