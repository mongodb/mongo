// Copyright (C) 2002 Brad King (brad.king@kitware.com) 
//                    Douglas Gregor (gregod@cs.rpi.edu)
//
// Copyright (C) 2002, 2008 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// For more information, see http://www.boost.org

#ifndef BOOST_UTILITY_ADDRESSOF_HPP
# define BOOST_UTILITY_ADDRESSOF_HPP

# include <boost/config.hpp>
# include <boost/detail/workaround.hpp>

namespace boost
{

namespace detail
{

template<class T> struct addr_impl_ref
{
    T & v_;

    inline addr_impl_ref( T & v ): v_( v ) {}
    inline operator T& () const { return v_; }

private:
    addr_impl_ref & operator=(const addr_impl_ref &);
};

template<class T> struct addressof_impl
{
    static inline T * f( T & v, long )
    {
        return reinterpret_cast<T*>(
            &const_cast<char&>(reinterpret_cast<const volatile char &>(v)));
    }

    static inline T * f( T * v, int )
    {
        return v;
    }
};

} // namespace detail

template<class T> T * addressof( T & v )
{
#if defined( __BORLANDC__ ) && BOOST_WORKAROUND( __BORLANDC__, BOOST_TESTED_AT( 0x610 ) )

    return boost::detail::addressof_impl<T>::f( v, 0 );

#else

    return boost::detail::addressof_impl<T>::f( boost::detail::addr_impl_ref<T>( v ), 0 );

#endif
}

#if defined( __SUNPRO_CC ) && BOOST_WORKAROUND( __SUNPRO_CC, BOOST_TESTED_AT( 0x590 ) )

namespace detail
{

template<class T> struct addressof_addp
{
    typedef T * type;
};

} // namespace detail

template< class T, std::size_t N >
typename detail::addressof_addp< T[N] >::type addressof( T (&t)[N] )
{
    return &t;
}

#endif

// Borland doesn't like casting an array reference to a char reference
// but these overloads work around the problem.
#if defined( __BORLANDC__ ) && BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
template<typename T,std::size_t N>
T (*addressof(T (&t)[N]))[N]
{
   return reinterpret_cast<T(*)[N]>(&t);
}

template<typename T,std::size_t N>
const T (*addressof(const T (&t)[N]))[N]
{
   return reinterpret_cast<const T(*)[N]>(&t);
}
#endif

} // namespace boost

#endif // BOOST_UTILITY_ADDRESSOF_HPP
