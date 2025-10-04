#ifndef BOOST_SMART_PTR_DETAIL_LOCAL_SP_DELETER_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_LOCAL_SP_DELETER_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//  detail/local_sp_deleter.hpp
//
//  Copyright 2017 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  See http://www.boost.org/libs/smart_ptr/ for documentation.

#include <boost/smart_ptr/detail/local_counted_base.hpp>
#include <boost/config.hpp>

namespace boost
{

namespace detail
{

template<class D> class local_sp_deleter: public local_counted_impl_em
{
private:

    D d_;

public:

    local_sp_deleter(): d_()
    {
    }

    explicit local_sp_deleter( D const& d ) noexcept: d_( d )
    {
    }

    explicit local_sp_deleter( D&& d ) noexcept: d_( std::move(d) )
    {
    }

    D& deleter() noexcept
    {
        return d_;
    }

    template<class Y> void operator()( Y* p ) noexcept
    {
        d_( p );
    }

    void operator()( std::nullptr_t p ) noexcept
    {
        d_( p );
    }
};

template<> class local_sp_deleter<void>
{
};

template<class D> D * get_local_deleter( local_sp_deleter<D> * p ) noexcept
{
    return &p->deleter();
}

inline void * get_local_deleter( local_sp_deleter<void> * /*p*/ ) noexcept
{
    return 0;
}

} // namespace detail

} // namespace boost

#endif  // #ifndef BOOST_SMART_PTR_DETAIL_LOCAL_SP_DELETER_HPP_INCLUDED
