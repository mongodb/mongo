#pragma once

#include <fmt/format.h>

namespace immer::persist::detail {

template <class T, class Tag>
struct type_alias
{
    using rep_t = T;

    T value{};

    type_alias() = default;

    explicit type_alias(T value_)
        : value{std::move(value_)}
    {
    }

    friend bool operator==(const type_alias& left, const type_alias& right)
    {
        return left.value == right.value;
    }

    friend bool operator!=(const type_alias& left, const type_alias& right)
    {
        return left.value != right.value;
    }

    /**
     * This works only starting with fmt v10.
     */
    friend auto format_as(const type_alias& v) { return v.value; }

    template <class Archive>
    auto save_minimal(const Archive&) const
    {
        return value;
    }

    template <class Archive>
    auto load_minimal(const Archive&, const T& value_)
    {
        value = value_;
    }
};

} // namespace immer::persist::detail

/**
 * Have to use this for fmt v9.
 */
template <class T, class Tag>
struct fmt::formatter<immer::persist::detail::type_alias<T, Tag>> : formatter<T>
{
    template <typename FormatContext>
    auto format(const immer::persist::detail::type_alias<T, Tag>& value,
                FormatContext& ctx) const
    {
        return formatter<T>::format(value.value, ctx);
    }
};

template <class T, class Tag>
struct std::hash<immer::persist::detail::type_alias<T, Tag>>
{
    auto operator()(const immer::persist::detail::type_alias<T, Tag>& x) const
    {
        return hash<T>{}(x.value);
    }
};
