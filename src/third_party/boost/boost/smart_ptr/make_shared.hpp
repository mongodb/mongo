#ifndef BOOST_SMART_PTR_MAKE_SHARED_HPP_INCLUDED
#define BOOST_SMART_PTR_MAKE_SHARED_HPP_INCLUDED

//  make_shared.hpp
//
//  Copyright (c) 2007, 2008 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt
//
//  See http://www.boost.org/libs/smart_ptr/make_shared.html
//  for documentation.

#include <boost/config.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/type_traits/type_with_alignment.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <cstddef>
#include <new>

namespace boost
{

namespace detail
{

template< std::size_t N, std::size_t A > struct sp_aligned_storage
{
    union type
    {
        char data_[ N ];
        typename boost::type_with_alignment< A >::type align_;
    };
};

template< class T > class sp_ms_deleter
{
private:

    typedef typename sp_aligned_storage< sizeof( T ), ::boost::alignment_of< T >::value >::type storage_type;

    bool initialized_;
    storage_type storage_;

private:

    void destroy()
    {
        if( initialized_ )
        {
#if defined( __GNUC__ )

            // fixes incorrect aliasing warning
            T * p = reinterpret_cast< T* >( storage_.data_ );
            p->~T();

#else

            reinterpret_cast< T* >( storage_.data_ )->~T();

#endif

            initialized_ = false;
        }
    }

public:

    sp_ms_deleter(): initialized_( false )
    {
    }

    // optimization: do not copy storage_
    sp_ms_deleter( sp_ms_deleter const & ): initialized_( false )
    {
    }

    ~sp_ms_deleter()
    {
        destroy();
    }

    void operator()( T * )
    {
        destroy();
    }

    void * address()
    {
        return storage_.data_;
    }

    void set_initialized()
    {
        initialized_ = true;
    }
};

#if defined( BOOST_HAS_RVALUE_REFS )

template< class T > T&& sp_forward( T & t )
{
    return static_cast< T&& >( t );
}

#endif

} // namespace detail

#if !defined( BOOST_NO_FUNCTION_TEMPLATE_ORDERING )
# define BOOST_SP_MSD( T ) boost::detail::sp_inplace_tag< boost::detail::sp_ms_deleter< T > >()
#else
# define BOOST_SP_MSD( T ) boost::detail::sp_ms_deleter< T >()
#endif

// Zero-argument versions
//
// Used even when variadic templates are available because of the new T() vs new T issue

template< class T > boost::shared_ptr< T > make_shared()
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T();
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A > boost::shared_ptr< T > allocate_shared( A const & a )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T();
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

#if defined( BOOST_HAS_VARIADIC_TMPL ) && defined( BOOST_HAS_RVALUE_REFS )

// Variadic templates, rvalue reference

template< class T, class Arg1, class... Args > boost::shared_ptr< T > make_shared( Arg1 && arg1, Args && ... args )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( boost::detail::sp_forward<Arg1>( arg1 ), boost::detail::sp_forward<Args>( args )... );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class Arg1, class... Args > boost::shared_ptr< T > allocate_shared( A const & a, Arg1 && arg1, Args && ... args )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( boost::detail::sp_forward<Arg1>( arg1 ), boost::detail::sp_forward<Args>( args )... );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

#elif defined( BOOST_HAS_RVALUE_REFS )

// For example MSVC 10.0

template< class T, class A1 >
boost::shared_ptr< T > make_shared( A1 && a1 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T(
        boost::detail::sp_forward<A1>( a1 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 && a1 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( 
        boost::detail::sp_forward<A1>( a1 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2 >
boost::shared_ptr< T > make_shared( A1 && a1, A2 && a2 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T(
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 && a1, A2 && a2 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( 
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3 >
boost::shared_ptr< T > make_shared( A1 && a1, A2 && a2, A3 && a3 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T(
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 && a1, A2 && a2, A3 && a3 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( 
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4 >
boost::shared_ptr< T > make_shared( A1 && a1, A2 && a2, A3 && a3, A4 && a4 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T(
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 && a1, A2 && a2, A3 && a3, A4 && a4 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( 
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5 >
boost::shared_ptr< T > make_shared( A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T(
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( 
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5, class A6 >
boost::shared_ptr< T > make_shared( A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5, A6 && a6 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T(
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 ), 
        boost::detail::sp_forward<A6>( a6 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5, class A6 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5, A6 && a6 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( 
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 ), 
        boost::detail::sp_forward<A6>( a6 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5, class A6, class A7 >
boost::shared_ptr< T > make_shared( A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5, A6 && a6, A7 && a7 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T(
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 ), 
        boost::detail::sp_forward<A6>( a6 ), 
        boost::detail::sp_forward<A7>( a7 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5, class A6, class A7 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5, A6 && a6, A7 && a7 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( 
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 ), 
        boost::detail::sp_forward<A6>( a6 ), 
        boost::detail::sp_forward<A7>( a7 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8 >
boost::shared_ptr< T > make_shared( A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5, A6 && a6, A7 && a7, A8 && a8 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T(
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 ), 
        boost::detail::sp_forward<A6>( a6 ), 
        boost::detail::sp_forward<A7>( a7 ), 
        boost::detail::sp_forward<A8>( a8 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5, A6 && a6, A7 && a7, A8 && a8 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( 
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 ), 
        boost::detail::sp_forward<A6>( a6 ), 
        boost::detail::sp_forward<A7>( a7 ), 
        boost::detail::sp_forward<A8>( a8 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9 >
boost::shared_ptr< T > make_shared( A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5, A6 && a6, A7 && a7, A8 && a8, A9 && a9 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T(
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 ), 
        boost::detail::sp_forward<A6>( a6 ), 
        boost::detail::sp_forward<A7>( a7 ), 
        boost::detail::sp_forward<A8>( a8 ), 
        boost::detail::sp_forward<A9>( a9 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 && a1, A2 && a2, A3 && a3, A4 && a4, A5 && a5, A6 && a6, A7 && a7, A8 && a8, A9 && a9 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( 
        boost::detail::sp_forward<A1>( a1 ), 
        boost::detail::sp_forward<A2>( a2 ), 
        boost::detail::sp_forward<A3>( a3 ), 
        boost::detail::sp_forward<A4>( a4 ), 
        boost::detail::sp_forward<A5>( a5 ), 
        boost::detail::sp_forward<A6>( a6 ), 
        boost::detail::sp_forward<A7>( a7 ), 
        boost::detail::sp_forward<A8>( a8 ), 
        boost::detail::sp_forward<A9>( a9 )
        );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

#else

// C++03 version

template< class T, class A1 >
boost::shared_ptr< T > make_shared( A1 const & a1 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 const & a1 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2 >
boost::shared_ptr< T > make_shared( A1 const & a1, A2 const & a2 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 const & a1, A2 const & a2 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3 >
boost::shared_ptr< T > make_shared( A1 const & a1, A2 const & a2, A3 const & a3 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 const & a1, A2 const & a2, A3 const & a3 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4 >
boost::shared_ptr< T > make_shared( A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5 >
boost::shared_ptr< T > make_shared( A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5, class A6 >
boost::shared_ptr< T > make_shared( A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5, A6 const & a6 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5, a6 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5, class A6 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5, A6 const & a6 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5, a6 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5, class A6, class A7 >
boost::shared_ptr< T > make_shared( A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5, A6 const & a6, A7 const & a7 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5, a6, a7 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5, class A6, class A7 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5, A6 const & a6, A7 const & a7 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5, a6, a7 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8 >
boost::shared_ptr< T > make_shared( A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5, A6 const & a6, A7 const & a7, A8 const & a8 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5, a6, a7, a8 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5, A6 const & a6, A7 const & a7, A8 const & a8 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5, a6, a7, a8 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9 >
boost::shared_ptr< T > make_shared( A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5, A6 const & a6, A7 const & a7, A8 const & a8, A9 const & a9 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5, a6, a7, a8, a9 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9 >
boost::shared_ptr< T > allocate_shared( A const & a, A1 const & a1, A2 const & a2, A3 const & a3, A4 const & a4, A5 const & a5, A6 const & a6, A7 const & a7, A8 const & a8, A9 const & a9 )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = boost::get_deleter< boost::detail::sp_ms_deleter< T > >( pt );

    void * pv = pd->address();

    ::new( pv ) T( a1, a2, a3, a4, a5, a6, a7, a8, a9 );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

#endif

#undef BOOST_SP_MSD

} // namespace boost

#endif // #ifndef BOOST_SMART_PTR_MAKE_SHARED_HPP_INCLUDED
