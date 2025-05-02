#ifndef BOOST_ARRAY_HPP_INCLUDED
#define BOOST_ARRAY_HPP_INCLUDED

/* The following code declares class array,
 * an STL container (as wrapper) for arrays of constant size.
 *
 * See
 *      http://www.boost.org/libs/array/
 * for documentation.
 *
 * The original author site is at: http://www.josuttis.com/
 *
 * (C) Copyright Nicolai M. Josuttis 2001.
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 *  9 Jan 2013 - (mtc) Added constexpr
 * 14 Apr 2012 - (mtc) Added support for boost::hash
 * 28 Dec 2010 - (mtc) Added cbegin and cend (and crbegin and crend) for C++Ox compatibility.
 * 10 Mar 2010 - (mtc) fill method added, matching resolution of the standard library working group.
 *      See <http://www.open-std.org/jtc1/sc22/wg21/docs/lwg-defects.html#776> or Trac issue #3168
 *      Eventually, we should remove "assign" which is now a synonym for "fill" (Marshall Clow)
 * 10 Mar 2010 - added workaround for SUNCC and !STLPort [trac #3893] (Marshall Clow)
 * 29 Jan 2004 - c_array() added, BOOST_NO_PRIVATE_IN_AGGREGATE removed (Nico Josuttis)
 * 23 Aug 2002 - fix for Non-MSVC compilers combined with MSVC libraries.
 * 05 Aug 2001 - minor update (Nico Josuttis)
 * 20 Jan 2001 - STLport fix (Beman Dawes)
 * 29 Sep 2000 - Initial Revision (Nico Josuttis)
 *
 * Jan 29, 2004
 */

#include <boost/config.hpp>
#include <boost/config/workaround.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC, >= 1400)
# pragma warning(push)
# pragma warning(disable: 4510) // boost::array<T,N>' : default constructor could not be generated
# pragma warning(disable: 4512) // boost::array<T,N>' : assignment operator could not be generated
# pragma warning(disable: 4610) // class 'boost::array<T,N>' can never be instantiated - user defined constructor required
# pragma warning(disable: 4702) // unreachable code
#endif

#include <boost/assert.hpp>
#include <boost/static_assert.hpp>
#include <boost/throw_exception.hpp>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <cstddef>

#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
# if __has_include(<compare>)
#  include <compare>
# endif
#endif

namespace boost {

    template<class T, std::size_t N>
    class array {
      public:
        T elems[N];    // fixed-size array of elements of type T

      public:
        // type definitions
        typedef T              value_type;
        typedef T*             iterator;
        typedef const T*       const_iterator;
        typedef T&             reference;
        typedef const T&       const_reference;
        typedef std::size_t    size_type;
        typedef std::ptrdiff_t difference_type;

        // iterator support
        BOOST_CXX14_CONSTEXPR iterator  begin()       BOOST_NOEXCEPT { return elems; }
        BOOST_CONSTEXPR const_iterator  begin() const BOOST_NOEXCEPT { return elems; }
        BOOST_CONSTEXPR const_iterator cbegin() const BOOST_NOEXCEPT { return elems; }

        BOOST_CXX14_CONSTEXPR iterator  end()       BOOST_NOEXCEPT { return elems+N; }
        BOOST_CONSTEXPR const_iterator  end() const BOOST_NOEXCEPT { return elems+N; }
        BOOST_CONSTEXPR const_iterator cend() const BOOST_NOEXCEPT { return elems+N; }

        // reverse iterator support
        typedef std::reverse_iterator<iterator> reverse_iterator;
        typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

        reverse_iterator rbegin() BOOST_NOEXCEPT { return reverse_iterator(end()); }
        const_reverse_iterator rbegin() const BOOST_NOEXCEPT {
            return const_reverse_iterator(end());
        }
        const_reverse_iterator crbegin() const BOOST_NOEXCEPT {
            return const_reverse_iterator(end());
        }

        reverse_iterator rend() BOOST_NOEXCEPT { return reverse_iterator(begin()); }
        const_reverse_iterator rend() const BOOST_NOEXCEPT {
            return const_reverse_iterator(begin());
        }
        const_reverse_iterator crend() const BOOST_NOEXCEPT {
            return const_reverse_iterator(begin());
        }

        // operator[]
        BOOST_CXX14_CONSTEXPR reference operator[](size_type i)
        {
            return BOOST_ASSERT_MSG( i < N, "out of range" ), elems[i];
        }

#if !BOOST_WORKAROUND(BOOST_GCC, < 50000)
        BOOST_CONSTEXPR
#endif
        const_reference operator[](size_type i) const
        {
            return BOOST_ASSERT_MSG( i < N, "out of range" ), elems[i];
        }

        // at() with range check
        BOOST_CXX14_CONSTEXPR reference at(size_type i)       { return rangecheck(i), elems[i]; }
        BOOST_CONSTEXPR const_reference at(size_type i) const { return rangecheck(i), elems[i]; }

        // front() and back()
        BOOST_CXX14_CONSTEXPR reference front()
        {
            return elems[0];
        }

        BOOST_CONSTEXPR const_reference front() const
        {
            return elems[0];
        }

        BOOST_CXX14_CONSTEXPR reference back()
        {
            return elems[N-1];
        }

        BOOST_CONSTEXPR const_reference back() const
        {
            return elems[N-1];
        }

        // size is constant
        static BOOST_CONSTEXPR size_type size() BOOST_NOEXCEPT { return N; }
        static BOOST_CONSTEXPR bool empty() BOOST_NOEXCEPT { return false; }
        static BOOST_CONSTEXPR size_type max_size() BOOST_NOEXCEPT { return N; }
        enum { static_size = N };

        // swap (note: linear complexity)
        BOOST_CXX14_CONSTEXPR void swap (array<T,N>& y)
        {
            std::swap( elems, y.elems );
        }

        // direct access to data
        BOOST_CONSTEXPR const T* data() const BOOST_NOEXCEPT { return elems; }
        BOOST_CXX14_CONSTEXPR T* data() BOOST_NOEXCEPT { return elems; }

        // obsolete
        BOOST_DEPRECATED( "please use `data()` instead" )
        T* c_array() BOOST_NOEXCEPT { return elems; }

        // assignment with type conversion
        template <typename T2>
        array<T,N>& operator= (const array<T2,N>& rhs)
        {
            for( std::size_t i = 0; i < N; ++i )
            {
                elems[ i ] = rhs.elems[ i ];
            }

            return *this;
        }

        // fill with one value
        BOOST_CXX14_CONSTEXPR void fill (const T& value)
        {
            // using elems[ 0 ] as a temporary copy
            // avoids the aliasing opportunity betw.
            // `value` and `elems`

            elems[ 0 ] = value;

            for( std::size_t i = 1; i < N; ++i )
            {
                elems[ i ] = elems[ 0 ];
            }
        }

        // an obsolete synonym for fill
        BOOST_DEPRECATED( "please use `fill` instead" )
        void assign (const T& value) { fill ( value ); }

        // check range (may be private because it is static)
        static BOOST_CONSTEXPR bool rangecheck (size_type i) {
            return i >= size() ? boost::throw_exception(std::out_of_range ("array<>: index out of range")), true : true;
        }

    };

    template< class T >
    class array< T, 0 > {
      public:
        struct {} elems; // enables initialization with = {{}}

      public:
        // type definitions
        typedef T              value_type;
        typedef T*             iterator;
        typedef const T*       const_iterator;
        typedef T&             reference;
        typedef const T&       const_reference;
        typedef std::size_t    size_type;
        typedef std::ptrdiff_t difference_type;

        // iterator support
        BOOST_CXX14_CONSTEXPR iterator  begin()       BOOST_NOEXCEPT { return data(); }
        BOOST_CONSTEXPR const_iterator  begin() const BOOST_NOEXCEPT { return data(); }
        BOOST_CONSTEXPR const_iterator cbegin() const BOOST_NOEXCEPT { return data(); }

        BOOST_CXX14_CONSTEXPR iterator  end()       BOOST_NOEXCEPT { return  begin(); }
        BOOST_CONSTEXPR const_iterator  end() const BOOST_NOEXCEPT { return  begin(); }
        BOOST_CONSTEXPR const_iterator cend() const BOOST_NOEXCEPT { return cbegin(); }

        // reverse iterator support
        typedef std::reverse_iterator<iterator> reverse_iterator;
        typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

        reverse_iterator rbegin() BOOST_NOEXCEPT { return reverse_iterator(end()); }
        const_reverse_iterator rbegin() const BOOST_NOEXCEPT {
            return const_reverse_iterator(end());
        }
        const_reverse_iterator crbegin() const BOOST_NOEXCEPT {
            return const_reverse_iterator(end());
        }

        reverse_iterator rend() BOOST_NOEXCEPT { return reverse_iterator(begin()); }
        const_reverse_iterator rend() const BOOST_NOEXCEPT {
            return const_reverse_iterator(begin());
        }
        const_reverse_iterator crend() const BOOST_NOEXCEPT {
            return const_reverse_iterator(begin());
        }

        // operator[]
        reference operator[](size_type /*i*/)
        {
            return failed_rangecheck();
        }

        const_reference operator[](size_type /*i*/) const
        {
            return failed_rangecheck();
        }

        // at() with range check
        reference at(size_type /*i*/)               { return failed_rangecheck(); }
        const_reference at(size_type /*i*/) const   { return failed_rangecheck(); }

        // front() and back()
        reference front()
        {
            return failed_rangecheck();
        }

        const_reference front() const
        {
            return failed_rangecheck();
        }

        reference back()
        {
            return failed_rangecheck();
        }

        const_reference back() const
        {
            return failed_rangecheck();
        }

        // size is constant
        static BOOST_CONSTEXPR size_type size() BOOST_NOEXCEPT { return 0; }
        static BOOST_CONSTEXPR bool empty() BOOST_NOEXCEPT { return true; }
        static BOOST_CONSTEXPR size_type max_size() BOOST_NOEXCEPT { return 0; }
        enum { static_size = 0 };

        BOOST_CXX14_CONSTEXPR void swap (array<T,0>& /*y*/)
        {
        }

        // direct access to data
        BOOST_CONSTEXPR const T* data() const BOOST_NOEXCEPT { return 0; }
        BOOST_CXX14_CONSTEXPR T* data() BOOST_NOEXCEPT { return 0; }

        // obsolete
        BOOST_DEPRECATED( "please use `data()` instead" )
        T* c_array() BOOST_NOEXCEPT { return 0; }

        // assignment with type conversion
        template <typename T2>
        array<T,0>& operator= (const array<T2,0>& ) {
            return *this;
        }

        // an obsolete synonym for fill
        BOOST_DEPRECATED( "please use `fill` instead" )
        void assign (const T& value) { fill ( value ); }

        // fill with one value
        BOOST_CXX14_CONSTEXPR void fill (const T& ) {}

        // check range (may be private because it is static)
        static reference failed_rangecheck ()
        {
            boost::throw_exception( std::out_of_range( "attempt to access element of an empty array" ) );
        }
    };

    // comparisons
    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR bool operator== (const array<T,N>& x, const array<T,N>& y)
    {
        for( std::size_t i = 0; i < N; ++i )
        {
            if( !( x[ i ] == y[ i ] ) ) return false;
        }

        return true;
    }

#if BOOST_WORKAROUND(BOOST_GCC, < 90000)

    template<class T>
    BOOST_CXX14_CONSTEXPR bool operator== (const array<T, 0>& /*x*/, const array<T, 0>& /*y*/)
    {
        return true;
    }

#endif

    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR bool operator!= (const array<T,N>& x, const array<T,N>& y) {
        return !(x==y);
    }

    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR bool operator< (const array<T,N>& x, const array<T,N>& y)
    {
        for( std::size_t i = 0; i < N; ++i )
        {
            if( x[ i ] < y[ i ] ) return true;
            if( y[ i ] < x[ i ] ) return false;
        }

        return false;
    }

#if BOOST_WORKAROUND(BOOST_GCC, < 90000)

    template<class T>
    BOOST_CXX14_CONSTEXPR bool operator< (const array<T, 0>& /*x*/, const array<T, 0>& /*y*/)
    {
        return false;
    }

#endif

    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR bool operator> (const array<T,N>& x, const array<T,N>& y) {
        return y<x;
    }

    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR bool operator<= (const array<T,N>& x, const array<T,N>& y) {
        return !(y<x);
    }

    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR bool operator>= (const array<T,N>& x, const array<T,N>& y) {
        return !(x<y);
    }

    // global swap()
    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR inline void swap (array<T,N>& x, array<T,N>& y) {
        x.swap(y);
    }

#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
# if __has_include(<compare>)

    template<class T, std::size_t N>
    constexpr auto operator<=> (const array<T,N>& x, const array<T,N>& y)
        -> decltype( x.elems[ 0 ] <=> y.elems[ 0 ] )
    {
        for( std::size_t i = 0; i < N; ++i )
        {
            auto r = x.elems[ i ] <=> y.elems[ i ];
            if( r != 0 ) return r;
        }

        return std::strong_ordering::equal;
    }

    template<class T>
    constexpr auto operator<=> (const array<T,0>& /*x*/, const array<T,0>& /*y*/)
        -> std::strong_ordering
    {
        return std::strong_ordering::equal;
    }

# endif
#endif

    // undocumented and obsolete
    template <typename T, std::size_t N>
    BOOST_DEPRECATED( "please use `elems` instead" )
    T(&get_c_array(boost::array<T,N>& arg))[N]
    {
        return arg.elems;
    }

    // Const version.
    template <typename T, std::size_t N>
    BOOST_DEPRECATED( "please use `elems` instead" )
    const T(&get_c_array(const boost::array<T,N>& arg))[N]
    {
        return arg.elems;
    }

    template <size_t Idx, typename T, size_t N>
    BOOST_CXX14_CONSTEXPR T &get(boost::array<T,N> &arr) BOOST_NOEXCEPT
    {
        BOOST_STATIC_ASSERT_MSG ( Idx < N, "boost::get<>(boost::array &) index out of range" );
        return arr[Idx];
    }

    template <size_t Idx, typename T, size_t N>
    BOOST_CONSTEXPR const T &get(const boost::array<T,N> &arr) BOOST_NOEXCEPT
    {
        BOOST_STATIC_ASSERT_MSG ( Idx < N, "boost::get<>(const boost::array &) index out of range" );
        return arr[Idx];
    }

    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR array<T, N> to_array( T const (&a)[ N ] )
    {
        array<T, N> r = {};

        for( std::size_t i = 0; i < N; ++i )
        {
            r[ i ] = a[ i ];
        }

        return r;
    }

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)

    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR array<T, N> to_array( T (&&a)[ N ] )
    {
        array<T, N> r = {};

        for( std::size_t i = 0; i < N; ++i )
        {
            r[ i ] = std::move( a[ i ] );
        }

        return r;
    }

    template<class T, std::size_t N>
    BOOST_CXX14_CONSTEXPR array<T, N> to_array( T const (&&a)[ N ] )
    {
        array<T, N> r = {};

        for( std::size_t i = 0; i < N; ++i )
        {
            r[ i ] = a[ i ];
        }

        return r;
    }

#endif

} /* namespace boost */

#ifndef BOOST_NO_CXX11_HDR_ARRAY
//  If we don't have std::array, I'm assuming that we don't have std::get
namespace std {
   template <size_t Idx, typename T, size_t N>
   BOOST_DEPRECATED( "please use `boost::get` instead" )
   T &get(boost::array<T,N> &arr) BOOST_NOEXCEPT {
       BOOST_STATIC_ASSERT_MSG ( Idx < N, "std::get<>(boost::array &) index out of range" );
       return arr[Idx];
       }

   template <size_t Idx, typename T, size_t N>
   BOOST_DEPRECATED( "please use `boost::get` instead" )
   const T &get(const boost::array<T,N> &arr) BOOST_NOEXCEPT {
       BOOST_STATIC_ASSERT_MSG ( Idx < N, "std::get<>(const boost::array &) index out of range" );
       return arr[Idx];
       }
}
#endif

#if BOOST_WORKAROUND(BOOST_MSVC, >= 1400)
# pragma warning(pop)
#endif

#endif // #ifndef BOOST_ARRAY_HPP_INCLUDED
