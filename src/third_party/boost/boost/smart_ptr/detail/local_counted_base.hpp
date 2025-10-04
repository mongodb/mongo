#ifndef BOOST_SMART_PTR_DETAIL_LOCAL_COUNTED_BASE_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_LOCAL_COUNTED_BASE_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//  detail/local_counted_base.hpp
//
//  Copyright 2017 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  See http://www.boost.org/libs/smart_ptr/ for documentation.

#include <boost/smart_ptr/detail/shared_count.hpp>
#include <boost/config.hpp>
#include <utility>

namespace boost
{

namespace detail
{

class BOOST_SYMBOL_VISIBLE local_counted_base
{
private:

    local_counted_base & operator= ( local_counted_base const & );

private:

    // not 'int' or 'unsigned' to avoid aliasing and enable optimizations
    enum count_type { min_ = 0, initial_ = 1, max_ = 2147483647 };

    count_type local_use_count_;

public:

    constexpr local_counted_base() noexcept: local_use_count_( initial_ )
    {
    }

    constexpr local_counted_base( local_counted_base const & ) noexcept: local_use_count_( initial_ )
    {
    }

    virtual ~local_counted_base() /*noexcept*/
    {
    }

    virtual void local_cb_destroy() noexcept = 0;

    virtual boost::detail::shared_count local_cb_get_shared_count() const noexcept = 0;

    void add_ref() noexcept
    {
#if !defined(__NVCC__)
#if defined( __has_builtin )
# if __has_builtin( __builtin_assume )

        __builtin_assume( local_use_count_ >= 1 );

# endif
#endif
#endif

        local_use_count_ = static_cast<count_type>( local_use_count_ + 1 );
    }

    void release() noexcept
    {
        local_use_count_ = static_cast<count_type>( local_use_count_ - 1 );

        if( local_use_count_ == 0 )
        {
            local_cb_destroy();
        }
    }

    long local_use_count() const noexcept
    {
        return local_use_count_;
    }
};

class BOOST_SYMBOL_VISIBLE local_counted_impl: public local_counted_base
{
private:

    local_counted_impl( local_counted_impl const & );

private:

    shared_count pn_;

public:

    explicit local_counted_impl( shared_count const& pn ) noexcept: pn_( pn )
    {
    }

    explicit local_counted_impl( shared_count && pn ) noexcept: pn_( std::move(pn) )
    {
    }

    void local_cb_destroy() noexcept override
    {
        delete this;
    }

    boost::detail::shared_count local_cb_get_shared_count() const noexcept override
    {
        return pn_;
    }
};

class BOOST_SYMBOL_VISIBLE local_counted_impl_em: public local_counted_base
{
public:

    shared_count pn_;

    void local_cb_destroy() noexcept override
    {
        shared_count().swap( pn_ );
    }

    boost::detail::shared_count local_cb_get_shared_count() const noexcept override
    {
        return pn_;
    }
};

} // namespace detail

} // namespace boost

#endif  // #ifndef BOOST_SMART_PTR_DETAIL_LOCAL_COUNTED_BASE_HPP_INCLUDED
