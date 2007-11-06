#ifndef BOOST_DETAIL_SHARED_COUNT_HPP_INCLUDED
#define BOOST_DETAIL_SHARED_COUNT_HPP_INCLUDED

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

#ifdef __BORLANDC__
# pragma warn -8027     // Functions containing try are not expanded inline
#endif

#include <boost/config.hpp>
#include <boost/checked_delete.hpp>
#include <boost/throw_exception.hpp>
#include <boost/detail/bad_weak_ptr.hpp>
#include <boost/detail/sp_counted_base.hpp>
#include <boost/detail/sp_counted_impl.hpp>

#include <memory>           // std::auto_ptr
#include <functional>       // std::less
#include <new>              // std::bad_alloc
#include <typeinfo>         // std::type_info in get_deleter

namespace boost
{

namespace detail
{

#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)

int const shared_count_id = 0x2C35F101;
int const   weak_count_id = 0x298C38A4;

#endif

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

    shared_count(): pi_(0) // nothrow
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

    template<class P, class D> shared_count(P p, D d): pi_(0)
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

    template<class P, class D, class A> shared_count( P p, D d, A a ): pi_( 0 )
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        typedef sp_counted_impl_pda<P, D, A> impl_type;
        typedef typename A::template rebind< impl_type >::other A2;

        A2 a2( a );

#ifndef BOOST_NO_EXCEPTIONS

        try
        {
            pi_ = a2.allocate( 1, static_cast< impl_type* >( 0 ) );
            new( static_cast< void* >( pi_ ) ) impl_type( p, d, a );
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

        pi_ = a2.allocate( 1, static_cast< impl_type* >( 0 ) );

        if( pi_ != 0 )
        {
            new( static_cast< void* >( pi_ ) ) impl_type( p, d, a );
        }
        else
        {
            d( p );
            boost::throw_exception( std::bad_alloc() );
        }

#endif
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

    ~shared_count() // nothrow
    {
        if( pi_ != 0 ) pi_->release();
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        id_ = 0;
#endif
    }

    shared_count(shared_count const & r): pi_(r.pi_) // nothrow
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        if( pi_ != 0 ) pi_->add_ref_copy();
    }

    explicit shared_count(weak_count const & r); // throws bad_weak_ptr when r.use_count() == 0

    shared_count & operator= (shared_count const & r) // nothrow
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

    void swap(shared_count & r) // nothrow
    {
        sp_counted_base * tmp = r.pi_;
        r.pi_ = pi_;
        pi_ = tmp;
    }

    long use_count() const // nothrow
    {
        return pi_ != 0? pi_->use_count(): 0;
    }

    bool unique() const // nothrow
    {
        return use_count() == 1;
    }

    friend inline bool operator==(shared_count const & a, shared_count const & b)
    {
        return a.pi_ == b.pi_;
    }

    friend inline bool operator<(shared_count const & a, shared_count const & b)
    {
        return std::less<sp_counted_base *>()( a.pi_, b.pi_ );
    }

    void * get_deleter(std::type_info const & ti) const
    {
        return pi_? pi_->get_deleter( ti ): 0;
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

    weak_count(): pi_(0) // nothrow
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(weak_count_id)
#endif
    {
    }

    weak_count(shared_count const & r): pi_(r.pi_) // nothrow
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        if(pi_ != 0) pi_->weak_add_ref();
    }

    weak_count(weak_count const & r): pi_(r.pi_) // nothrow
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        , id_(shared_count_id)
#endif
    {
        if(pi_ != 0) pi_->weak_add_ref();
    }

    ~weak_count() // nothrow
    {
        if(pi_ != 0) pi_->weak_release();
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        id_ = 0;
#endif
    }

    weak_count & operator= (shared_count const & r) // nothrow
    {
        sp_counted_base * tmp = r.pi_;
        if(tmp != 0) tmp->weak_add_ref();
        if(pi_ != 0) pi_->weak_release();
        pi_ = tmp;

        return *this;
    }

    weak_count & operator= (weak_count const & r) // nothrow
    {
        sp_counted_base * tmp = r.pi_;
        if(tmp != 0) tmp->weak_add_ref();
        if(pi_ != 0) pi_->weak_release();
        pi_ = tmp;

        return *this;
    }

    void swap(weak_count & r) // nothrow
    {
        sp_counted_base * tmp = r.pi_;
        r.pi_ = pi_;
        pi_ = tmp;
    }

    long use_count() const // nothrow
    {
        return pi_ != 0? pi_->use_count(): 0;
    }

    friend inline bool operator==(weak_count const & a, weak_count const & b)
    {
        return a.pi_ == b.pi_;
    }

    friend inline bool operator<(weak_count const & a, weak_count const & b)
    {
        return std::less<sp_counted_base *>()(a.pi_, b.pi_);
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

} // namespace detail

} // namespace boost

#ifdef __BORLANDC__
# pragma warn .8027     // Functions containing try are not expanded inline
#endif

#endif  // #ifndef BOOST_DETAIL_SHARED_COUNT_HPP_INCLUDED
