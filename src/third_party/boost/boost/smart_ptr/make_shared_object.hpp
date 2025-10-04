#ifndef BOOST_SMART_PTR_MAKE_SHARED_OBJECT_HPP_INCLUDED
#define BOOST_SMART_PTR_MAKE_SHARED_OBJECT_HPP_INCLUDED

//  make_shared_object.hpp
//
//  Copyright (c) 2007, 2008, 2012 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt
//
//  See http://www.boost.org/libs/smart_ptr/ for documentation.

#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/detail/sp_type_traits.hpp>
#include <boost/config.hpp>
#include <utility>
#include <cstddef>
#include <new>
#include <type_traits>

namespace boost
{
namespace detail
{

template< std::size_t N, std::size_t A > struct sp_aligned_storage
{
    union type
    {
        char data_[ N ];
        typename sp_type_with_alignment< A >::type align_;
    };
};

template< class T > class sp_ms_deleter
{
private:

    typedef typename sp_aligned_storage< sizeof( T ), std::alignment_of< T >::value >::type storage_type;

    bool initialized_;
    storage_type storage_;

private:

    void destroy() noexcept
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

    sp_ms_deleter() noexcept : initialized_( false )
    {
    }

    template<class A> explicit sp_ms_deleter( A const & ) noexcept : initialized_( false )
    {
    }

    // optimization: do not copy storage_
    sp_ms_deleter( sp_ms_deleter const & ) noexcept : initialized_( false )
    {
    }

    ~sp_ms_deleter() noexcept
    {
        destroy();
    }

    void operator()( T * ) noexcept
    {
        destroy();
    }

    static void operator_fn( T* ) noexcept // operator() can't be static
    {
    }

    void * address() noexcept
    {
        return storage_.data_;
    }

    void set_initialized() noexcept
    {
        initialized_ = true;
    }
};

template< class T, class A > class sp_as_deleter
{
private:

    typedef typename sp_aligned_storage< sizeof( T ), std::alignment_of< T >::value >::type storage_type;

    storage_type storage_;
    A a_;
    bool initialized_;

private:

    void destroy() noexcept
    {
        if( initialized_ )
        {
            T * p = reinterpret_cast< T* >( storage_.data_ );

            std::allocator_traits<A>::destroy( a_, p );

            initialized_ = false;
        }
    }

public:

    sp_as_deleter( A const & a ) noexcept : a_( a ), initialized_( false )
    {
    }

    // optimization: do not copy storage_
    sp_as_deleter( sp_as_deleter const & r ) noexcept : a_( r.a_), initialized_( false )
    {
    }

    ~sp_as_deleter() noexcept
    {
        destroy();
    }

    void operator()( T * ) noexcept
    {
        destroy();
    }

    static void operator_fn( T* ) noexcept // operator() can't be static
    {
    }

    void * address() noexcept
    {
        return storage_.data_;
    }

    void set_initialized() noexcept
    {
        initialized_ = true;
    }
};

template< class T > struct sp_if_not_array
{
    typedef boost::shared_ptr< T > type;
};

template< class T > struct sp_if_not_array< T[] >
{
};

template< class T, std::size_t N > struct sp_if_not_array< T[N] >
{
};

} // namespace detail

#define BOOST_SP_MSD( T ) boost::detail::sp_inplace_tag< boost::detail::sp_ms_deleter< T > >()

// _noinit versions

template< class T > typename boost::detail::sp_if_not_array< T >::type make_shared_noinit()
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = static_cast<boost::detail::sp_ms_deleter< T > *>( pt._internal_get_untyped_deleter() );

    void * pv = pd->address();

    ::new( pv ) T;
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A > typename boost::detail::sp_if_not_array< T >::type allocate_shared_noinit( A const & a )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ), a );

    boost::detail::sp_ms_deleter< T > * pd = static_cast<boost::detail::sp_ms_deleter< T > *>( pt._internal_get_untyped_deleter() );

    void * pv = pd->address();

    ::new( pv ) T;
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

//

template< class T, class... Args > typename boost::detail::sp_if_not_array< T >::type make_shared( Args && ... args )
{
    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), BOOST_SP_MSD( T ) );

    boost::detail::sp_ms_deleter< T > * pd = static_cast<boost::detail::sp_ms_deleter< T > *>( pt._internal_get_untyped_deleter() );

    void * pv = pd->address();

    ::new( pv ) T( std::forward<Args>( args )... );
    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

template< class T, class A, class... Args > typename boost::detail::sp_if_not_array< T >::type allocate_shared( A const & a, Args && ... args )
{
    typedef typename std::allocator_traits<A>::template rebind_alloc<T> A2;
    A2 a2( a );

    typedef boost::detail::sp_as_deleter< T, A2 > D;

    boost::shared_ptr< T > pt( static_cast< T* >( 0 ), boost::detail::sp_inplace_tag<D>(), a2 );

    D * pd = static_cast< D* >( pt._internal_get_untyped_deleter() );
    void * pv = pd->address();

    std::allocator_traits<A2>::construct( a2, static_cast< T* >( pv ), std::forward<Args>( args )... );

    pd->set_initialized();

    T * pt2 = static_cast< T* >( pv );

    boost::detail::sp_enable_shared_from_this( &pt, pt2, pt2 );
    return boost::shared_ptr< T >( pt, pt2 );
}

#undef BOOST_SP_MSD

} // namespace boost

#endif // #ifndef BOOST_SMART_PTR_MAKE_SHARED_OBJECT_HPP_INCLUDED
