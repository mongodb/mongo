#pragma once

#include <cereal/cereal.hpp>

#include <immer/set.hpp>

namespace cereal {

template <typename Archive,
          typename T,
          typename H,
          typename E,
          typename MP,
          std::uint32_t B>
void CEREAL_LOAD_FUNCTION_NAME(Archive& ar, immer::set<T, H, E, MP, B>& m)
{
    size_type size;
    ar(make_size_tag(size));
    m = {};

    for (auto i = size_type{}; i < size; ++i) {
        T x;
        ar(x);
        m = std::move(m).insert(std::move(x));
    }
    if (size != m.size())
        throw std::runtime_error{"duplicate items?"};
}

template <typename Archive,
          typename T,
          typename H,
          typename E,
          typename MP,
          std::uint32_t B>
void CEREAL_SAVE_FUNCTION_NAME(Archive& ar, const immer::set<T, H, E, MP, B>& m)
{
    ar(make_size_tag(static_cast<size_type>(m.size())));
    for (auto&& v : m)
        ar(v);
}

} // namespace cereal
