#pragma once

#include <cereal/cereal.hpp>

#include <immer/map.hpp>

namespace cereal {

template <typename Element>
auto get_auto_id(const Element& x) -> decltype(x.id)
{
    return x.id;
}

template <typename K, typename T>
struct has_auto_id : std::false_type
{};

template <typename T>
struct has_auto_id<std::decay_t<decltype(get_auto_id(std::declval<T>()))>, T>
    : std::true_type
{};

template <typename Archive,
          typename K,
          typename T,
          typename H,
          typename E,
          typename MP,
          std::uint32_t B>
std::enable_if_t<has_auto_id<K, T>::value>
CEREAL_LOAD_FUNCTION_NAME(Archive& ar, immer::map<K, T, H, E, MP, B>& m)
{
    size_type size;
    ar(make_size_tag(size));
    m = {};

    for (auto i = size_type{}; i < size; ++i) {
        T x;
        ar(x);
        auto id = get_auto_id(x);
        m       = std::move(m).set(std::move(id), std::move(x));
    }
    if (size != m.size())
        throw std::runtime_error{"duplicate ids?"};
}

template <typename Archive,
          typename K,
          typename T,
          typename H,
          typename E,
          typename MP,
          std::uint32_t B>
std::enable_if_t<has_auto_id<K, T>::value>
CEREAL_SAVE_FUNCTION_NAME(Archive& ar, const immer::map<K, T, H, E, MP, B>& m)
{
    ar(make_size_tag(static_cast<size_type>(m.size())));
    for (auto&& v : m)
        ar(v.second);
}

template <typename Archive,
          typename K,
          typename T,
          typename H,
          typename E,
          typename MP,
          std::uint32_t B>
std::enable_if_t<!has_auto_id<K, T>::value>
CEREAL_LOAD_FUNCTION_NAME(Archive& ar, immer::map<K, T, H, E, MP, B>& m)
{
    size_type size;
    ar(make_size_tag(size));
    m = {};

    for (auto i = size_type{}; i < size; ++i) {
        K k;
        T x;
        ar(make_map_item(k, x));
        m = std::move(m).set(std::move(k), std::move(x));
    }
    if (size != m.size())
        throw std::runtime_error{"duplicate ids?"};
}

template <typename Archive,
          typename K,
          typename T,
          typename H,
          typename E,
          typename MP,
          std::uint32_t B>
std::enable_if_t<!has_auto_id<K, T>::value>
CEREAL_SAVE_FUNCTION_NAME(Archive& ar, const immer::map<K, T, H, E, MP, B>& m)
{
    ar(make_size_tag(static_cast<size_type>(m.size())));
    for (auto&& v : m)
        ar(make_map_item(v.first, v.second));
}

} // namespace cereal
