#pragma once

#include <cereal/cereal.hpp>
#include <immer/box.hpp>

namespace cereal {

template <typename Archive, typename T, typename MemoryPolicy>
void CEREAL_LOAD_FUNCTION_NAME(Archive& ar, immer::box<T, MemoryPolicy>& m)
{
    T x;
    ar(x);
    m = std::move(x);
}

template <typename Archive, typename T, typename MemoryPolicy>
void CEREAL_SAVE_FUNCTION_NAME(Archive& ar,
                               const immer::box<T, MemoryPolicy>& m)
{
    ar(m.get());
}

} // namespace cereal
