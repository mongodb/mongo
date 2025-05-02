//  filesystem path.cpp  -------------------------------------------------------------  //

//  Copyright Beman Dawes 2008
//  Copyright Andrey Semashev 2021-2024

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#include "platform_config.hpp"

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/detail/path_traits.hpp> // codecvt_error_category()
#include <boost/system/error_category.hpp> // for BOOST_SYSTEM_HAS_CONSTEXPR
#include <boost/assert.hpp>
#include <algorithm>
#include <iterator>
#include <utility>
#include <string>
#include <cstddef>
#include <cstring>
#include <cstdlib> // std::atexit

#ifdef BOOST_WINDOWS_API
#include "windows_file_codecvt.hpp"
#include "windows_tools.hpp"
#include <windows.h>
#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__HAIKU__)
#include <boost/filesystem/detail/utf8_codecvt_facet.hpp>
#endif

#ifdef BOOST_FILESYSTEM_DEBUG
#include <iostream>
#include <iomanip>
#endif

#include "atomic_tools.hpp"
#include "private_config.hpp"

#include <boost/filesystem/detail/header.hpp> // must be the last #include

namespace fs = boost::filesystem;

using boost::filesystem::path;

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                class path helpers                                    //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace {
//------------------------------------------------------------------------------------//
//                        miscellaneous class path helpers                            //
//------------------------------------------------------------------------------------//

typedef path::value_type value_type;
typedef path::string_type string_type;
typedef string_type::size_type size_type;

#ifdef BOOST_WINDOWS_API

const wchar_t dot_path_literal[] = L".";
const wchar_t dot_dot_path_literal[] = L"..";
const wchar_t separators[] = L"/\\";
using boost::filesystem::detail::colon;
using boost::filesystem::detail::questionmark;

inline bool is_alnum(wchar_t c)
{
    return boost::filesystem::detail::is_letter(c) || (c >= L'0' && c <= L'9');
}

inline bool is_device_name_char(wchar_t c)
{
    // https://googleprojectzero.blogspot.com/2016/02/the-definitive-guide-on-win32-to-nt.html
    // Device names are:
    //
    // - PRN
    // - AUX
    // - NUL
    // - CON
    // - LPT[1-9]
    // - COM[1-9]
    // - CONIN$
    // - CONOUT$
    return is_alnum(c) || c == L'$';
}

//! Returns position of the first directory separator in the \a size initial characters of \a p, or \a size if not found
inline size_type find_separator(const wchar_t* p, size_type size) noexcept
{
    size_type pos = 0u;
    for (; pos < size; ++pos)
    {
        const wchar_t c = p[pos];
        if (boost::filesystem::detail::is_directory_separator(c))
            break;
    }
    return pos;
}

#else // BOOST_WINDOWS_API

const char dot_path_literal[] = ".";
const char dot_dot_path_literal[] = "..";
const char separators[] = "/";

//! Returns position of the first directory separator in the \a size initial characters of \a p, or \a size if not found
inline size_type find_separator(const char* p, size_type size) noexcept
{
    const char* sep = static_cast< const char* >(std::memchr(p, '/', size));
    size_type pos = size;
    if (BOOST_LIKELY(!!sep))
        pos = sep - p;
    return pos;
}

#endif // BOOST_WINDOWS_API

// pos is position of the separator
bool is_root_separator(string_type const& str, size_type root_dir_pos, size_type pos);

// Returns: Size of the filename element that ends at end_pos (which is past-the-end position). 0 if no filename found.
size_type find_filename_size(string_type const& str, size_type root_name_size, size_type end_pos);

// Returns: starting position of root directory or size if not found. Sets root_name_size to length
// of the root name if the characters before the returned position (if any) are considered a root name.
size_type find_root_directory_start(const value_type* path, size_type size, size_type& root_name_size);

// Finds position and size of the first element of the path
void first_element(string_type const& src, size_type& element_pos, size_type& element_size, size_type size);

// Finds position and size of the first element of the path
inline void first_element(string_type const& src, size_type& element_pos, size_type& element_size)
{
    first_element(src, element_pos, element_size, src.size());
}

} // unnamed namespace

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            class path implementation                                 //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace boost {
namespace filesystem {
namespace detail {

// C++14 provides a mismatch algorithm with four iterator arguments(), but earlier
// standard libraries didn't, so provide this needed functionality.
inline std::pair< path::iterator, path::iterator > mismatch(path::iterator it1, path::iterator it1end, path::iterator it2, path::iterator it2end)
{
    for (; it1 != it1end && it2 != it2end && path_algorithms::compare_v4(*it1, *it2) == 0;)
    {
        path_algorithms::increment_v4(it1);
        path_algorithms::increment_v4(it2);
    }
    return std::make_pair(it1, it2);
}

//  normal  --------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL path path_algorithms::lexically_normal_v3(path const& p)
{
    const value_type* const pathname = p.m_pathname.c_str();
    const size_type pathname_size = p.m_pathname.size();
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(pathname, pathname_size, root_name_size);
    path normal(pathname, pathname + root_name_size);

#if defined(BOOST_WINDOWS_API)
    for (size_type i = 0; i < root_name_size; ++i)
    {
        if (normal.m_pathname[i] == path::separator)
            normal.m_pathname[i] = path::preferred_separator;
    }
#endif

    size_type root_path_size = root_name_size;
    if (root_dir_pos < pathname_size)
    {
        root_path_size = root_dir_pos + 1;
        normal.m_pathname.push_back(path::preferred_separator);
    }

    size_type i = root_path_size;

    // Skip redundant directory separators after the root directory
    while (i < pathname_size && detail::is_directory_separator(pathname[i]))
        ++i;

    if (i < pathname_size)
    {
        bool last_element_was_dot = false;
        while (true)
        {
            {
                const size_type start_pos = i;

                // Find next separator
                i += find_separator(pathname + i, pathname_size - i);

                const size_type size = i - start_pos;

                // Skip dot elements
                if (size == 1u && pathname[start_pos] == path::dot)
                {
                    last_element_was_dot = true;
                    goto skip_append;
                }

                last_element_was_dot = false;

                // Process dot dot elements
                if (size == 2u && pathname[start_pos] == path::dot && pathname[start_pos + 1] == path::dot && normal.m_pathname.size() > root_path_size)
                {
                    // Don't remove previous dot dot elements
                    const size_type normal_size = normal.m_pathname.size();
                    size_type filename_size = find_filename_size(normal.m_pathname, root_path_size, normal_size);
                    size_type pos = normal_size - filename_size;
                    if (filename_size != 2u || normal.m_pathname[pos] != path::dot || normal.m_pathname[pos + 1] != path::dot)
                    {
                        if (pos > root_path_size && detail::is_directory_separator(normal.m_pathname[pos - 1]))
                            --pos;
                        normal.m_pathname.erase(normal.m_pathname.begin() + pos , normal.m_pathname.end());
                        goto skip_append;
                    }
                }

                // Append the element
                path_algorithms::append_separator_if_needed(normal);
                normal.m_pathname.append(pathname + start_pos, size);
            }

        skip_append:
            if (i == pathname_size)
                break;

            // Skip directory separators, including duplicates
            while (i < pathname_size && detail::is_directory_separator(pathname[i]))
                ++i;

            if (i == pathname_size)
            {
                // If a path ends with a separator, add a trailing dot element
                goto append_trailing_dot;
            }
        }

        if (normal.empty() || last_element_was_dot)
        {
        append_trailing_dot:
            path_algorithms::append_separator_if_needed(normal);
            normal.m_pathname.push_back(path::dot);
        }
    }

    return normal;
}

BOOST_FILESYSTEM_DECL path path_algorithms::lexically_normal_v4(path const& p)
{
    const value_type* const pathname = p.m_pathname.c_str();
    const size_type pathname_size = p.m_pathname.size();
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(pathname, pathname_size, root_name_size);
    path normal(pathname, pathname + root_name_size);

    size_type root_path_size = root_name_size;
    if (root_dir_pos < pathname_size)
    {
        root_path_size = root_dir_pos + 1;
        normal.m_pathname.push_back(path::preferred_separator);
    }

    size_type i = root_path_size;

    // Skip redundant directory separators after the root directory
    while (i < pathname_size && detail::is_directory_separator(pathname[i]))
        ++i;

    if (i < pathname_size)
    {
        while (true)
        {
            bool last_element_was_dot = false;
            {
                const size_type start_pos = i;

                // Find next separator
                i += find_separator(pathname + i, pathname_size - i);

                const size_type size = i - start_pos;

                // Skip dot elements
                if (size == 1u && pathname[start_pos] == path::dot)
                {
                    last_element_was_dot = true;
                    goto skip_append;
                }

                // Process dot dot elements
                if (size == 2u && pathname[start_pos] == path::dot && pathname[start_pos + 1] == path::dot && normal.m_pathname.size() > root_path_size)
                {
                    // Don't remove previous dot dot elements
                    const size_type normal_size = normal.m_pathname.size();
                    size_type filename_size = find_filename_size(normal.m_pathname, root_path_size, normal_size);
                    size_type pos = normal_size - filename_size;
                    if (filename_size != 2u || normal.m_pathname[pos] != path::dot || normal.m_pathname[pos + 1] != path::dot)
                    {
                        if (pos > root_path_size && detail::is_directory_separator(normal.m_pathname[pos - 1]))
                            --pos;
                        normal.m_pathname.erase(normal.m_pathname.begin() + pos, normal.m_pathname.end());
                        goto skip_append;
                    }
                }

                // Append the element
                path_algorithms::append_separator_if_needed(normal);
                normal.m_pathname.append(pathname + start_pos, size);
            }

        skip_append:
            if (i == pathname_size)
            {
                // If a path ends with a trailing dot after a directory element, add a trailing separator
                if (last_element_was_dot && !normal.empty() && !normal.filename_is_dot_dot())
                    path_algorithms::append_separator_if_needed(normal);

                break;
            }

            // Skip directory separators, including duplicates
            while (i < pathname_size && detail::is_directory_separator(pathname[i]))
                ++i;

            if (i == pathname_size)
            {
                // If a path ends with a separator, add a trailing separator
                if (!normal.empty() && !normal.filename_is_dot_dot())
                    path_algorithms::append_separator_if_needed(normal);
                break;
            }
        }

        // If the original path was not empty and normalized ended up being empty, make it a dot
        if (normal.empty())
            normal.m_pathname.push_back(path::dot);
    }

    return normal;
}

//  generic_path ---------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL path path_algorithms::generic_path_v3(path const& p)
{
    path tmp;
    const size_type pathname_size = p.m_pathname.size();
    tmp.m_pathname.reserve(pathname_size);

    const value_type* const pathname = p.m_pathname.c_str();

    // Don't remove duplicate slashes from the root name
    size_type root_name_size = 0u;
    size_type root_dir_pos = find_root_directory_start(pathname, pathname_size, root_name_size);
    if (root_name_size > 0u)
    {
        tmp.m_pathname.append(pathname, root_name_size);
#if defined(BOOST_WINDOWS_API)
        std::replace(tmp.m_pathname.begin(), tmp.m_pathname.end(), L'\\', L'/');
#endif
    }

    size_type pos = root_name_size;
    if (root_dir_pos < pathname_size)
    {
        tmp.m_pathname.push_back(path::separator);
        pos = root_dir_pos + 1u;
    }

    while (pos < pathname_size)
    {
        size_type element_size = find_separator(pathname + pos, pathname_size - pos);
        if (element_size > 0u)
        {
            tmp.m_pathname.append(pathname + pos, element_size);

            pos += element_size;
            if (pos >= pathname_size)
                break;

            tmp.m_pathname.push_back(path::separator);
        }

        ++pos;
    }

    return tmp;
}

BOOST_FILESYSTEM_DECL path path_algorithms::generic_path_v4(path const& p)
{
    path tmp;
    const size_type pathname_size = p.m_pathname.size();
    tmp.m_pathname.reserve(pathname_size);

    const value_type* const pathname = p.m_pathname.c_str();

    // Treat root name specially as it may contain backslashes, duplicate ones too,
    // in case of UNC paths and Windows-specific prefixes.
    size_type root_name_size = 0u;
    size_type root_dir_pos = find_root_directory_start(pathname, pathname_size, root_name_size);
    if (root_name_size > 0u)
        tmp.m_pathname.append(pathname, root_name_size);

    size_type pos = root_name_size;
    if (root_dir_pos < pathname_size)
    {
        tmp.m_pathname.push_back(path::separator);
        pos = root_dir_pos + 1u;
    }

    while (pos < pathname_size)
    {
        size_type element_size = find_separator(pathname + pos, pathname_size - pos);
        if (element_size > 0u)
        {
            tmp.m_pathname.append(pathname + pos, element_size);

            pos += element_size;
            if (pos >= pathname_size)
                break;

            tmp.m_pathname.push_back(path::separator);
        }

        ++pos;
    }

    return tmp;
}

#if defined(BOOST_WINDOWS_API)

//  make_preferred -------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL void path_algorithms::make_preferred_v3(path& p)
{
    std::replace(p.m_pathname.begin(), p.m_pathname.end(), L'/', L'\\');
}

BOOST_FILESYSTEM_DECL void path_algorithms::make_preferred_v4(path& p)
{
    const size_type pathname_size = p.m_pathname.size();
    if (pathname_size > 0u)
    {
        value_type* const pathname = &p.m_pathname[0];

        // Avoid converting slashes in the root name
        size_type root_name_size = 0u;
        find_root_directory_start(pathname, pathname_size, root_name_size);

        std::replace(pathname + root_name_size, pathname + pathname_size, L'/', L'\\');
    }
}

#endif // defined(BOOST_WINDOWS_API)

//  append  --------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL void path_algorithms::append_v3(path& p, const value_type* begin, const value_type* end)
{
    if (begin != end)
    {
        if (BOOST_LIKELY(begin < p.m_pathname.data() || begin >= (p.m_pathname.data() + p.m_pathname.size())))
        {
            if (!detail::is_directory_separator(*begin))
                path_algorithms::append_separator_if_needed(p);
            p.m_pathname.append(begin, end);
        }
        else
        {
            // overlapping source
            string_type rhs(begin, end);
            path_algorithms::append_v3(p, rhs.data(), rhs.data() + rhs.size());
        }
    }
}

BOOST_FILESYSTEM_DECL void path_algorithms::append_v4(path& p, const value_type* begin, const value_type* end)
{
    if (begin != end)
    {
        if (BOOST_LIKELY(begin < p.m_pathname.data() || begin >= (p.m_pathname.data() + p.m_pathname.size())))
        {
            const size_type that_size = end - begin;
            size_type that_root_name_size = 0;
            size_type that_root_dir_pos = find_root_directory_start(begin, that_size, that_root_name_size);

            // if (p.is_absolute())
            if
            (
#if defined(BOOST_WINDOWS_API)
                that_root_name_size > 0 &&
#endif
                that_root_dir_pos < that_size
            )
            {
            return_assign:
                p.assign(begin, end);
                return;
            }

            size_type this_root_name_size = 0;
            find_root_directory_start(p.m_pathname.c_str(), p.m_pathname.size(), this_root_name_size);

            if
            (
                that_root_name_size > 0 &&
                (that_root_name_size != this_root_name_size || std::memcmp(p.m_pathname.c_str(), begin, this_root_name_size * sizeof(value_type)) != 0)
            )
            {
                goto return_assign;
            }

            if (that_root_dir_pos < that_size)
            {
                // Remove root directory (if any) and relative path to replace with those from p
                p.m_pathname.erase(p.m_pathname.begin() + this_root_name_size, p.m_pathname.end());
            }

            const value_type* const that_path = begin + that_root_name_size;
            if (!detail::is_directory_separator(*that_path))
                path_algorithms::append_separator_if_needed(p);
            p.m_pathname.append(that_path, end);
        }
        else
        {
            // overlapping source
            string_type rhs(begin, end);
            path_algorithms::append_v4(p, rhs.data(), rhs.data() + rhs.size());
        }
    }
    else if (path_algorithms::has_filename_v4(p))
    {
        p.m_pathname.push_back(path::preferred_separator);
    }
}

//  compare  -------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL int path_algorithms::lex_compare_v3
(
    path_detail::path_iterator first1, path_detail::path_iterator const& last1,
    path_detail::path_iterator first2, path_detail::path_iterator const& last2
)
{
    for (; first1 != last1 && first2 != last2;)
    {
        if (first1->native() < first2->native())
            return -1;
        if (first2->native() < first1->native())
            return 1;
        BOOST_ASSERT(first2->native() == first1->native());
        path_algorithms::increment_v3(first1);
        path_algorithms::increment_v3(first2);
    }
    if (first1 == last1 && first2 == last2)
        return 0;
    return first1 == last1 ? -1 : 1;
}

BOOST_FILESYSTEM_DECL int path_algorithms::lex_compare_v4
(
    path_detail::path_iterator first1, path_detail::path_iterator const& last1,
    path_detail::path_iterator first2, path_detail::path_iterator const& last2
)
{
    for (; first1 != last1 && first2 != last2;)
    {
        if (first1->native() < first2->native())
            return -1;
        if (first2->native() < first1->native())
            return 1;
        BOOST_ASSERT(first2->native() == first1->native());
        path_algorithms::increment_v4(first1);
        path_algorithms::increment_v4(first2);
    }
    if (first1 == last1 && first2 == last2)
        return 0;
    return first1 == last1 ? -1 : 1;
}

BOOST_FILESYSTEM_DECL int path_algorithms::compare_v3(path const& left, path const& right)
{
    return path_algorithms::lex_compare_v3(left.begin(), left.end(), right.begin(), right.end());
}

BOOST_FILESYSTEM_DECL int path_algorithms::compare_v4(path const& left, path const& right)
{
    return path_algorithms::lex_compare_v4(left.begin(), left.end(), right.begin(), right.end());
}

//  append_separator_if_needed  ------------------------------------------------------//

BOOST_FILESYSTEM_DECL path_algorithms::string_type::size_type path_algorithms::append_separator_if_needed(path& p)
{
    if (!p.m_pathname.empty() &&
#ifdef BOOST_WINDOWS_API
        *(p.m_pathname.end() - 1) != colon &&
#endif
        !detail::is_directory_separator(*(p.m_pathname.end() - 1)))
    {
        string_type::size_type tmp(p.m_pathname.size());
        p.m_pathname.push_back(path::preferred_separator);
        return tmp;
    }
    return 0;
}

//  erase_redundant_separator  -------------------------------------------------------//

BOOST_FILESYSTEM_DECL void path_algorithms::erase_redundant_separator(path& p, string_type::size_type sep_pos)
{
    if (sep_pos                                          // a separator was added
        && sep_pos < p.m_pathname.size()                 // and something was appended
        && (p.m_pathname[sep_pos + 1] == path::separator // and it was also separator
#ifdef BOOST_WINDOWS_API
            || p.m_pathname[sep_pos + 1] == path::preferred_separator // or preferred_separator
#endif
            ))
    {
        p.m_pathname.erase(p.m_pathname.begin() + sep_pos); // erase the added separator
    }
}

//  modifiers  -----------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL void path_algorithms::remove_filename_v3(path& p)
{
    p.remove_filename_and_trailing_separators();
}

BOOST_FILESYSTEM_DECL void path_algorithms::remove_filename_v4(path& p)
{
    size_type filename_size = path_algorithms::find_filename_v4_size(p);
    p.m_pathname.erase(p.m_pathname.begin() + (p.m_pathname.size() - filename_size), p.m_pathname.end());
}

BOOST_FILESYSTEM_DECL void path_algorithms::replace_extension_v3(path& p, path const& new_extension)
{
    // erase existing extension, including the dot, if any
    size_type ext_pos = p.m_pathname.size() - path_algorithms::extension_v3(p).m_pathname.size();
    p.m_pathname.erase(p.m_pathname.begin() + ext_pos, p.m_pathname.end());

    if (!new_extension.empty())
    {
        // append new_extension, adding the dot if necessary
        if (new_extension.m_pathname[0] != path::dot)
            p.m_pathname.push_back(path::dot);
        p.m_pathname.append(new_extension.m_pathname);
    }
}

BOOST_FILESYSTEM_DECL void path_algorithms::replace_extension_v4(path& p, path const& new_extension)
{
    // erase existing extension, including the dot, if any
    size_type ext_pos = p.m_pathname.size() - path_algorithms::find_extension_v4_size(p);
    p.m_pathname.erase(p.m_pathname.begin() + ext_pos, p.m_pathname.end());

    if (!new_extension.empty())
    {
        // append new_extension, adding the dot if necessary
        if (new_extension.m_pathname[0] != path::dot)
            p.m_pathname.push_back(path::dot);
        p.m_pathname.append(new_extension.m_pathname);
    }
}

//  decomposition  -------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL size_type path_algorithms::find_root_name_size(path const& p)
{
    size_type root_name_size = 0;
    find_root_directory_start(p.m_pathname.c_str(), p.m_pathname.size(), root_name_size);
    return root_name_size;
}

BOOST_FILESYSTEM_DECL size_type path_algorithms::find_root_path_size(path const& p)
{
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(p.m_pathname.c_str(), p.m_pathname.size(), root_name_size);

    size_type size = root_name_size;
    if (root_dir_pos < p.m_pathname.size())
        size = root_dir_pos + 1;

    return size;
}

BOOST_FILESYSTEM_DECL path_algorithms::substring path_algorithms::find_root_directory(path const& p)
{
    substring root_dir;
    size_type root_name_size = 0;
    root_dir.pos = find_root_directory_start(p.m_pathname.c_str(), p.m_pathname.size(), root_name_size);
    root_dir.size = static_cast< std::size_t >(root_dir.pos < p.m_pathname.size());
    return root_dir;
}

BOOST_FILESYSTEM_DECL path_algorithms::substring path_algorithms::find_relative_path(path const& p)
{
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(p.m_pathname.c_str(), p.m_pathname.size(), root_name_size);

    // Skip root name, root directory and any duplicate separators
    size_type size = root_name_size;
    if (root_dir_pos < p.m_pathname.size())
    {
        size = root_dir_pos + 1;

        for (size_type n = p.m_pathname.size(); size < n; ++size)
        {
            if (!detail::is_directory_separator(p.m_pathname[size]))
                break;
        }
    }

    substring rel_path;
    rel_path.pos = size;
    rel_path.size = p.m_pathname.size() - size;

    return rel_path;
}

BOOST_FILESYSTEM_DECL path_algorithms::string_type::size_type path_algorithms::find_parent_path_size(path const& p)
{
    const size_type size = p.m_pathname.size();
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(p.m_pathname.c_str(), size, root_name_size);

    size_type filename_size = find_filename_size(p.m_pathname, root_name_size, size);
    size_type end_pos = size - filename_size;
    while (true)
    {
        if (end_pos <= root_name_size)
        {
            // Keep the root name as the parent path if there was a filename
            if (filename_size == 0)
                end_pos = 0u;
            break;
        }

        --end_pos;

        if (!detail::is_directory_separator(p.m_pathname[end_pos]))
        {
            ++end_pos;
            break;
        }

        if (end_pos == root_dir_pos)
        {
            // Keep the trailing root directory if there was a filename
            end_pos += filename_size > 0;
            break;
        }
    }

    return end_pos;
}

BOOST_FILESYSTEM_DECL path path_algorithms::filename_v3(path const& p)
{
    const size_type size = p.m_pathname.size();
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(p.m_pathname.c_str(), size, root_name_size);
    size_type filename_size, pos;
    if (root_dir_pos < size && detail::is_directory_separator(p.m_pathname[size - 1]) && is_root_separator(p.m_pathname, root_dir_pos, size - 1))
    {
        // Return root directory
        pos = root_dir_pos;
        filename_size = 1u;
    }
    else if (root_name_size == size)
    {
        // Return root name
        pos = 0u;
        filename_size = root_name_size;
    }
    else
    {
        filename_size = find_filename_size(p.m_pathname, root_name_size, size);
        pos = size - filename_size;
        if (filename_size == 0u && pos > root_name_size && detail::is_directory_separator(p.m_pathname[pos - 1]) && !is_root_separator(p.m_pathname, root_dir_pos, pos - 1))
            return detail::dot_path();
    }

    const value_type* ptr = p.m_pathname.c_str() + pos;
    return path(ptr, ptr + filename_size);
}

BOOST_FILESYSTEM_DECL path_algorithms::string_type::size_type path_algorithms::find_filename_v4_size(path const& p)
{
    const size_type size = p.m_pathname.size();
    size_type root_name_size = 0;
    find_root_directory_start(p.m_pathname.c_str(), size, root_name_size);
    return find_filename_size(p.m_pathname, root_name_size, size);
}

BOOST_FILESYSTEM_DECL path path_algorithms::stem_v3(path const& p)
{
    path name(path_algorithms::filename_v3(p));
    if (path_algorithms::compare_v4(name, detail::dot_path()) != 0 && path_algorithms::compare_v4(name, detail::dot_dot_path()) != 0)
    {
        size_type pos = name.m_pathname.rfind(path::dot);
        if (pos != string_type::npos)
            name.m_pathname.erase(name.m_pathname.begin() + pos, name.m_pathname.end());
    }
    return name;
}

BOOST_FILESYSTEM_DECL path path_algorithms::stem_v4(path const& p)
{
    path name(path_algorithms::filename_v4(p));
    if (path_algorithms::compare_v4(name, detail::dot_path()) != 0 && path_algorithms::compare_v4(name, detail::dot_dot_path()) != 0)
    {
        size_type pos = name.m_pathname.rfind(path::dot);
        if (pos != 0 && pos != string_type::npos)
            name.m_pathname.erase(name.m_pathname.begin() + pos, name.m_pathname.end());
    }
    return name;
}

BOOST_FILESYSTEM_DECL path path_algorithms::extension_v3(path const& p)
{
    path name(path_algorithms::filename_v3(p));
    if (path_algorithms::compare_v4(name, detail::dot_path()) == 0 || path_algorithms::compare_v4(name, detail::dot_dot_path()) == 0)
        return path();
    size_type pos(name.m_pathname.rfind(path::dot));
    return pos == string_type::npos ? path() : path(name.m_pathname.c_str() + pos);
}

BOOST_FILESYSTEM_DECL path_algorithms::string_type::size_type path_algorithms::find_extension_v4_size(path const& p)
{
    const size_type size = p.m_pathname.size();
    size_type root_name_size = 0;
    find_root_directory_start(p.m_pathname.c_str(), size, root_name_size);
    size_type filename_size = find_filename_size(p.m_pathname, root_name_size, size);
    size_type filename_pos = size - filename_size;
    if
    (
        filename_size > 0u &&
        // Check for "." and ".." filenames
        !(p.m_pathname[filename_pos] == path::dot &&
            (filename_size == 1u || (filename_size == 2u && p.m_pathname[filename_pos + 1u] == path::dot)))
    )
    {
        size_type ext_pos = size;
        while (ext_pos > filename_pos)
        {
            --ext_pos;
            if (p.m_pathname[ext_pos] == path::dot)
                break;
        }

        if (ext_pos > filename_pos)
            return size - ext_pos;
    }

    return 0u;
}

} // namespace detail

BOOST_FILESYSTEM_DECL path& path::remove_filename_and_trailing_separators()
{
    size_type end_pos = detail::path_algorithms::find_parent_path_size(*this);
    m_pathname.erase(m_pathname.begin() + end_pos, m_pathname.end());
    return *this;
}

BOOST_FILESYSTEM_DECL path& path::remove_trailing_separator()
{
    if (!m_pathname.empty() && detail::is_directory_separator(m_pathname[m_pathname.size() - 1]))
        m_pathname.erase(m_pathname.end() - 1);
    return *this;
}

BOOST_FILESYSTEM_DECL path& path::replace_filename(path const& replacement)
{
    detail::path_algorithms::remove_filename_v4(*this);
    detail::path_algorithms::append_v4(*this, replacement.m_pathname.data(), replacement.m_pathname.data() + replacement.m_pathname.size());
    return *this;
}

//  lexical operations  --------------------------------------------------------------//

BOOST_FILESYSTEM_DECL path path::lexically_relative(path const& base) const
{
    path::iterator b = begin(), e = end(), base_b = base.begin(), base_e = base.end();
    std::pair< path::iterator, path::iterator > mm = detail::mismatch(b, e, base_b, base_e);
    if (mm.first == b && mm.second == base_b)
        return path();
    if (mm.first == e && mm.second == base_e)
        return detail::dot_path();

    std::ptrdiff_t n = 0;
    for (; mm.second != base_e; detail::path_algorithms::increment_v4(mm.second))
    {
        path const& p = *mm.second;
        if (detail::path_algorithms::compare_v4(p, detail::dot_dot_path()) == 0)
            --n;
        else if (!p.empty() && detail::path_algorithms::compare_v4(p, detail::dot_path()) != 0)
            ++n;
    }
    if (n < 0)
        return path();
    if (n == 0 && (mm.first == e || mm.first->empty()))
        return detail::dot_path();

    path tmp;
    for (; n > 0; --n)
        detail::path_algorithms::append_v4(tmp, detail::dot_dot_path());
    for (; mm.first != e; detail::path_algorithms::increment_v4(mm.first))
        detail::path_algorithms::append_v4(tmp, *mm.first);
    return tmp;
}

} // namespace filesystem
} // namespace boost

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                         class path helpers implementation                            //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace {

//  is_root_separator  ---------------------------------------------------------------//

// pos is position of the separator
inline bool is_root_separator(string_type const& str, size_type root_dir_pos, size_type pos)
{
    BOOST_ASSERT_MSG(pos < str.size() && fs::detail::is_directory_separator(str[pos]), "precondition violation");

    // root_dir_pos points at the leftmost separator, we need to skip any duplicate separators right of root dir
    while (pos > root_dir_pos && fs::detail::is_directory_separator(str[pos - 1]))
        --pos;

    return pos == root_dir_pos;
}

//  find_filename_size  --------------------------------------------------------------//

// Returns: Size of the filename element that ends at end_pos (which is past-the-end position). 0 if no filename found.
inline size_type find_filename_size(string_type const& str, size_type root_name_size, size_type end_pos)
{
    size_type pos = end_pos;
    while (pos > root_name_size)
    {
        --pos;

        if (fs::detail::is_directory_separator(str[pos]))
        {
            ++pos; // filename starts past the separator
            break;
        }
    }

    return end_pos - pos;
}

//  find_root_directory_start  -------------------------------------------------------//

// Returns: starting position of root directory or size if not found
size_type find_root_directory_start(const value_type* path, size_type size, size_type& root_name_size)
{
    root_name_size = 0;
    if (size == 0)
        return 0;

    bool parsing_root_name = false;
    size_type pos = 0;

    // case "//", possibly followed by more characters
    if (fs::detail::is_directory_separator(path[0]))
    {
        if (size >= 2 && fs::detail::is_directory_separator(path[1]))
        {
            if (size == 2)
            {
                // The whole path is just a pair of separators
                root_name_size = 2;
                return 2;
            }
#ifdef BOOST_WINDOWS_API
            // https://docs.microsoft.com/en-us/windows/win32/fileio/naming-a-file
            // cases "\\?\" and "\\.\"
            else if (size >= 4 && (path[2] == questionmark || path[2] == fs::path::dot) && fs::detail::is_directory_separator(path[3]))
            {
                parsing_root_name = true;
                pos += 4;
            }
#endif
            else if (fs::detail::is_directory_separator(path[2]))
            {
                // The path starts with three directory separators, which is interpreted as a root directory followed by redundant separators
                return 0;
            }
            else
            {
                // case "//net {/}"
                parsing_root_name = true;
                pos += 2;
                goto find_next_separator;
            }
        }
#ifdef BOOST_WINDOWS_API
        // https://stackoverflow.com/questions/23041983/path-prefixes-and
        // case "\??\" (NT path prefix)
        else if (size >= 4 && path[1] == questionmark && path[2] == questionmark && fs::detail::is_directory_separator(path[3]))
        {
            parsing_root_name = true;
            pos += 4;
        }
#endif
        else
        {
            // The path starts with a separator, possibly followed by a non-separator character
            return 0;
        }
    }

#ifdef BOOST_WINDOWS_API
    // case "c:" or "prn:"
    // Note: There is ambiguity in a "c:x" path interpretation. It could either mean a file "x" located at the current directory for drive C:,
    //       or an alternative stream "x" of a file "c". Windows API resolve this as the former, and so do we.
    if ((size - pos) >= 2 && fs::detail::is_letter(path[pos]))
    {
        size_type i = pos + 1;
        for (; i < size; ++i)
        {
            if (!is_device_name_char(path[i]))
                break;
        }

        if (i < size && path[i] == colon)
        {
            pos = i + 1;
            root_name_size = pos;
            parsing_root_name = false;

            if (pos < size && fs::detail::is_directory_separator(path[pos]))
                return pos;
        }
    }
#endif

    if (!parsing_root_name)
        return size;

find_next_separator:
    pos += find_separator(path + pos, size - pos);
    if (parsing_root_name)
        root_name_size = pos;

    return pos;
}

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                        class path::iterator implementation                           //
//                                                                                      //
//--------------------------------------------------------------------------------------//

//  first_element ----------------------------------------------------------------------//

//   sets pos and len of first element, excluding extra separators
//   if src.empty(), sets pos,len, to 0,0.
void first_element(string_type const& src, size_type& element_pos, size_type& element_size, size_type size)
{
    element_pos = 0;
    element_size = 0;
    if (src.empty())
        return;

    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(src.c_str(), size, root_name_size);

    // First element is the root name, if there is one
    if (root_name_size > 0)
    {
        element_size = root_name_size;
        return;
    }

    // Otherwise, the root directory
    if (root_dir_pos < size)
    {
        element_pos = root_dir_pos;
        element_size = 1u;
        return;
    }

    // Otherwise, the first filename or directory name in a relative path
    size_type end_pos = src.find_first_of(separators);
    if (end_pos == string_type::npos)
        end_pos = src.size();
    element_size = end_pos;
}

} // unnamed namespace

namespace boost {
namespace filesystem {
namespace detail {

BOOST_FILESYSTEM_DECL void path_algorithms::increment_v3(path_detail::path_iterator& it)
{
    const size_type size = it.m_path_ptr->m_pathname.size();
    BOOST_ASSERT_MSG(it.m_pos < size, "path::iterator increment past end()");

    // increment to position past current element; if current element is implicit dot,
    // this will cause m_pos to represent the end iterator
    it.m_pos += it.m_element.m_pathname.size();

    // if the end is reached, we are done
    if (it.m_pos >= size)
    {
        BOOST_ASSERT_MSG(it.m_pos == size, "path::iterator increment after the referenced path was modified");
        it.m_element.clear(); // aids debugging
        return;
    }

    // process separator (Windows drive spec is only case not a separator)
    if (detail::is_directory_separator(it.m_path_ptr->m_pathname[it.m_pos]))
    {
        size_type root_name_size = 0;
        size_type root_dir_pos = find_root_directory_start(it.m_path_ptr->m_pathname.c_str(), size, root_name_size);

        // detect root directory and set iterator value to the separator if it is
        if (it.m_pos == root_dir_pos && it.m_element.m_pathname.size() == root_name_size)
        {
            it.m_element.m_pathname = path::separator; // generic format; see docs
            return;
        }

        // skip separators until m_pos points to the start of the next element
        while (it.m_pos != size && detail::is_directory_separator(it.m_path_ptr->m_pathname[it.m_pos]))
        {
            ++it.m_pos;
        }

        // detect trailing separator, and treat it as ".", per POSIX spec
        if (it.m_pos == size &&
            !is_root_separator(it.m_path_ptr->m_pathname, root_dir_pos, it.m_pos - 1))
        {
            --it.m_pos;
            it.m_element = detail::dot_path();
            return;
        }
    }

    // get m_element
    size_type end_pos = it.m_path_ptr->m_pathname.find_first_of(separators, it.m_pos);
    if (end_pos == string_type::npos)
        end_pos = size;
    const path::value_type* p = it.m_path_ptr->m_pathname.c_str();
    it.m_element.m_pathname.assign(p + it.m_pos, p + end_pos);
}

BOOST_FILESYSTEM_DECL void path_algorithms::increment_v4(path_detail::path_iterator& it)
{
    const size_type size = it.m_path_ptr->m_pathname.size();
    BOOST_ASSERT_MSG(it.m_pos <= size, "path::iterator increment past end()");

    if (it.m_element.m_pathname.empty() && (it.m_pos + 1) == size && detail::is_directory_separator(it.m_path_ptr->m_pathname[it.m_pos]))
    {
        // The iterator was pointing to the last empty element of the path; set to end.
        it.m_pos = size;
        return;
    }

    // increment to position past current element; if current element is implicit dot,
    // this will cause m_pos to represent the end iterator
    it.m_pos += it.m_element.m_pathname.size();

    // if the end is reached, we are done
    if (it.m_pos >= size)
    {
        BOOST_ASSERT_MSG(it.m_pos == size, "path::iterator increment after the referenced path was modified");
        it.m_element.clear(); // aids debugging
        return;
    }

    // process separator (Windows drive spec is only case not a separator)
    if (detail::is_directory_separator(it.m_path_ptr->m_pathname[it.m_pos]))
    {
        size_type root_name_size = 0;
        size_type root_dir_pos = find_root_directory_start(it.m_path_ptr->m_pathname.c_str(), size, root_name_size);

        // detect root directory and set iterator value to the separator if it is
        if (it.m_pos == root_dir_pos && it.m_element.m_pathname.size() == root_name_size)
        {
            it.m_element.m_pathname = path::separator; // generic format; see docs
            return;
        }

        // skip separators until m_pos points to the start of the next element
        while (it.m_pos != size && detail::is_directory_separator(it.m_path_ptr->m_pathname[it.m_pos]))
        {
            ++it.m_pos;
        }

        // detect trailing separator
        if (it.m_pos == size &&
            !is_root_separator(it.m_path_ptr->m_pathname, root_dir_pos, it.m_pos - 1))
        {
            --it.m_pos;
            it.m_element.m_pathname.clear();
            return;
        }
    }

    // get m_element
    size_type end_pos = it.m_path_ptr->m_pathname.find_first_of(separators, it.m_pos);
    if (end_pos == string_type::npos)
        end_pos = size;
    const path::value_type* p = it.m_path_ptr->m_pathname.c_str();
    it.m_element.m_pathname.assign(p + it.m_pos, p + end_pos);
}

BOOST_FILESYSTEM_DECL void path_algorithms::decrement_v3(path_detail::path_iterator& it)
{
    const size_type size = it.m_path_ptr->m_pathname.size();
    BOOST_ASSERT_MSG(it.m_pos > 0, "path::iterator decrement past begin()");
    BOOST_ASSERT_MSG(it.m_pos <= size, "path::iterator decrement after the referenced path was modified");

    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(it.m_path_ptr->m_pathname.c_str(), size, root_name_size);

    if (root_dir_pos < size && it.m_pos == root_dir_pos)
    {
        // Was pointing at root directory, decrement to root name
    set_to_root_name:
        it.m_pos = 0u;
        const path::value_type* p = it.m_path_ptr->m_pathname.c_str();
        it.m_element.m_pathname.assign(p, p + root_name_size);
        return;
    }

    // if at end and there was a trailing non-root '/', return "."
    if (it.m_pos == size &&
        size > 1 &&
        detail::is_directory_separator(it.m_path_ptr->m_pathname[it.m_pos - 1]) &&
        !is_root_separator(it.m_path_ptr->m_pathname, root_dir_pos, it.m_pos - 1))
    {
        --it.m_pos;
        it.m_element = detail::dot_path();
        return;
    }

    // skip separators unless root directory
    size_type end_pos = it.m_pos;
    while (end_pos > root_name_size)
    {
        --end_pos;

        if (end_pos == root_dir_pos)
        {
            // Decremented to the root directory
            it.m_pos = end_pos;
            it.m_element.m_pathname = path::separator; // generic format; see docs
            return;
        }

        if (!detail::is_directory_separator(it.m_path_ptr->m_pathname[end_pos]))
        {
            ++end_pos;
            break;
        }
    }

    if (end_pos <= root_name_size)
        goto set_to_root_name;

    size_type filename_size = find_filename_size(it.m_path_ptr->m_pathname, root_name_size, end_pos);
    it.m_pos = end_pos - filename_size;
    const path::value_type* p = it.m_path_ptr->m_pathname.c_str();
    it.m_element.m_pathname.assign(p + it.m_pos, p + end_pos);
}

BOOST_FILESYSTEM_DECL void path_algorithms::decrement_v4(path_detail::path_iterator& it)
{
    const size_type size = it.m_path_ptr->m_pathname.size();
    BOOST_ASSERT_MSG(it.m_pos > 0, "path::iterator decrement past begin()");
    BOOST_ASSERT_MSG(it.m_pos <= size, "path::iterator decrement after the referenced path was modified");

    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(it.m_path_ptr->m_pathname.c_str(), size, root_name_size);

    if (root_dir_pos < size && it.m_pos == root_dir_pos)
    {
        // Was pointing at root directory, decrement to root name
    set_to_root_name:
        it.m_pos = 0u;
        const path::value_type* p = it.m_path_ptr->m_pathname.c_str();
        it.m_element.m_pathname.assign(p, p + root_name_size);
        return;
    }

    // if at end and there was a trailing '/', return ""
    if (it.m_pos == size &&
        size > 1 &&
        detail::is_directory_separator(it.m_path_ptr->m_pathname[it.m_pos - 1]) &&
        !is_root_separator(it.m_path_ptr->m_pathname, root_dir_pos, it.m_pos - 1))
    {
        --it.m_pos;
        it.m_element.m_pathname.clear();
        return;
    }

    // skip separators unless root directory
    size_type end_pos = it.m_pos;
    while (end_pos > root_name_size)
    {
        --end_pos;

        if (end_pos == root_dir_pos)
        {
            // Decremented to the root directory
            it.m_pos = end_pos;
            it.m_element.m_pathname = path::separator; // generic format; see docs
            return;
        }

        if (!detail::is_directory_separator(it.m_path_ptr->m_pathname[end_pos]))
        {
            ++end_pos;
            break;
        }
    }

    if (end_pos <= root_name_size)
        goto set_to_root_name;

    size_type filename_size = find_filename_size(it.m_path_ptr->m_pathname, root_name_size, end_pos);
    it.m_pos = end_pos - filename_size;
    const path::value_type* p = it.m_path_ptr->m_pathname.c_str();
    it.m_element.m_pathname.assign(p + it.m_pos, p + end_pos);
}

} // namespace detail

//  path iterators  ------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL path::iterator path::begin() const
{
    iterator itr;
    itr.m_path_ptr = this;

    size_type element_size;
    first_element(m_pathname, itr.m_pos, element_size);

    if (element_size > 0)
    {
        itr.m_element = m_pathname.substr(itr.m_pos, element_size);
#ifdef BOOST_WINDOWS_API
        if (itr.m_element.m_pathname.size() == 1u && itr.m_element.m_pathname[0] == path::preferred_separator)
            itr.m_element.m_pathname[0] = path::separator;
#endif
    }

    return itr;
}

BOOST_FILESYSTEM_DECL path::iterator path::end() const
{
    iterator itr;
    itr.m_path_ptr = this;
    itr.m_pos = m_pathname.size();
    return itr;
}

} // namespace filesystem
} // namespace boost

namespace {

//------------------------------------------------------------------------------------//
//                                locale helpers                                      //
//------------------------------------------------------------------------------------//

//  Prior versions of these locale and codecvt implementations tried to take advantage
//  of static initialization where possible, kept a local copy of the current codecvt
//  facet (to avoid codecvt() having to call use_facet()), and was not multi-threading
//  safe (again for efficiency).
//
//  This was error prone, and required different implementation techniques depending
//  on the compiler and also whether static or dynamic linking was used. Furthermore,
//  users could not easily provide their multi-threading safe wrappers because the
//  path interface requires the implementation itself to call codecvt() to obtain the
//  default facet, and the initialization of the static within path_locale() could race.
//
//  The code below is portable to all platforms, is much simpler, and hopefully will be
//  much more robust. Timing tests (on Windows, using a Visual C++ release build)
//  indicated the current code is roughly 9% slower than the previous code, and that
//  seems a small price to pay for better code that is easier to use.

std::locale default_locale()
{
#if defined(BOOST_WINDOWS_API)
    std::locale global_loc = std::locale();
    return std::locale(global_loc, new boost::filesystem::detail::windows_file_codecvt());
#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__HAIKU__)
    // "All BSD system functions expect their string parameters to be in UTF-8 encoding
    // and nothing else." See
    // http://developer.apple.com/mac/library/documentation/MacOSX/Conceptual/BPInternational/Articles/FileEncodings.html
    //
    // "The kernel will reject any filename that is not a valid UTF-8 string, and it will
    // even be normalized (to Unicode NFD) before stored on disk, at least when using HFS.
    // The right way to deal with it would be to always convert the filename to UTF-8
    // before trying to open/create a file." See
    // http://lists.apple.com/archives/unix-porting/2007/Sep/msg00023.html
    //
    // "How a file name looks at the API level depends on the API. Current Carbon APIs
    // handle file names as an array of UTF-16 characters; POSIX ones handle them as an
    // array of UTF-8, which is why UTF-8 works well in Terminal. How it's stored on disk
    // depends on the disk format; HFS+ uses UTF-16, but that's not important in most
    // cases." See
    // http://lists.apple.com/archives/applescript-users/2002/Sep/msg00319.html
    //
    // Many thanks to Peter Dimov for digging out the above references!

    std::locale global_loc = std::locale();
    return std::locale(global_loc, new boost::filesystem::detail::utf8_codecvt_facet());
#else // Other POSIX
    // ISO C calls std::locale("") "the locale-specific native environment", and this
    // locale is the default for many POSIX-based operating systems such as Linux.
    return std::locale("");
#endif
}

std::locale* g_path_locale = nullptr;

void schedule_path_locale_cleanup() noexcept;

// std::locale("") construction, needed on non-Apple POSIX systems, can throw
// (if environmental variables LC_MESSAGES or LANG are wrong, for example), so
// get_path_locale() provides lazy initialization to ensure that any
// exceptions occur after main() starts and so can be caught. Furthermore,
// g_path_locale is only initialized if path::codecvt() or path::imbue() are themselves
// actually called, ensuring that an exception will only be thrown if std::locale("")
// is really needed.
inline std::locale& get_path_locale()
{
#if !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
    atomic_ns::atomic_ref< std::locale* > a(g_path_locale);
    std::locale* p = a.load(atomic_ns::memory_order_acquire);
    if (BOOST_UNLIKELY(!p))
    {
        std::locale* new_p = new std::locale(default_locale());
        if (a.compare_exchange_strong(p, new_p, atomic_ns::memory_order_acq_rel, atomic_ns::memory_order_acquire))
        {
            p = new_p;
            schedule_path_locale_cleanup();
        }
        else
        {
            delete new_p;
        }
    }
    return *p;
#else // !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
    std::locale* p = g_path_locale;
    if (BOOST_UNLIKELY(!p))
    {
        g_path_locale = p = new std::locale(default_locale());
        schedule_path_locale_cleanup();
    }
    return *p;
#endif // !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
}

inline std::locale* replace_path_locale(std::locale const& loc)
{
    std::locale* new_p = new std::locale(loc);
#if !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
    std::locale* p = atomic_ns::atomic_ref< std::locale* >(g_path_locale).exchange(new_p, atomic_ns::memory_order_acq_rel);
#else
    std::locale* p = g_path_locale;
    g_path_locale = new_p;
#endif
    if (!p)
        schedule_path_locale_cleanup();
    return p;
}

#if defined(_MSC_VER)

const boost::filesystem::path* g_dot_path = nullptr;
const boost::filesystem::path* g_dot_dot_path = nullptr;

inline void schedule_path_locale_cleanup() noexcept
{
}

inline boost::filesystem::path const& get_dot_path()
{
#if !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
    atomic_ns::atomic_ref< const boost::filesystem::path* > a(g_dot_path);
    const boost::filesystem::path* p = a.load(atomic_ns::memory_order_acquire);
    if (BOOST_UNLIKELY(!p))
    {
        const boost::filesystem::path* new_p = new boost::filesystem::path(dot_path_literal);
        if (a.compare_exchange_strong(p, new_p, atomic_ns::memory_order_acq_rel, atomic_ns::memory_order_acquire))
            p = new_p;
        else
            delete new_p;
    }
    return *p;
#else // !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
    const boost::filesystem::path* p = g_dot_path;
    if (BOOST_UNLIKELY(!p))
        g_dot_path = p = new boost::filesystem::path(dot_path_literal);
    return *p;
#endif // !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
}

inline boost::filesystem::path const& get_dot_dot_path()
{
#if !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
    atomic_ns::atomic_ref< const boost::filesystem::path* > a(g_dot_dot_path);
    const boost::filesystem::path* p = a.load(atomic_ns::memory_order_acquire);
    if (BOOST_UNLIKELY(!p))
    {
        const boost::filesystem::path* new_p = new boost::filesystem::path(dot_dot_path_literal);
        if (a.compare_exchange_strong(p, new_p, atomic_ns::memory_order_acq_rel, atomic_ns::memory_order_acquire))
            p = new_p;
        else
            delete new_p;
    }
    return *p;
#else // !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
    const boost::filesystem::path* p = g_dot_dot_path;
    if (BOOST_UNLIKELY(!p))
        g_dot_dot_path = p = new boost::filesystem::path(dot_dot_path_literal);
    return *p;
#endif // !defined(BOOST_FILESYSTEM_SINGLE_THREADED)
}

void __cdecl destroy_path_globals()
{
    delete g_dot_dot_path;
    g_dot_dot_path = nullptr;
    delete g_dot_path;
    g_dot_path = nullptr;
    delete g_path_locale;
    g_path_locale = nullptr;
}

BOOST_FILESYSTEM_INIT_FUNC init_path_globals()
{
#if !defined(BOOST_SYSTEM_HAS_CONSTEXPR)
    // codecvt_error_category needs to be called early to dynamic-initialize the error category instance
    boost::filesystem::codecvt_error_category();
#endif
    std::atexit(&destroy_path_globals);
    return BOOST_FILESYSTEM_INITRETSUCCESS_V;
}

#if _MSC_VER >= 1400

#pragma section(".CRT$XCM", long, read)
__declspec(allocate(".CRT$XCM")) BOOST_ATTRIBUTE_UNUSED BOOST_FILESYSTEM_ATTRIBUTE_RETAIN
extern const init_func_ptr_t p_init_path_globals = &init_path_globals;

#else // _MSC_VER >= 1400

#if (_MSC_VER >= 1300) // 1300 == VC++ 7.0
#pragma data_seg(push, old_seg)
#endif
#pragma data_seg(".CRT$XCM")
BOOST_ATTRIBUTE_UNUSED BOOST_FILESYSTEM_ATTRIBUTE_RETAIN
extern const init_func_ptr_t p_init_path_globals = &init_path_globals;
#pragma data_seg()
#if (_MSC_VER >= 1300) // 1300 == VC++ 7.0
#pragma data_seg(pop, old_seg)
#endif

#endif // _MSC_VER >= 1400

#if defined(BOOST_FILESYSTEM_NO_ATTRIBUTE_RETAIN)
//! Makes sure the global initializer pointers are referenced and not removed by linker
struct globals_retainer
{
    const init_func_ptr_t* volatile m_p_init_path_globals;

    globals_retainer() { m_p_init_path_globals = &p_init_path_globals; }
};
BOOST_ATTRIBUTE_UNUSED
static const globals_retainer g_globals_retainer;
#endif // defined(BOOST_FILESYSTEM_NO_ATTRIBUTE_RETAIN)

#else // defined(_MSC_VER)

struct path_locale_deleter
{
    ~path_locale_deleter()
    {
        delete g_path_locale;
        g_path_locale = nullptr;
    }
};

#if defined(BOOST_FILESYSTEM_HAS_INIT_PRIORITY)

BOOST_FILESYSTEM_INIT_PRIORITY(BOOST_FILESYSTEM_PATH_GLOBALS_INIT_PRIORITY) BOOST_ATTRIBUTE_UNUSED
const path_locale_deleter g_path_locale_deleter = {};
BOOST_FILESYSTEM_INIT_PRIORITY(BOOST_FILESYSTEM_PATH_GLOBALS_INIT_PRIORITY)
const boost::filesystem::path g_dot_path(dot_path_literal);
BOOST_FILESYSTEM_INIT_PRIORITY(BOOST_FILESYSTEM_PATH_GLOBALS_INIT_PRIORITY)
const boost::filesystem::path g_dot_dot_path(dot_dot_path_literal);

inline void schedule_path_locale_cleanup() noexcept
{
}

inline boost::filesystem::path const& get_dot_path()
{
    return g_dot_path;
}

inline boost::filesystem::path const& get_dot_dot_path()
{
    return g_dot_dot_path;
}

#else // defined(BOOST_FILESYSTEM_HAS_INIT_PRIORITY)

inline void schedule_path_locale_cleanup() noexcept
{
    BOOST_ATTRIBUTE_UNUSED static const path_locale_deleter g_path_locale_deleter;
}

inline boost::filesystem::path const& get_dot_path()
{
    static const boost::filesystem::path g_dot_path(dot_path_literal);
    return g_dot_path;
}

inline boost::filesystem::path const& get_dot_dot_path()
{
    static const boost::filesystem::path g_dot_dot_path(dot_dot_path_literal);
    return g_dot_dot_path;
}

#endif // defined(BOOST_FILESYSTEM_HAS_INIT_PRIORITY)

#endif // defined(_MSC_VER)

} // unnamed namespace

//--------------------------------------------------------------------------------------//
//              path::codecvt() and path::imbue() implementation                        //
//--------------------------------------------------------------------------------------//

namespace boost {
namespace filesystem {

BOOST_FILESYSTEM_DECL path::codecvt_type const& path::codecvt()
{
#ifdef BOOST_FILESYSTEM_DEBUG
    std::cout << "***** path::codecvt() called" << std::endl;
#endif
    return std::use_facet< std::codecvt< wchar_t, char, std::mbstate_t > >(get_path_locale());
}

BOOST_FILESYSTEM_DECL std::locale path::imbue(std::locale const& loc)
{
#ifdef BOOST_FILESYSTEM_DEBUG
    std::cout << "***** path::imbue() called" << std::endl;
#endif
    std::locale* p = replace_path_locale(loc);
    if (BOOST_LIKELY(p != nullptr))
    {
        // Note: copying/moving std::locale does not throw
        std::locale temp(std::move(*p));
        delete p;
        return temp;
    }

    return default_locale();
}

namespace detail {

BOOST_FILESYSTEM_DECL path const& dot_path()
{
    return get_dot_path();
}

BOOST_FILESYSTEM_DECL path const& dot_dot_path()
{
    return get_dot_dot_path();
}

} // namespace detail
} // namespace filesystem
} // namespace boost

#include <boost/filesystem/detail/footer.hpp>
