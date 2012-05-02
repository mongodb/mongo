
// Copyright 2005-2009 Daniel James.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  Based on Peter Dimov's proposal
//  http://www.open-std.org/JTC1/SC22/WG21/docs/papers/2005/n1756.pdf
//  issue 6.18. 

// This implements the extensions to the standard.
// It's undocumented, so you shouldn't use it....

#if !defined(BOOST_FUNCTIONAL_HASH_EXTENSIONS_HPP)
#define BOOST_FUNCTIONAL_HASH_EXTENSIONS_HPP

#include <boost/functional/hash/hash.hpp>
#include <boost/detail/container_fwd.hpp>

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#if defined(BOOST_NO_FUNCTION_TEMPLATE_ORDERING)
#include <boost/type_traits/is_array.hpp>
#endif

#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
#include <boost/type_traits/is_const.hpp>
#endif

namespace boost
{
    template <class A, class B>
    std::size_t hash_value(std::pair<A, B> const&);
    template <class T, class A>
    std::size_t hash_value(std::vector<T, A> const&);
    template <class T, class A>
    std::size_t hash_value(std::list<T, A> const& v);
    template <class T, class A>
    std::size_t hash_value(std::deque<T, A> const& v);
    template <class K, class C, class A>
    std::size_t hash_value(std::set<K, C, A> const& v);
    template <class K, class C, class A>
    std::size_t hash_value(std::multiset<K, C, A> const& v);
    template <class K, class T, class C, class A>
    std::size_t hash_value(std::map<K, T, C, A> const& v);
    template <class K, class T, class C, class A>
    std::size_t hash_value(std::multimap<K, T, C, A> const& v);

    template <class T>
    std::size_t hash_value(std::complex<T> const&);

    template <class A, class B>
    std::size_t hash_value(std::pair<A, B> const& v)
    {
        std::size_t seed = 0;
        hash_combine(seed, v.first);
        hash_combine(seed, v.second);
        return seed;
    }

    template <class T, class A>
    std::size_t hash_value(std::vector<T, A> const& v)
    {
        return hash_range(v.begin(), v.end());
    }

    template <class T, class A>
    std::size_t hash_value(std::list<T, A> const& v)
    {
        return hash_range(v.begin(), v.end());
    }

    template <class T, class A>
    std::size_t hash_value(std::deque<T, A> const& v)
    {
        return hash_range(v.begin(), v.end());
    }

    template <class K, class C, class A>
    std::size_t hash_value(std::set<K, C, A> const& v)
    {
        return hash_range(v.begin(), v.end());
    }

    template <class K, class C, class A>
    std::size_t hash_value(std::multiset<K, C, A> const& v)
    {
        return hash_range(v.begin(), v.end());
    }

    template <class K, class T, class C, class A>
    std::size_t hash_value(std::map<K, T, C, A> const& v)
    {
        return hash_range(v.begin(), v.end());
    }

    template <class K, class T, class C, class A>
    std::size_t hash_value(std::multimap<K, T, C, A> const& v)
    {
        return hash_range(v.begin(), v.end());
    }

    template <class T>
    std::size_t hash_value(std::complex<T> const& v)
    {
        boost::hash<T> hasher;
        std::size_t seed = hasher(v.imag());
        seed ^= hasher(v.real()) + (seed<<6) + (seed>>2);
        return seed;
    }

    //
    // call_hash_impl
    //

    // On compilers without function template ordering, this deals with arrays.

#if defined(BOOST_NO_FUNCTION_TEMPLATE_ORDERING)
    namespace hash_detail
    {
        template <bool IsArray>
        struct call_hash_impl
        {
            template <class T>
            struct inner
            {
                static std::size_t call(T const& v)
                {
                    using namespace boost;
                    return hash_value(v);
                }
            };
        };

        template <>
        struct call_hash_impl<true>
        {
            template <class Array>
            struct inner
            {
#if !BOOST_WORKAROUND(BOOST_MSVC, < 1300)
                static std::size_t call(Array const& v)
#else
                static std::size_t call(Array& v)
#endif
                {
                    const int size = sizeof(v) / sizeof(*v);
                    return boost::hash_range(v, v + size);
                }
            };
        };

        template <class T>
        struct call_hash
            : public call_hash_impl<boost::is_array<T>::value>
                ::BOOST_NESTED_TEMPLATE inner<T>
        {
        };
    }
#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING

    //
    // boost::hash
    //


#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)

    template <class T> struct hash
        : std::unary_function<T, std::size_t>
    {
#if !defined(BOOST_NO_FUNCTION_TEMPLATE_ORDERING)
        std::size_t operator()(T const& val) const
        {
            return hash_value(val);
        }
#else
        std::size_t operator()(T const& val) const
        {
            return hash_detail::call_hash<T>::call(val);
        }
#endif
    };

#if BOOST_WORKAROUND(__DMC__, <= 0x848)
    template <class T, unsigned int n> struct hash<T[n]>
        : std::unary_function<T[n], std::size_t>
    {
        std::size_t operator()(const T* val) const
        {
            return boost::hash_range(val, val+n);
        }
    };
#endif

#else // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

    // On compilers without partial specialization, boost::hash<T>
    // has already been declared to deal with pointers, so just
    // need to supply the non-pointer version of hash_impl.

    namespace hash_detail
    {
        template <bool IsPointer>
        struct hash_impl;

#if !BOOST_WORKAROUND(BOOST_MSVC, < 1300)

        template <>
        struct hash_impl<false>
        {
            template <class T>
            struct inner
                : std::unary_function<T, std::size_t>
            {
#if !defined(BOOST_NO_FUNCTION_TEMPLATE_ORDERING)
                std::size_t operator()(T const& val) const
                {
                    return hash_value(val);
                }
#else
                std::size_t operator()(T const& val) const
                {
                    return hash_detail::call_hash<T>::call(val);
                }
#endif
            };
        };

#else // Visual C++ 6.5

        // Visual C++ 6.5 has problems with nested member functions and
        // applying const to const types in templates. So we get this:

        template <bool IsConst>
        struct hash_impl_msvc
        {
            template <class T>
            struct inner
                : public std::unary_function<T, std::size_t>
            {
                std::size_t operator()(T const& val) const
                {
                    return hash_detail::call_hash<T const>::call(val);
                }

                std::size_t operator()(T& val) const
                {
                    return hash_detail::call_hash<T>::call(val);
                }
            };
        };

        template <>
        struct hash_impl_msvc<true>
        {
            template <class T>
            struct inner
                : public std::unary_function<T, std::size_t>
            {
                std::size_t operator()(T& val) const
                {
                    return hash_detail::call_hash<T>::call(val);
                }
            };
        };
        
        template <class T>
        struct hash_impl_msvc2
            : public hash_impl_msvc<boost::is_const<T>::value>
                    ::BOOST_NESTED_TEMPLATE inner<T> {};
        
        template <>
        struct hash_impl<false>
        {
            template <class T>
            struct inner : public hash_impl_msvc2<T> {};
        };

#endif // Visual C++ 6.5
    }
#endif  // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
}

#endif
