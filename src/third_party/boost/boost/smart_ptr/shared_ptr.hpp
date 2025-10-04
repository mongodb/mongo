#ifndef BOOST_SMART_PTR_SHARED_PTR_HPP_INCLUDED
#define BOOST_SMART_PTR_SHARED_PTR_HPP_INCLUDED

//
//  shared_ptr.hpp
//
//  (C) Copyright Greg Colvin and Beman Dawes 1998, 1999.
//  Copyright (c) 2001-2008 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  See http://www.boost.org/libs/smart_ptr/ for documentation.
//

#include <boost/smart_ptr/detail/shared_count.hpp>
#include <boost/smart_ptr/detail/sp_convertible.hpp>
#include <boost/smart_ptr/detail/sp_disable_deprecated.hpp>
#include <boost/smart_ptr/detail/sp_noexcept.hpp>
#include <boost/core/checked_delete.hpp>
#include <boost/throw_exception.hpp>
#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>

#if !defined(BOOST_SP_NO_ATOMIC_ACCESS)
#include <boost/smart_ptr/detail/spinlock_pool.hpp>
#endif

#include <algorithm>            // for std::swap
#include <functional>           // for std::less
#include <typeinfo>             // for std::bad_cast
#include <cstddef>              // for std::size_t
#include <memory>               // for std::auto_ptr
#include <iosfwd>               // for std::basic_ostream

#if defined( BOOST_SP_DISABLE_DEPRECATED )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace boost
{

template<class T> class shared_ptr;
template<class T> class weak_ptr;
template<class T> class enable_shared_from_this;
class enable_shared_from_raw;

namespace movelib
{

    template< class T, class D > class unique_ptr;

} // namespace movelib

namespace detail
{

// sp_element, element_type

template< class T > struct sp_element
{
    typedef T type;
};

template< class T > struct sp_element< T[] >
{
    typedef T type;
};

template< class T, std::size_t N > struct sp_element< T[N] >
{
    typedef T type;
};

// sp_dereference, return type of operator*

template< class T > struct sp_dereference
{
    typedef T & type;
};

template<> struct sp_dereference< void >
{
    typedef void type;
};

template<> struct sp_dereference< void const >
{
    typedef void type;
};

template<> struct sp_dereference< void volatile >
{
    typedef void type;
};

template<> struct sp_dereference< void const volatile >
{
    typedef void type;
};

template< class T > struct sp_dereference< T[] >
{
    typedef void type;
};

template< class T, std::size_t N > struct sp_dereference< T[N] >
{
    typedef void type;
};

// sp_member_access, return type of operator->

template< class T > struct sp_member_access
{
    typedef T * type;
};

template< class T > struct sp_member_access< T[] >
{
    typedef void type;
};

template< class T, std::size_t N > struct sp_member_access< T[N] >
{
    typedef void type;
};

// sp_array_access, return type of operator[]

template< class T > struct sp_array_access
{
    typedef void type;
};

template< class T > struct sp_array_access< T[] >
{
    typedef T & type;
};

template< class T, std::size_t N > struct sp_array_access< T[N] >
{
    typedef T & type;
};

// sp_extent, for operator[] index check

template< class T > struct sp_extent
{
    enum _vt { value = 0 };
};

template< class T, std::size_t N > struct sp_extent< T[N] >
{
    enum _vt { value = N };
};

// enable_shared_from_this support

template< class X, class Y, class T > inline void sp_enable_shared_from_this( boost::shared_ptr<X> const * ppx, Y const * py, boost::enable_shared_from_this< T > const * pe )
{
    if( pe != 0 )
    {
        pe->_internal_accept_owner( ppx, const_cast< Y* >( py ) );
    }
}

template< class X, class Y > inline void sp_enable_shared_from_this( boost::shared_ptr<X> * ppx, Y const * py, boost::enable_shared_from_raw const * pe );

#ifdef _MANAGED

// Avoid C4793, ... causes native code generation

struct sp_any_pointer
{
    template<class T> sp_any_pointer( T* ) {}
};

inline void sp_enable_shared_from_this( sp_any_pointer, sp_any_pointer, sp_any_pointer )
{
}

#else // _MANAGED

inline void sp_enable_shared_from_this( ... )
{
}

#endif // _MANAGED

// sp_assert_convertible

template< class Y, class T > inline void sp_assert_convertible() noexcept
{
    static_assert( sp_convertible< Y, T >::value, "incompatible pointer type" );
}

// pointer constructor helper

template< class T, class Y > inline void sp_pointer_construct( boost::shared_ptr< T > * ppx, Y * p, boost::detail::shared_count & pn )
{
    boost::detail::shared_count( p ).swap( pn );
    boost::detail::sp_enable_shared_from_this( ppx, p, p );
}

template< class T, class Y > inline void sp_pointer_construct( boost::shared_ptr< T[] > * /*ppx*/, Y * p, boost::detail::shared_count & pn )
{
    sp_assert_convertible< Y[], T[] >();
    boost::detail::shared_count( p, boost::checked_array_deleter< T >() ).swap( pn );
}

template< class T, std::size_t N, class Y > inline void sp_pointer_construct( boost::shared_ptr< T[N] > * /*ppx*/, Y * p, boost::detail::shared_count & pn )
{
    sp_assert_convertible< Y[N], T[N] >();
    boost::detail::shared_count( p, boost::checked_array_deleter< T >() ).swap( pn );
}

// deleter constructor helper

template< class T, class Y > inline void sp_deleter_construct( boost::shared_ptr< T > * ppx, Y * p )
{
    boost::detail::sp_enable_shared_from_this( ppx, p, p );
}

template< class T, class Y > inline void sp_deleter_construct( boost::shared_ptr< T[] > * /*ppx*/, Y * /*p*/ )
{
    sp_assert_convertible< Y[], T[] >();
}

template< class T, std::size_t N, class Y > inline void sp_deleter_construct( boost::shared_ptr< T[N] > * /*ppx*/, Y * /*p*/ )
{
    sp_assert_convertible< Y[N], T[N] >();
}

struct sp_internal_constructor_tag
{
};

} // namespace detail


//
//  shared_ptr
//
//  An enhanced relative of scoped_ptr with reference counted copy semantics.
//  The object pointed to is deleted when the last shared_ptr pointing to it
//  is destroyed or reset.
//

template<class T> class shared_ptr
{
private:

    // Borland 5.5.1 specific workaround
    typedef shared_ptr<T> this_type;

public:

    typedef typename boost::detail::sp_element< T >::type element_type;

    constexpr shared_ptr() noexcept : px( 0 ), pn()
    {
    }

    constexpr shared_ptr( std::nullptr_t ) noexcept : px( 0 ), pn()
    {
    }

    constexpr shared_ptr( boost::detail::sp_internal_constructor_tag, element_type * px_, boost::detail::shared_count const & pn_ ) noexcept : px( px_ ), pn( pn_ )
    {
    }

    constexpr shared_ptr( boost::detail::sp_internal_constructor_tag, element_type * px_, boost::detail::shared_count && pn_ ) noexcept : px( px_ ), pn( std::move( pn_ ) )
    {
    }

    template<class Y>
    explicit shared_ptr( Y * p ): px( p ), pn() // Y must be complete
    {
        boost::detail::sp_pointer_construct( this, p, pn );
    }

    //
    // Requirements: D's copy/move constructors must not throw
    //
    // shared_ptr will release p by calling d(p)
    //

    template<class Y, class D> shared_ptr( Y * p, D d ): px( p ), pn( p, static_cast< D&& >( d ) )
    {
        boost::detail::sp_deleter_construct( this, p );
    }

    template<class D> shared_ptr( std::nullptr_t p, D d ): px( p ), pn( p, static_cast< D&& >( d ) )
    {
    }

    // As above, but with allocator. A's copy constructor shall not throw.

    template<class Y, class D, class A> shared_ptr( Y * p, D d, A a ): px( p ), pn( p, static_cast< D&& >( d ), a )
    {
        boost::detail::sp_deleter_construct( this, p );
    }

    template<class D, class A> shared_ptr( std::nullptr_t p, D d, A a ): px( p ), pn( p, static_cast< D&& >( d ), a )
    {
    }

//  generated copy constructor, destructor are fine...
// ... except in C++0x, move disables the implicit copy

    shared_ptr( shared_ptr const & r ) noexcept : px( r.px ), pn( r.pn )
    {
    }

    template<class Y>
    explicit shared_ptr( weak_ptr<Y> const & r ): pn( r.pn ) // may throw
    {
        boost::detail::sp_assert_convertible< Y, T >();

        // it is now safe to copy r.px, as pn(r.pn) did not throw
        px = r.px;
    }

    template<class Y>
    shared_ptr( weak_ptr<Y> const & r, boost::detail::sp_nothrow_tag )
    noexcept : px( 0 ), pn( r.pn, boost::detail::sp_nothrow_tag() )
    {
        if( !pn.empty() )
        {
            px = r.px;
        }
    }

    template<class Y>
    shared_ptr( shared_ptr<Y> const & r, typename boost::detail::sp_enable_if_convertible<Y,T>::type = boost::detail::sp_empty() )
    noexcept : px( r.px ), pn( r.pn )
    {
        boost::detail::sp_assert_convertible< Y, T >();
    }

    // aliasing
    template< class Y >
    shared_ptr( shared_ptr<Y> const & r, element_type * p ) noexcept : px( p ), pn( r.pn )
    {
    }

#ifndef BOOST_NO_AUTO_PTR

    template<class Y>
    explicit shared_ptr( std::auto_ptr<Y> & r ): px(r.get()), pn()
    {
        boost::detail::sp_assert_convertible< Y, T >();

        Y * tmp = r.get();
        pn = boost::detail::shared_count( r );

        boost::detail::sp_deleter_construct( this, tmp );
    }

    template<class Y>
    shared_ptr( std::auto_ptr<Y> && r ): px(r.get()), pn()
    {
        boost::detail::sp_assert_convertible< Y, T >();

        Y * tmp = r.get();
        pn = boost::detail::shared_count( r );

        boost::detail::sp_deleter_construct( this, tmp );
    }

#endif // BOOST_NO_AUTO_PTR

    template< class Y, class D >
    shared_ptr( std::unique_ptr< Y, D > && r ): px( r.get() ), pn()
    {
        boost::detail::sp_assert_convertible< Y, T >();

        typename std::unique_ptr< Y, D >::pointer tmp = r.get();

        if( tmp != 0 )
        {
            pn = boost::detail::shared_count( r );
            boost::detail::sp_deleter_construct( this, tmp );
        }
    }

    template< class Y, class D >
    shared_ptr( boost::movelib::unique_ptr< Y, D > r ): px( r.get() ), pn()
    {
        boost::detail::sp_assert_convertible< Y, T >();

        typename boost::movelib::unique_ptr< Y, D >::pointer tmp = r.get();

        if( tmp != 0 )
        {
            pn = boost::detail::shared_count( r );
            boost::detail::sp_deleter_construct( this, tmp );
        }
    }

    // assignment

    shared_ptr & operator=( shared_ptr const & r ) noexcept
    {
        this_type(r).swap(*this);
        return *this;
    }

    template<class Y>
    shared_ptr & operator=(shared_ptr<Y> const & r) noexcept
    {
        this_type(r).swap(*this);
        return *this;
    }

#ifndef BOOST_NO_AUTO_PTR

    template<class Y>
    shared_ptr & operator=( std::auto_ptr<Y> & r )
    {
        this_type( r ).swap( *this );
        return *this;
    }

    template<class Y>
    shared_ptr & operator=( std::auto_ptr<Y> && r )
    {
        this_type( static_cast< std::auto_ptr<Y> && >( r ) ).swap( *this );
        return *this;
    }

#endif // BOOST_NO_AUTO_PTR

    template<class Y, class D>
    shared_ptr & operator=( std::unique_ptr<Y, D> && r )
    {
        this_type( static_cast< std::unique_ptr<Y, D> && >( r ) ).swap(*this);
        return *this;
    }

    template<class Y, class D>
    shared_ptr & operator=( boost::movelib::unique_ptr<Y, D> r )
    {
        // this_type( static_cast< unique_ptr<Y, D> && >( r ) ).swap( *this );

        boost::detail::sp_assert_convertible< Y, T >();

        typename boost::movelib::unique_ptr< Y, D >::pointer p = r.get();

        shared_ptr tmp;

        if( p != 0 )
        {
            tmp.px = p;
            tmp.pn = boost::detail::shared_count( r );

            boost::detail::sp_deleter_construct( &tmp, p );
        }

        tmp.swap( *this );

        return *this;
    }

// Move support

    shared_ptr( shared_ptr && r ) noexcept : px( r.px ), pn( static_cast< boost::detail::shared_count && >( r.pn ) )
    {
        r.px = 0;
    }

    template<class Y>
    shared_ptr( shared_ptr<Y> && r, typename boost::detail::sp_enable_if_convertible<Y,T>::type = boost::detail::sp_empty() )
    noexcept : px( r.px ), pn( static_cast< boost::detail::shared_count && >( r.pn ) )
    {
        boost::detail::sp_assert_convertible< Y, T >();
        r.px = 0;
    }

    shared_ptr & operator=( shared_ptr && r ) noexcept
    {
        this_type( static_cast< shared_ptr && >( r ) ).swap( *this );
        return *this;
    }

    template<class Y>
    shared_ptr & operator=( shared_ptr<Y> && r ) noexcept
    {
        this_type( static_cast< shared_ptr<Y> && >( r ) ).swap( *this );
        return *this;
    }

    // aliasing move
    template<class Y>
    shared_ptr( shared_ptr<Y> && r, element_type * p ) noexcept : px( p ), pn()
    {
        pn.swap( r.pn );
        r.px = 0;
    }

    shared_ptr & operator=( std::nullptr_t ) noexcept
    {
        this_type().swap(*this);
        return *this;
    }

    void reset() noexcept
    {
        this_type().swap(*this);
    }

    template<class Y> void reset( Y * p ) // Y must be complete
    {
        BOOST_ASSERT( p == 0 || p != px ); // catch self-reset errors
        this_type( p ).swap( *this );
    }

    template<class Y, class D> void reset( Y * p, D d )
    {
        this_type( p, static_cast< D&& >( d ) ).swap( *this );
    }

    template<class Y, class D, class A> void reset( Y * p, D d, A a )
    {
        this_type( p, static_cast< D&& >( d ), a ).swap( *this );
    }

    template<class Y> void reset( shared_ptr<Y> const & r, element_type * p ) noexcept
    {
        this_type( r, p ).swap( *this );
    }

    template<class Y> void reset( shared_ptr<Y> && r, element_type * p ) noexcept
    {
        this_type( static_cast< shared_ptr<Y> && >( r ), p ).swap( *this );
    }

    typename boost::detail::sp_dereference< T >::type operator* () const BOOST_SP_NOEXCEPT_WITH_ASSERT
    {
        BOOST_ASSERT( px != 0 );
        return *px;
    }
    
    typename boost::detail::sp_member_access< T >::type operator-> () const BOOST_SP_NOEXCEPT_WITH_ASSERT
    {
        BOOST_ASSERT( px != 0 );
        return px;
    }
    
    typename boost::detail::sp_array_access< T >::type operator[] ( std::ptrdiff_t i ) const BOOST_SP_NOEXCEPT_WITH_ASSERT
    {
        BOOST_ASSERT( px != 0 );
        BOOST_ASSERT( i >= 0 && ( i < boost::detail::sp_extent< T >::value || boost::detail::sp_extent< T >::value == 0 ) );

        return static_cast< typename boost::detail::sp_array_access< T >::type >( px[ i ] );
    }

    element_type * get() const noexcept
    {
        return px;
    }

    explicit operator bool () const noexcept
    {
        return px != 0;
    }

    bool unique() const noexcept
    {
        return pn.unique();
    }

    long use_count() const noexcept
    {
        return pn.use_count();
    }

    void swap( shared_ptr & other ) noexcept
    {
        std::swap(px, other.px);
        pn.swap(other.pn);
    }

    template<class Y> bool owner_before( shared_ptr<Y> const & rhs ) const noexcept
    {
        return pn < rhs.pn;
    }

    template<class Y> bool owner_before( weak_ptr<Y> const & rhs ) const noexcept
    {
        return pn < rhs.pn;
    }

    template<class Y> bool owner_equals( shared_ptr<Y> const & rhs ) const noexcept
    {
        return pn == rhs.pn;
    }

    template<class Y> bool owner_equals( weak_ptr<Y> const & rhs ) const noexcept
    {
        return pn == rhs.pn;
    }

    std::size_t owner_hash_value() const noexcept
    {
        return pn.hash_value();
    }

    void * _internal_get_deleter( boost::detail::sp_typeinfo_ const & ti ) const noexcept
    {
        return pn.get_deleter( ti );
    }

    void * _internal_get_local_deleter( boost::detail::sp_typeinfo_ const & ti ) const noexcept
    {
        return pn.get_local_deleter( ti );
    }

    void * _internal_get_untyped_deleter() const noexcept
    {
        return pn.get_untyped_deleter();
    }

    bool _internal_equiv( shared_ptr const & r ) const noexcept
    {
        return px == r.px && pn == r.pn;
    }

    boost::detail::shared_count _internal_count() const noexcept
    {
        return pn;
    }

private:

    template<class Y> friend class shared_ptr;
    template<class Y> friend class weak_ptr;


    element_type * px;                 // contained pointer
    boost::detail::shared_count pn;    // reference counter

};  // shared_ptr

template<class T, class U> inline bool operator==(shared_ptr<T> const & a, shared_ptr<U> const & b) noexcept
{
    return a.get() == b.get();
}

template<class T, class U> inline bool operator!=(shared_ptr<T> const & a, shared_ptr<U> const & b) noexcept
{
    return a.get() != b.get();
}

template<class T> inline bool operator==( shared_ptr<T> const & p, std::nullptr_t ) noexcept
{
    return p.get() == 0;
}

template<class T> inline bool operator==( std::nullptr_t, shared_ptr<T> const & p ) noexcept
{
    return p.get() == 0;
}

template<class T> inline bool operator!=( shared_ptr<T> const & p, std::nullptr_t ) noexcept
{
    return p.get() != 0;
}

template<class T> inline bool operator!=( std::nullptr_t, shared_ptr<T> const & p ) noexcept
{
    return p.get() != 0;
}

template<class T, class U> inline bool operator<(shared_ptr<T> const & a, shared_ptr<U> const & b) noexcept
{
    return a.owner_before( b );
}

template<class T> inline void swap(shared_ptr<T> & a, shared_ptr<T> & b) noexcept
{
    a.swap(b);
}

template<class T, class U> shared_ptr<T> static_pointer_cast( shared_ptr<U> const & r ) noexcept
{
    (void) static_cast< T* >( static_cast< U* >( 0 ) );

    typedef typename shared_ptr<T>::element_type E;

    E * p = static_cast< E* >( r.get() );
    return shared_ptr<T>( r, p );
}

template<class T, class U> shared_ptr<T> const_pointer_cast( shared_ptr<U> const & r ) noexcept
{
    (void) const_cast< T* >( static_cast< U* >( 0 ) );

    typedef typename shared_ptr<T>::element_type E;

    E * p = const_cast< E* >( r.get() );
    return shared_ptr<T>( r, p );
}

template<class T, class U> shared_ptr<T> dynamic_pointer_cast( shared_ptr<U> const & r ) noexcept
{
    (void) dynamic_cast< T* >( static_cast< U* >( 0 ) );

    typedef typename shared_ptr<T>::element_type E;

    E * p = dynamic_cast< E* >( r.get() );
    return p? shared_ptr<T>( r, p ): shared_ptr<T>();
}

template<class T, class U> shared_ptr<T> reinterpret_pointer_cast( shared_ptr<U> const & r ) noexcept
{
    (void) reinterpret_cast< T* >( static_cast< U* >( 0 ) );

    typedef typename shared_ptr<T>::element_type E;

    E * p = reinterpret_cast< E* >( r.get() );
    return shared_ptr<T>( r, p );
}

template<class T, class U> shared_ptr<T> static_pointer_cast( shared_ptr<U> && r ) noexcept
{
    (void) static_cast< T* >( static_cast< U* >( 0 ) );

    typedef typename shared_ptr<T>::element_type E;

    E * p = static_cast< E* >( r.get() );
    return shared_ptr<T>( std::move(r), p );
}

template<class T, class U> shared_ptr<T> const_pointer_cast( shared_ptr<U> && r ) noexcept
{
    (void) const_cast< T* >( static_cast< U* >( 0 ) );

    typedef typename shared_ptr<T>::element_type E;

    E * p = const_cast< E* >( r.get() );
    return shared_ptr<T>( std::move(r), p );
}

template<class T, class U> shared_ptr<T> dynamic_pointer_cast( shared_ptr<U> && r ) noexcept
{
    (void) dynamic_cast< T* >( static_cast< U* >( 0 ) );

    typedef typename shared_ptr<T>::element_type E;

    E * p = dynamic_cast< E* >( r.get() );
    return p? shared_ptr<T>( std::move(r), p ): shared_ptr<T>();
}

template<class T, class U> shared_ptr<T> reinterpret_pointer_cast( shared_ptr<U> && r ) noexcept
{
    (void) reinterpret_cast< T* >( static_cast< U* >( 0 ) );

    typedef typename shared_ptr<T>::element_type E;

    E * p = reinterpret_cast< E* >( r.get() );
    return shared_ptr<T>( std::move(r), p );
}

// get_pointer() enables boost::mem_fn to recognize shared_ptr

template<class T> inline typename shared_ptr<T>::element_type * get_pointer(shared_ptr<T> const & p) noexcept
{
    return p.get();
}

// operator<<

template<class E, class T, class Y> std::basic_ostream<E, T> & operator<< (std::basic_ostream<E, T> & os, shared_ptr<Y> const & p)
{
    os << p.get();
    return os;
}

// get_deleter

namespace detail
{

template<class D, class T> D * basic_get_deleter( shared_ptr<T> const & p ) noexcept
{
    return static_cast<D *>( p._internal_get_deleter(BOOST_SP_TYPEID_(D)) );
}

template<class D, class T> D * basic_get_local_deleter( D *, shared_ptr<T> const & p ) noexcept;
template<class D, class T> D const * basic_get_local_deleter( D const *, shared_ptr<T> const & p ) noexcept;

class esft2_deleter_wrapper
{
private:

    shared_ptr<void const volatile> deleter_;

public:

    esft2_deleter_wrapper() noexcept
    {
    }

    template< class T > void set_deleter( shared_ptr<T> const & deleter ) noexcept
    {
        deleter_ = deleter;
    }

    template<typename D> D* get_deleter() const noexcept
    {
        return boost::detail::basic_get_deleter<D>( deleter_ );
    }

    template< class T> void operator()( T* ) BOOST_SP_NOEXCEPT_WITH_ASSERT
    {
        BOOST_ASSERT( deleter_.use_count() <= 1 );
        deleter_.reset();
    }
};

} // namespace detail

template<class D, class T> D * get_deleter( shared_ptr<T> const & p ) noexcept
{
    D * d = boost::detail::basic_get_deleter<D>( p );

    if( d == 0 )
    {
        d = boost::detail::basic_get_local_deleter( d, p );
    }

    if( d == 0 )
    {
        boost::detail::esft2_deleter_wrapper *del_wrapper = boost::detail::basic_get_deleter<boost::detail::esft2_deleter_wrapper>(p);
// The following get_deleter method call is fully qualified because
// older versions of gcc (2.95, 3.2.3) fail to compile it when written del_wrapper->get_deleter<D>()
        if(del_wrapper) d = del_wrapper->::boost::detail::esft2_deleter_wrapper::get_deleter<D>();
    }

    return d;
}

// atomic access

#if !defined(BOOST_SP_NO_ATOMIC_ACCESS)

template<class T> inline bool atomic_is_lock_free( shared_ptr<T> const * /*p*/ ) noexcept
{
    return false;
}

template<class T> shared_ptr<T> atomic_load( shared_ptr<T> const * p ) noexcept
{
    boost::detail::spinlock_pool<2>::scoped_lock lock( p );
    return *p;
}

template<class T, class M> inline shared_ptr<T> atomic_load_explicit( shared_ptr<T> const * p, /*memory_order mo*/ M ) noexcept
{
    return atomic_load( p );
}

template<class T> void atomic_store( shared_ptr<T> * p, shared_ptr<T> r ) noexcept
{
    boost::detail::spinlock_pool<2>::scoped_lock lock( p );
    p->swap( r );
}

template<class T, class M> inline void atomic_store_explicit( shared_ptr<T> * p, shared_ptr<T> r, /*memory_order mo*/ M ) noexcept
{
    atomic_store( p, r ); // std::move( r )
}

template<class T> shared_ptr<T> atomic_exchange( shared_ptr<T> * p, shared_ptr<T> r ) noexcept
{
    boost::detail::spinlock & sp = boost::detail::spinlock_pool<2>::spinlock_for( p );

    sp.lock();
    p->swap( r );
    sp.unlock();

    return r; // return std::move( r )
}

template<class T, class M> shared_ptr<T> inline atomic_exchange_explicit( shared_ptr<T> * p, shared_ptr<T> r, /*memory_order mo*/ M ) noexcept
{
    return atomic_exchange( p, r ); // std::move( r )
}

template<class T> bool atomic_compare_exchange( shared_ptr<T> * p, shared_ptr<T> * v, shared_ptr<T> w ) noexcept
{
    boost::detail::spinlock & sp = boost::detail::spinlock_pool<2>::spinlock_for( p );

    sp.lock();

    if( p->_internal_equiv( *v ) )
    {
        p->swap( w );

        sp.unlock();

        return true;
    }
    else
    {
        shared_ptr<T> tmp( *p );

        sp.unlock();

        tmp.swap( *v );
        return false;
    }
}

template<class T, class M> inline bool atomic_compare_exchange_explicit( shared_ptr<T> * p, shared_ptr<T> * v, shared_ptr<T> w, /*memory_order success*/ M, /*memory_order failure*/ M ) noexcept
{
    return atomic_compare_exchange( p, v, w ); // std::move( w )
}

#endif // !defined(BOOST_SP_NO_ATOMIC_ACCESS)

// hash_value

template< class T > struct hash;

template< class T > std::size_t hash_value( boost::shared_ptr<T> const & p ) noexcept
{
    return boost::hash< typename boost::shared_ptr<T>::element_type* >()( p.get() );
}

} // namespace boost

// std::hash

namespace std
{

template<class T> struct hash< ::boost::shared_ptr<T> >
{
    std::size_t operator()( ::boost::shared_ptr<T> const & p ) const noexcept
    {
        return std::hash< typename ::boost::shared_ptr<T>::element_type* >()( p.get() );
    }
};

} // namespace std

#include <boost/smart_ptr/detail/local_sp_deleter.hpp>

namespace boost
{

namespace detail
{

template<class D, class T> D * basic_get_local_deleter( D *, shared_ptr<T> const & p ) noexcept
{
    return static_cast<D *>( p._internal_get_local_deleter( BOOST_SP_TYPEID_(local_sp_deleter<D>) ) );
}

template<class D, class T> D const * basic_get_local_deleter( D const *, shared_ptr<T> const & p ) noexcept
{
    return static_cast<D *>( p._internal_get_local_deleter( BOOST_SP_TYPEID_(local_sp_deleter<D>) ) );
}

} // namespace detail

#if defined(__cpp_deduction_guides)

template<class T> shared_ptr( weak_ptr<T> ) -> shared_ptr<T>;
template<class T, class D> shared_ptr( std::unique_ptr<T, D> ) -> shared_ptr<T>;

#endif

} // namespace boost

#if defined( BOOST_SP_DISABLE_DEPRECATED )
#pragma GCC diagnostic pop
#endif

#endif  // #ifndef BOOST_SMART_PTR_SHARED_PTR_HPP_INCLUDED
