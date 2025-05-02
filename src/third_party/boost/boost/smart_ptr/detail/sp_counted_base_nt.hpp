#ifndef BOOST_SMART_PTR_DETAIL_SP_COUNTED_BASE_NT_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_SP_COUNTED_BASE_NT_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//
//  detail/sp_counted_base_nt.hpp
//
//  Copyright (c) 2001, 2002, 2003 Peter Dimov and Multi Media Ltd.
//  Copyright 2004-2005 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/smart_ptr/detail/sp_typeinfo_.hpp>
#include <boost/config.hpp>
#include <boost/cstdint.hpp>

#if defined(BOOST_SP_REPORT_IMPLEMENTATION)

#include <boost/config/pragma_message.hpp>
BOOST_PRAGMA_MESSAGE("Using single-threaded, non-atomic sp_counted_base")

#endif

namespace boost
{

namespace detail
{

class BOOST_SYMBOL_VISIBLE sp_counted_base
{
private:

    sp_counted_base( sp_counted_base const & );
    sp_counted_base & operator= ( sp_counted_base const & );

    boost::int_least32_t use_count_;        // #shared
    boost::int_least32_t weak_count_;       // #weak + (#shared != 0)

public:

    sp_counted_base() noexcept: use_count_( 1 ), weak_count_( 1 )
    {
    }

    virtual ~sp_counted_base() /*noexcept*/
    {
    }

    // dispose() is called when use_count_ drops to zero, to release
    // the resources managed by *this.

    virtual void dispose() noexcept = 0; // nothrow

    // destroy() is called when weak_count_ drops to zero.

    virtual void destroy() noexcept // nothrow
    {
        delete this;
    }

    virtual void * get_deleter( sp_typeinfo_ const & ti ) noexcept = 0;
    virtual void * get_local_deleter( sp_typeinfo_ const & ti ) noexcept = 0;
    virtual void * get_untyped_deleter() noexcept = 0;

    void add_ref_copy() noexcept
    {
        ++use_count_;
    }

    bool add_ref_lock() noexcept // true on success
    {
        if( use_count_ == 0 ) return false;
        ++use_count_;
        return true;
    }

    void release() noexcept
    {
        if( --use_count_ == 0 )
        {
            dispose();
            weak_release();
        }
    }

    void weak_add_ref() noexcept
    {
        ++weak_count_;
    }

    void weak_release() noexcept
    {
        if( --weak_count_ == 0 )
        {
            destroy();
        }
    }

    long use_count() const noexcept
    {
        return use_count_;
    }
};

} // namespace detail

} // namespace boost

#endif  // #ifndef BOOST_SMART_PTR_DETAIL_SP_COUNTED_BASE_NT_HPP_INCLUDED
