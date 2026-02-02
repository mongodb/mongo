#pragma once

#include <immer/extra/persist/types.hpp>

#include <immer/array.hpp>
#include <immer/map.hpp>

#include <cereal/types/utility.hpp>

namespace immer::persist::detail {

/**
 * This type is used in container-specific pools to refer to the actual data
 * from the leaf nodes. The values are serialized as a list.
 */
template <class T>
struct values_save
{
    const T* begin = nullptr;
    const T* end   = nullptr;

    auto tie() const { return std::tie(begin, end); }

    friend bool operator==(const values_save& left, const values_save& right)
    {
        return left.tie() == right.tie();
    }
};

/**
 * This type is used in container-specific pools to contain the actual data for
 * the leaf nodes. The values are deserialized from a list.
 */
template <class T>
struct values_load
{
    immer::array<T> data;

    values_load() = default;

    values_load(immer::array<T> data_)
        : data{std::move(data_)}
    {
    }

    values_load(const values_save<T>& save)
        : data{save.begin, save.end}
    {
    }

    friend bool operator==(const values_load& left, const values_load& right)
    {
        return left.data == right.data;
    }
};

template <class Archive, class T>
void save(Archive& ar, const values_save<T>& value)
{
    ar(cereal::make_size_tag(
        static_cast<cereal::size_type>(value.end - value.begin)));
    for (auto p = value.begin; p != value.end; ++p) {
        ar(*p);
    }
}

template <class Archive, class T>
void load(Archive& ar, values_load<T>& m)
{
    cereal::size_type size;
    ar(cereal::make_size_tag(size));

    for (auto i = cereal::size_type{}; i < size; ++i) {
        T x;
        ar(x);
        m.data = std::move(m.data).push_back(std::move(x));
    }
}

} // namespace immer::persist::detail
