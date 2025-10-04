#ifndef BOOST_SMART_PTR_DETAIL_SHARED_COUNT_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_SHARED_COUNT_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//
//  detail/shared_count.hpp
//
//  Copyright (c) 2001, 2002, 2003 Peter Dimov and Multi Media Ltd.
//  Copyright 2004-2005 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/smart_ptr/bad_weak_ptr.hpp>
#include <boost/smart_ptr/detail/sp_counted_base.hpp>
#include <boost/smart_ptr/detail/sp_counted_impl.hpp>
#include <boost/smart_ptr/detail/sp_disable_deprecated.hpp>
#include <boost/smart_ptr/detail/deprecated_macros.hpp>
#include <boost/core/checked_delete.hpp>
#include <boost/throw_exception.hpp>
#include <boost/core/addressof.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <boost/cstdint.hpp>
#include <memory>            // std::auto_ptr
#include <functional>        // std::less
#include <cstddef>           // std::size_t

#ifdef BOOST_NO_EXCEPTIONS
# include <new>              // std::bad_alloc
#endif

#if defined( BOOST_SP_DISABLE_DEPRECATED )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace boost
{

namespace movelib
{

template< class T, class D > class unique_ptr;

} // namespace movelib

namespace detail
{

#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)

int const shared_count_id = 0x2C35F101;
int const   weak_count_id = 0x298C38A4;

#endif

struct sp_nothrow_tag {};

template< class D > struct sp_inplace_tag
{
};

template< class T > class sp_reference_wrapper
{ 
public:

    explicit sp_reference_wrapper( T & t): t_( boost::addressof( t ) )
    {
    }

    template< class Y > void operator()( Y * p ) const
    {
        (*t_)( p );
    }

private:

    T * t_;
};

template< class D > struct sp_convert_reference
{
    typedef D type;
};

template< class D > struct sp_convert_reference< D& >
{
    typedef sp_reference_wrapper< D > type;
};

template<class T> std::size_t sp_hash_pointer( T* p ) noexcept
{
    boost::uintptr_t v = reinterpret_cast<boost::uintptr_t>( p );

    // match boost::hash<T*>
    return static_cast<std::size_t>( v + ( v >> 3 ) );
}

class weak_count;

class shared_count
{
private:

    sp_counted_base * pi_;

#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
    int id_;
#endif

    friend class weak_count;

public:

    constexpr shared_count() noexcept: pi_(0)
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
    }

    constexpr explicit shared_count( sp_counted_base * pi ) noexcept: pi_( pi )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
    }

    template<class Y> explicit shared_count( Y * p ): pi_( 0 )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
#ifndef BOOST_NO_EXCEPTIONS

        try
        {
            pi_ = new sp_counted_impl_p<Y>( p );
        }
        catch(...)
        {
            boost::checked_delete( p );
            throw;
        }

#else

        pi_ = new sp_counted_impl_p<Y>( p );

        if( pi_ == 0 )
        {
            boost::checked_delete( p );
            boost::throw_exception( std::bad_alloc() );
        }

#endif
    }

    template<class P, class D> shared_count( P p, D d ): pi_(0)
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
#ifndef BOOST_NO_EXCEPTIONS

        try
        {
            pi_ = new sp_counted_impl_pd<P, D>(p, d);
        }
        catch(...)
        {
            d(p); // delete p
            throw;
        }

#else

        pi_ = new sp_counted_impl_pd<P, D>(p, d);

        if(pi_ == 0)
        {
            d(p); // delete p
            boost::throw_exception(std::bad_alloc());
        }

#endif
    }

    template< class P, class D > shared_count( P p, sp_inplace_tag<D> ): pi_( 0 )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
#ifndef BOOST_NO_EXCEPTIONS

        try
        {
            pi_ = new sp_counted_impl_pd< P, D >( p );
        }
        catch( ... )
        {
            D::operator_fn( p ); // delete p
            throw;
        }

#else

        pi_ = new sp_counted_impl_pd< P, D >( p );

        if( pi_ == 0 )
        {
            D::operator_fn( p ); // delete p
            boost::throw_exception( std::bad_alloc() );
        }

#endif // #ifndef BOOST_NO_EXCEPTIONS
    }

    template<class P, class D, class A> shared_count( P p, D d, A a ): pi_( 0 )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        typedef sp_counted_impl_pda<P, D, A> impl_type;

        typedef typename std::allocator_traits<A>::template rebind_alloc< impl_type > A2;

        A2 a2( a );

#ifndef BOOST_NO_EXCEPTIONS

        try
        {
            pi_ = a2.allocate( 1 );
            ::new( static_cast< void* >( pi_ ) ) impl_type( p, d, a );
        }
        catch(...)
        {
            d( p );

            if( pi_ != 0 )
            {
                a2.deallocate( static_cast< impl_type* >( pi_ ), 1 );
            }

            throw;
        }

#else

        pi_ = a2.allocate( 1 );

        if( pi_ != 0 )
        {
            ::new( static_cast< void* >( pi_ ) ) impl_type( p, d, a );
        }
        else
        {
            d( p );
            boost::throw_exception( std::bad_alloc() );
        }

#endif
    }

    template< class P, class D, class A > shared_count( P p, sp_inplace_tag< D >, A a ): pi_( 0 )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        typedef sp_counted_impl_pda< P, D, A > impl_type;

        typedef typename std::allocator_traits<A>::template rebind_alloc< impl_type > A2;

        A2 a2( a );

#ifndef BOOST_NO_EXCEPTIONS

        try
        {
            pi_ = a2.allocate( 1 );
            ::new( static_cast< void* >( pi_ ) ) impl_type( p, a );
        }
        catch(...)
        {
            D::operator_fn( p );

            if( pi_ != 0 )
            {
                a2.deallocate( static_cast< impl_type* >( pi_ ), 1 );
            }

            throw;
        }

#else

        pi_ = a2.allocate( 1 );

        if( pi_ != 0 )
        {
            ::new( static_cast< void* >( pi_ ) ) impl_type( p, a );
        }
        else
        {
            D::operator_fn( p );
            boost::throw_exception( std::bad_alloc() );
        }

#endif // #ifndef BOOST_NO_EXCEPTIONS
    }

#ifndef BOOST_NO_AUTO_PTR

    // auto_ptr<Y> is special cased to provide the strong guarantee

    template<class Y>
    explicit shared_count( std::auto_ptr<Y> & r ): pi_( new sp_counted_impl_p<Y>( r.get() ) )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
#ifdef BOOST_NO_EXCEPTIONS

        if( pi_ == 0 )
        {
            boost::throw_exception(std::bad_alloc());
        }

#endif

        r.release();
    }

#endif 

    template<class Y, class D>
    explicit shared_count( std::unique_ptr<Y, D> & r ): pi_( 0 )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        typedef typename sp_convert_reference<D>::type D2;

        D2 d2( static_cast<D&&>( r.get_deleter() ) );
        pi_ = new sp_counted_impl_pd< typename std::unique_ptr<Y, D>::pointer, D2 >( r.get(), d2 );

#ifdef BOOST_NO_EXCEPTIONS

        if( pi_ == 0 )
        {
            boost::throw_exception( std::bad_alloc() );
        }

#endif

        r.release();
    }

    template<class Y, class D>
    explicit shared_count( boost::movelib::unique_ptr<Y, D> & r ): pi_( 0 )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        typedef typename sp_convert_reference<D>::type D2;

        D2 d2( r.get_deleter() );
        pi_ = new sp_counted_impl_pd< typename boost::movelib::unique_ptr<Y, D>::pointer, D2 >( r.get(), d2 );

#ifdef BOOST_NO_EXCEPTIONS

        if( pi_ == 0 )
        {
            boost::throw_exception( std::bad_alloc() );
        }

#endif

        r.release();
    }

    ~shared_count() /*noexcept*/
    {
        if( pi_ != 0 ) pi_->release();
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        id_ = 0;
#endif
    }

    shared_count(shared_count const & r) noexcept: pi_(r.pi_)
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        if( pi_ != 0 ) pi_->add_ref_copy();
    }

    shared_count(shared_count && r) noexcept: pi_(r.pi_)
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        r.pi_ = 0;
    }

    explicit shared_count(weak_count const & r); // throws bad_weak_ptr when r.use_count() == 0
    shared_count( weak_count const & r, sp_nothrow_tag ) noexcept; // constructs an empty *this when r.use_count() == 0

    shared_count & operator= (shared_count const & r) noexcept
    {
        sp_counted_base * tmp = r.pi_;

        if( tmp != pi_ )
        {
            if( tmp != 0 ) tmp->add_ref_copy();
            if( pi_ != 0 ) pi_->release();
            pi_ = tmp;
        }

        return *this;
    }

    void swap(shared_count & r) noexcept
    {
        sp_counted_base * tmp = r.pi_;
        r.pi_ = pi_;
        pi_ = tmp;
    }

    long use_count() const noexcept
    {
        return pi_ != 0? pi_->use_count(): 0;
    }

    bool unique() const noexcept
    {
        return use_count() == 1;
    }

    bool empty() const noexcept
    {
        return pi_ == 0;
    }

    bool operator==( shared_count const & r ) const noexcept
    {
        return pi_ == r.pi_;
    }

    bool operator==( weak_count const & r ) const noexcept;

    bool operator<( shared_count const & r ) const noexcept
    {
        return std::less<sp_counted_base *>()( pi_, r.pi_ );
    }

    bool operator<( weak_count const & r ) const noexcept;

    void * get_deleter( sp_typeinfo_ const & ti ) const noexcept
    {
        return pi_? pi_->get_deleter( ti ): 0;
    }

    void * get_local_deleter( sp_typeinfo_ const & ti ) const noexcept
    {
        return pi_? pi_->get_local_deleter( ti ): 0;
    }

    void * get_untyped_deleter() const noexcept
    {
        return pi_? pi_->get_untyped_deleter(): 0;
    }

    std::size_t hash_value() const noexcept
    {
        return sp_hash_pointer( pi_ );
    }
};


class weak_count
{
private:

    sp_counted_base * pi_;

#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
    int id_;
#endif

    friend class shared_count;

public:

    constexpr weak_count() noexcept: pi_(0)
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(weak_count_id)
#endif
    {
    }

    weak_count(shared_count const & r) noexcept: pi_(r.pi_)
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(weak_count_id)
#endif
    {
        if(pi_ != 0) pi_->weak_add_ref();
    }

    weak_count(weak_count const & r) noexcept: pi_(r.pi_)
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(weak_count_id)
#endif
    {
        if(pi_ != 0) pi_->weak_add_ref();
    }

// Move support

    weak_count(weak_count && r) noexcept: pi_(r.pi_)
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(weak_count_id)
#endif
    {
        r.pi_ = 0;
    }

    ~weak_count() /*noexcept*/
    {
        if(pi_ != 0) pi_->weak_release();
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        id_ = 0;
#endif
    }

    weak_count & operator= (shared_count const & r) noexcept
    {
        sp_counted_base * tmp = r.pi_;

        if( tmp != pi_ )
        {
            if(tmp != 0) tmp->weak_add_ref();
            if(pi_ != 0) pi_->weak_release();
            pi_ = tmp;
        }

        return *this;
    }

    weak_count & operator= (weak_count const & r) noexcept
    {
        sp_counted_base * tmp = r.pi_;

        if( tmp != pi_ )
        {
            if(tmp != 0) tmp->weak_add_ref();
            if(pi_ != 0) pi_->weak_release();
            pi_ = tmp;
        }

        return *this;
    }

    void swap(weak_count & r) noexcept
    {
        sp_counted_base * tmp = r.pi_;
        r.pi_ = pi_;
        pi_ = tmp;
    }

    long use_count() const noexcept
    {
        return pi_ != 0? pi_->use_count(): 0;
    }

    bool empty() const noexcept
    {
        return pi_ == 0;
    }

    bool operator==( weak_count const & r ) const noexcept
    {
        return pi_ == r.pi_;
    }

    bool operator==( shared_count const & r ) const noexcept
    {
        return pi_ == r.pi_;
    }

    bool operator<( weak_count const & r ) const noexcept
    {
        return std::less<sp_counted_base *>()( pi_, r.pi_ );
    }

    bool operator<( shared_count const & r ) const noexcept
    {
        return std::less<sp_counted_base *>()( pi_, r.pi_ );
    }

    std::size_t hash_value() const noexcept
    {
        return sp_hash_pointer( pi_ );
    }
};

inline shared_count::shared_count( weak_count const & r ): pi_( r.pi_ )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
{
    if( pi_ == 0 || !pi_->add_ref_lock() )
    {
        boost::throw_exception( boost::bad_weak_ptr() );
    }
}

inline shared_count::shared_count( weak_count const & r, sp_nothrow_tag ) noexcept: pi_( r.pi_ )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
{
    if( pi_ != 0 && !pi_->add_ref_lock() )
    {
        pi_ = 0;
    }
}

inline bool shared_count::operator==( weak_count const & r ) const noexcept
{
    return pi_ == r.pi_;
}

inline bool shared_count::operator<( weak_count const & r ) const noexcept
{
    return std::less<sp_counted_base *>()( pi_, r.pi_ );
}

} // namespace detail

} // namespace boost

#if defined( BOOST_SP_DISABLE_DEPRECATED )
#pragma GCC diagnostic pop
#endif

#endif  // #ifndef BOOST_SMART_PTR_DETAIL_SHARED_COUNT_HPP_INCLUDED
