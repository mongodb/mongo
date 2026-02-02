#pragma once

#include <cereal/cereal.hpp>
#include <immer/vector.hpp>

namespace cereal {

template <typename Archive,
          typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
void CEREAL_LOAD_FUNCTION_NAME(Archive& ar,
                               immer::vector<T, MemoryPolicy, B, BL>& m)
{
    size_type size;
    ar(make_size_tag(size));
    m = {};

    for (auto i = size_type{}; i < size; ++i) {
        T x;
        ar(x);
        m = std::move(m).push_back(std::move(x));
    }
}

template <typename Archive,
          typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
void CEREAL_SAVE_FUNCTION_NAME(Archive& ar,
                               const immer::vector<T, MemoryPolicy, B, BL>& m)
{
    ar(make_size_tag(static_cast<size_type>(m.size())));
    for (auto&& v : m)
        ar(v);
}

template <typename Archive,
          typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
void CEREAL_LOAD_FUNCTION_NAME(Archive& ar,
                               immer::flex_vector<T, MemoryPolicy, B, BL>& m)
{
    size_type size;
    ar(make_size_tag(size));
    m = {};

    for (auto i = size_type{}; i < size; ++i) {
        T x;
        ar(x);
        m = std::move(m).push_back(std::move(x));
    }
}

template <typename Archive,
          typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
void CEREAL_SAVE_FUNCTION_NAME(
    Archive& ar, const immer::flex_vector<T, MemoryPolicy, B, BL>& m)
{
    ar(make_size_tag(static_cast<size_type>(m.size())));
    for (auto&& v : m)
        ar(v);
}

} // namespace cereal
