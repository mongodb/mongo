#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/**
 * To keep ABI compatability, we use CRT's own string view implementation even for C++ 17.
 */

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <stddef.h>
#include <string>
#include <type_traits>

#if __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
#    include <string_view>
#endif

namespace Aws
{
    namespace Crt
    {
        /**
         * Custom string view implementation in order to meet C++11 baseline
         * @tparam CharT
         * @tparam Traits
         */
        template <typename CharT, typename Traits = std::char_traits<CharT>> class basic_string_view
        {
          public:
            // types
            using traits_type = Traits;
            using value_type = CharT;
            using pointer = value_type *;
            using const_pointer = const value_type *;
            using reference = value_type &;
            using const_reference = const value_type &;
            using const_iterator = const value_type *;
            using iterator = const_iterator;
            using const_reverse_iterator = std::reverse_iterator<const_iterator>;
            using reverse_iterator = const_reverse_iterator;
            using size_type = size_t;
            using difference_type = ptrdiff_t;
            static constexpr size_type npos = static_cast<size_type>(-1);

            // constructors and assignment

            constexpr basic_string_view() noexcept : m_size{0}, m_data{nullptr} {}

            constexpr basic_string_view(const basic_string_view &) noexcept = default;

            constexpr basic_string_view(const CharT *s) noexcept : m_size{traits_type::length(s)}, m_data{s} {}

            constexpr basic_string_view(const CharT *s, size_type count) noexcept : m_size{count}, m_data{s} {}

            basic_string_view &operator=(const basic_string_view &) noexcept = default;

#if __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
            constexpr basic_string_view(const std::basic_string_view<CharT, Traits> &other) noexcept
                : m_size(other.size()), m_data(other.data())
            {
            }

            basic_string_view &operator=(const std::basic_string_view<CharT, Traits> &other) noexcept
            {
                m_data = other->data();
                m_size = other->size();
                return *this;
            }
#endif
            // iterators

            constexpr const_iterator begin() const noexcept { return this->m_data; }

            constexpr const_iterator end() const noexcept { return this->m_data + this->m_size; }

            constexpr const_iterator cbegin() const noexcept { return this->m_data; }

            constexpr const_iterator cend() const noexcept { return this->m_data + this->m_size; }

            constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(this->end()); }

            constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(this->begin()); }

            constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(this->end()); }

            constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(this->begin()); }

            constexpr size_type size() const noexcept { return this->m_size; }

            constexpr size_type length() const noexcept { return this->m_size; }

            constexpr size_type max_size() const noexcept { return (std::numeric_limits<size_type>::max)(); }

            constexpr bool empty() const noexcept { return this->m_size == 0; }

            // element accessors

            const_reference operator[](size_type pos) const noexcept
            {
                assert(pos < m_size);
                return *(this->m_data + pos);
            }

            const_reference at(size_type pos) const
            {
                assert(pos < m_size);
                return *(this->m_data + pos);
            }

            const_reference front() const noexcept
            {
                assert(m_size > 0);
                return *this->m_data;
            }

            const_reference back() const noexcept
            {
                assert(m_size > 0);
                return *(this->m_data + this->m_size - 1);
            }

            constexpr const_pointer data() const noexcept { return this->m_data; }

            // modifiers
            void remove_prefix(size_type n) noexcept
            {
                assert(this->m_size >= n);
                this->m_data += n;
                this->m_size -= n;
            }

            void remove_suffix(size_type n) noexcept { this->m_size -= n; }

            void swap(basic_string_view &other) noexcept
            {
                auto tmp = *this;
                *this = other;
                other = tmp;
            }

            // string operations
            size_type copy(CharT *s, size_type n, size_type pos = 0) const
            {
                assert(pos <= size());
                const size_type copyLen = (std::min)(n, m_size - pos);
                traits_type::copy(s, data() + pos, copyLen);
                return copyLen;
            }

            basic_string_view substr(size_type pos = 0, size_type n = npos) const noexcept(false)
            {
                assert(pos <= size());
                const size_type copyLen = (std::min)(n, m_size - pos);
                return basic_string_view{m_data + pos, copyLen};
            }

            int compare(const basic_string_view &s) const noexcept
            {
                const size_type compareLen = (std::min)(this->m_size, s.m_size);
                int ret = traits_type::compare(this->m_data, s.m_data, compareLen);
                if (ret == 0)
                {
                    ret = _s_compare(this->m_size, s.m_size);
                }
                return ret;
            }

            constexpr int compare(size_type pos1, size_type n1, const basic_string_view &s) const
            {
                return this->substr(pos1, n1).compare(s);
            }

            constexpr int compare(
                size_type pos1,
                size_type n1,
                const basic_string_view &s,
                size_type pos2,
                size_type n2) const
            {
                return this->substr(pos1, n1).compare(s.substr(pos2, n2));
            }

            constexpr int compare(const CharT *s) const noexcept { return this->compare(basic_string_view{s}); }

            constexpr int compare(size_type pos1, size_type n1, const CharT *s) const
            {
                return this->substr(pos1, n1).compare(basic_string_view{s});
            }

            constexpr int compare(size_type pos1, size_type n1, const CharT *s, size_type n2) const noexcept(false)
            {
                return this->substr(pos1, n1).compare(basic_string_view(s, n2));
            }

            constexpr bool starts_with(const basic_string_view &other) const noexcept
            {
                return this->substr(0, other.size()) == other;
            }

            constexpr bool starts_with(CharT c) const noexcept
            {
                return !this->empty() && traits_type::eq(this->front(), c);
            }

            constexpr bool starts_with(const CharT *s) const noexcept
            {
                return this->starts_with(basic_string_view(s));
            }

            constexpr bool ends_with(const basic_string_view &other) const noexcept
            {
                return this->m_size >= other.m_size && this->compare(this->m_size - other.m_size, npos, other) == 0;
            }

            constexpr bool ends_with(CharT c) const noexcept
            {
                return !this->empty() && traits_type::eq(this->back(), c);
            }

            constexpr bool ends_with(const CharT *s) const noexcept { return this->ends_with(basic_string_view(s)); }

            // find utilities
            constexpr size_type find(const basic_string_view &s, size_type pos = 0) const noexcept
            {
                return this->find(s.m_data, pos, s.m_size);
            }

            size_type find(CharT c, size_type pos = 0) const noexcept
            {
                if (pos >= m_size)
                {
                    return npos;
                }
                const CharT *r = Traits::find(m_data + pos, m_size - pos, c);
                if (r == nullptr)
                {
                    return npos;
                }
                return static_cast<size_type>(r - m_data);
            }

            size_type find(const CharT *s, size_type pos, size_type n) const noexcept
            {
                if (n && !s)
                {
                    return npos;
                }

                if (pos > m_size)
                {
                    return npos;
                }

                if (n == 0)
                {
                    return pos;
                }

                const CharT *r = _s_search_substr(m_data + pos, m_data + m_size, s, s + n);

                if (r == m_data + m_size)
                {
                    return npos;
                }
                return static_cast<size_type>(r - m_data);
            }

            constexpr size_type find(const CharT *s, size_type pos = 0) const noexcept
            {
                return this->find(s, pos, traits_type::length(s));
            }

            size_type rfind(basic_string_view s, size_type pos = npos) const noexcept
            {
                if (s.m_size && !s.m_data)
                {
                    return npos;
                }
                return this->rfind(s.m_data, pos, s.m_size);
            }

            size_type rfind(CharT c, size_type pos = npos) const noexcept
            {
                if (m_size <= 0)
                {
                    return npos;
                }

                if (pos < m_size)
                {
                    ++pos;
                }
                else
                {
                    pos = m_size;
                }

                for (const CharT *ptr = m_data + pos; ptr != m_data;)
                {
                    if (Traits::eq(*--ptr, c))
                    {
                        return static_cast<size_type>(ptr - m_data);
                    }
                }
                return npos;
            }

            size_type rfind(const CharT *s, size_type pos, size_type n) const noexcept
            {
                if (n && !s)
                {
                    return npos;
                }

                pos = (std::min)(pos, m_size);
                if (n < m_size - pos)
                {
                    pos += n;
                }
                else
                {
                    pos = m_size;
                }
                const CharT *r = _s_find_end(m_data, m_data + pos, s, s + n);
                if (n > 0 && r == m_data + pos)
                {
                    return npos;
                }
                return static_cast<size_type>(r - m_data);
            }

            constexpr size_type rfind(const CharT *s, size_type pos = npos) const noexcept
            {
                return this->rfind(s, pos, traits_type::length(s));
            }

            constexpr size_type find_first_of(basic_string_view s, size_type pos = 0) const noexcept
            {
                return this->find_first_of(s.m_data, pos, s.m_size);
            }

            constexpr size_type find_first_of(CharT c, size_type pos = 0) const noexcept { return this->find(c, pos); }

            size_type find_first_of(const CharT *s, size_type pos, size_type n) const noexcept
            {
                if (pos >= m_size || !n || !s)
                {
                    return npos;
                }

                const CharT *r = _s_find_first_of_ce(m_data + pos, m_data + m_size, s, s + n);

                if (r == m_data + m_size)
                {
                    return npos;
                }

                return static_cast<size_type>(r - m_data);
            }

            constexpr size_type find_first_of(const CharT *s, size_type pos = 0) const noexcept
            {
                return this->find_first_of(s, pos, traits_type::length(s));
            }

            constexpr size_type find_last_of(basic_string_view s, size_type pos = npos) const noexcept
            {
                return this->find_last_of(s.m_data, pos, s.m_size);
            }

            constexpr size_type find_last_of(CharT c, size_type pos = npos) const noexcept
            {
                return this->rfind(c, pos);
            }

            size_type find_last_of(const CharT *s, size_type pos, size_type n) const noexcept
            {
                if (!n || s == nullptr)
                {
                    return npos;
                }

                if (pos < m_size)
                {
                    ++pos;
                }
                else
                {
                    pos = m_size;
                }

                for (const CharT *ptr = m_data + pos; ptr != m_data;)
                {
                    const CharT *r = Traits::find(s, n, *--ptr);
                    if (r)
                    {
                        return static_cast<size_type>(ptr - m_data);
                    }
                }

                return npos;
            }

            constexpr size_type find_last_of(const CharT *s, size_type pos = npos) const noexcept
            {
                return this->find_last_of(s, pos, traits_type::length(s));
            }

            size_type find_first_not_of(basic_string_view s, size_type pos = 0) const noexcept
            {
                if (s.m_size && !s.m_data)
                {
                    return npos;
                }
                return this->find_first_not_of(s.m_data, pos, s.m_size);
            }

            size_type find_first_not_of(CharT c, size_type pos = 0) const noexcept
            {
                if (!m_data || pos >= m_size)
                {
                    return npos;
                }

                const CharT *pend = m_data + m_size;
                for (const CharT *ptr = m_data + pos; ptr != pend; ++ptr)
                {
                    if (!Traits::eq(*ptr, c))
                    {
                        return static_cast<size_type>(ptr - m_data);
                    }
                }

                return npos;
            }

            size_type find_first_not_of(const CharT *s, size_type pos, size_type n) const noexcept
            {
                if (n && s == nullptr)
                {
                    return npos;
                }

                if (m_data == nullptr || pos >= m_size)
                {
                    return npos;
                }

                const CharT *pend = m_data + m_size;
                for (const CharT *ptr = m_data + pos; ptr != pend; ++ptr)
                {
                    if (Traits::find(s, n, *ptr) == 0)
                    {
                        return static_cast<size_type>(ptr - m_data);
                    }
                }

                return npos;
            }

            constexpr size_type find_first_not_of(const CharT *s, size_type pos = 0) const noexcept
            {
                return this->find_first_not_of(s, pos, traits_type::length(s));
            }

            size_type find_last_not_of(basic_string_view s, size_type pos = npos) const noexcept
            {
                if (s.m_size && !s.m_data)
                {
                    return npos;
                }
                return this->find_last_not_of(s.m_data, pos, s.m_size);
            }

            size_type find_last_not_of(CharT c, size_type pos = npos) const noexcept
            {
                if (pos < m_size)
                {
                    ++pos;
                }
                else
                {
                    pos = m_size;
                }

                for (const CharT *ptr = m_data + pos; ptr != m_data;)
                {
                    if (!Traits::eq(*--ptr, c))
                    {
                        return static_cast<size_type>(ptr - m_data);
                    }
                }
                return npos;
            }

            size_type find_last_not_of(const CharT *s, size_type pos, size_type n) const noexcept
            {
                if (n && !s)
                {
                    return npos;
                }

                if (pos < m_size)
                {
                    ++pos;
                }
                else
                {
                    pos = m_size;
                }

                for (const CharT *ptr = m_data + pos; ptr != m_data;)
                {
                    if (Traits::find(s, n, *--ptr) == 0)
                    {
                        return static_cast<size_type>(ptr - m_data);
                    }
                }
                return npos;
            }

            constexpr size_type find_last_not_of(const CharT *s, size_type pos = npos) const noexcept
            {
                return this->find_last_not_of(s, pos, traits_type::length(s));
            }

          private:
            static int _s_compare(size_type n1, size_type n2) noexcept
            {
                const difference_type diff = n1 - n2;

                if (diff > (std::numeric_limits<int>::max)())
                {
                    return (std::numeric_limits<int>::max)();
                }

                if (diff < (std::numeric_limits<int>::min)())
                {
                    return (std::numeric_limits<int>::min)();
                }

                return static_cast<int>(diff);
            }

            static const CharT *_s_search_substr(
                const CharT *first1,
                const CharT *last1,
                const CharT *first2,
                const CharT *last2)
            {
                const ptrdiff_t length2 = last2 - first2;
                if (length2 == 0)
                {
                    return first1;
                }

                ptrdiff_t length1 = last1 - first1;
                if (length1 < length2)
                {
                    return last1;
                }

                while (true)
                {
                    length1 = last1 - first1;
                    if (length1 < length2)
                    {
                        return last1;
                    }

                    first1 = Traits::find(first1, length1 - length2 + 1, *first2);
                    if (first1 == 0)
                    {
                        return last1;
                    }

                    if (Traits::compare(first1, first2, length2) == 0)
                    {
                        return first1;
                    }

                    ++first1;
                }
            }

            static const CharT *_s_find_end(
                const CharT *first1,
                const CharT *last1,
                const CharT *first2,
                const CharT *last2)
            {
                const CharT *r = last1;
                if (first2 == last2)
                {
                    return r;
                }

                while (true)
                {
                    while (true)
                    {
                        if (first1 == last1)
                        {
                            return r;
                        }
                        if (Traits::eq(*first1, *first2))
                        {
                            break;
                        }
                        ++first1;
                    }

                    const CharT *m1 = first1;
                    const CharT *m2 = first2;
                    while (true)
                    {
                        if (++m2 == last2)
                        {
                            r = first1;
                            ++first1;
                            break;
                        }
                        if (++m1 == last1)
                        {
                            return r;
                        }
                        if (!Traits::eq(*m1, *m2))
                        {
                            ++first1;
                            break;
                        }
                    }
                }
            }

            static const CharT *_s_find_first_of_ce(
                const CharT *first1,
                const CharT *last1,
                const CharT *first2,
                const CharT *last2)
            {
                for (; first1 != last1; ++first1)
                {
                    for (const CharT *ptr = first2; ptr != last2; ++ptr)
                    {
                        if (Traits::eq(*first1, *ptr))
                        {
                            return first1;
                        }
                    }
                }
                return last1;
            }

            size_type m_size;
            const CharT *m_data;
        };

        // operator ==
        template <class CharT, class Traits>
        bool operator==(
            const basic_string_view<CharT, Traits> &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return (lhs.size() != rhs.size()) ? false : lhs.compare(rhs) == 0;
        }

        template <class CharT, class Traits>
        bool operator==(
            const basic_string_view<CharT, Traits> &lhs,
            typename std::common_type<basic_string_view<CharT, Traits>>::type &rhs) noexcept
        {
            return (lhs.size() != rhs.size()) ? false : lhs.compare(rhs) == 0;
        }

        template <class CharT, class Traits>
        bool operator==(
            typename std::common_type<basic_string_view<CharT, Traits>>::type &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return (lhs.size() != rhs.size()) ? false : lhs.compare(rhs) == 0;
        }

        // operator !=
        template <class CharT, class Traits>
        bool operator!=(
            const basic_string_view<CharT, Traits> &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return (lhs.size() != rhs.size()) ? true : lhs.compare(rhs) != 0;
        }

        template <class CharT, class Traits>
        bool operator!=(
            const basic_string_view<CharT, Traits> &lhs,
            typename std::common_type<basic_string_view<CharT, Traits>>::type &rhs) noexcept
        {
            return (lhs.size() != rhs.size()) ? true : lhs.compare(rhs) != 0;
        }

        template <class CharT, class Traits>
        bool operator!=(
            typename std::common_type<basic_string_view<CharT, Traits>>::type &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return (lhs.size() != rhs.size()) ? true : lhs.compare(rhs) != 0;
        }

        // operator <
        template <class CharT, class Traits>
        bool operator<(
            const basic_string_view<CharT, Traits> &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return lhs.compare(rhs) < 0;
        }

        template <class CharT, class Traits>
        constexpr bool operator<(
            const basic_string_view<CharT, Traits> &lhs,
            typename std::common_type<basic_string_view<CharT, Traits>>::type &rhs) noexcept
        {
            return lhs.compare(rhs) < 0;
        }

        template <class CharT, class Traits>
        constexpr bool operator<(
            typename std::common_type<basic_string_view<CharT, Traits>>::type &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return lhs.compare(rhs) < 0;
        }

        // operator >
        template <class CharT, class Traits>
        constexpr bool operator>(
            const basic_string_view<CharT, Traits> &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return lhs.compare(rhs) > 0;
        }

        template <class CharT, class Traits>
        constexpr bool operator>(
            const basic_string_view<CharT, Traits> &lhs,
            typename std::common_type<basic_string_view<CharT, Traits>>::type &rhs) noexcept
        {
            return lhs.compare(rhs) > 0;
        }

        template <class CharT, class Traits>
        constexpr bool operator>(
            typename std::common_type<basic_string_view<CharT, Traits>>::type &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return lhs.compare(rhs) > 0;
        }

        // operator <=
        template <class CharT, class Traits>
        constexpr bool operator<=(
            const basic_string_view<CharT, Traits> &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return lhs.compare(rhs) <= 0;
        }

        template <class CharT, class Traits>
        constexpr bool operator<=(
            const basic_string_view<CharT, Traits> &lhs,
            typename std::common_type<basic_string_view<CharT, Traits>>::type &rhs) noexcept
        {
            return lhs.compare(rhs) <= 0;
        }

        template <class CharT, class Traits>
        constexpr bool operator<=(
            typename std::common_type<basic_string_view<CharT, Traits>>::type &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return lhs.compare(rhs) <= 0;
        }

        // operator >=
        template <class CharT, class Traits>
        constexpr bool operator>=(
            const basic_string_view<CharT, Traits> &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return lhs.compare(rhs) >= 0;
        }

        template <class CharT, class Traits>
        constexpr bool operator>=(
            const basic_string_view<CharT, Traits> &lhs,
            typename std::common_type<basic_string_view<CharT, Traits>>::type &rhs) noexcept
        {
            return lhs.compare(rhs) >= 0;
        }

        template <class CharT, class Traits>
        constexpr bool operator>=(
            typename std::common_type<basic_string_view<CharT, Traits>>::type &lhs,
            const basic_string_view<CharT, Traits> &rhs) noexcept
        {
            return lhs.compare(rhs) >= 0;
        }

        typedef basic_string_view<char> string_view;
        typedef basic_string_view<char16_t> u16string_view;
        typedef basic_string_view<char32_t> u32string_view;
        typedef basic_string_view<wchar_t> wstring_view;

        inline namespace literals
        {
            inline namespace string_view_literals
            {
                inline basic_string_view<char> operator"" _sv(const char *s, size_t length) noexcept
                {
                    return basic_string_view<char>(s, length);
                }

                inline basic_string_view<wchar_t> operator"" _sv(const wchar_t * s, size_t length) noexcept
                {
                    return basic_string_view<wchar_t>(s, length);
                }

                inline basic_string_view<char16_t> operator"" _sv(const char16_t *s, size_t length) noexcept
                {
                    return basic_string_view<char16_t>(s, length);
                }

                inline basic_string_view<char32_t> operator"" _sv(const char32_t *s, size_t length) noexcept
                {
                    return basic_string_view<char32_t>(s, length);
                }
            } // namespace string_view_literals

        } // namespace literals

        using StringView = string_view;
    } // namespace Crt
} // namespace Aws

// hash
namespace std
{
    template <class CharT, class Traits> struct hash<Aws::Crt::basic_string_view<CharT, Traits>>
    {
        size_t operator()(const Aws::Crt::basic_string_view<CharT, Traits> &val) const noexcept;
    };

    template <class CharT, class Traits>
    size_t hash<Aws::Crt::basic_string_view<CharT, Traits>>::operator()(
        const Aws::Crt::basic_string_view<CharT, Traits> &val) const noexcept
    {
        auto str = std::basic_string<CharT, Traits>(val.data(), val.size());
        return std::hash<std::basic_string<CharT, Traits>>{}(str);
    }
} // namespace std
