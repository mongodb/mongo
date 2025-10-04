// Copyright (c) 2006, 2007 Julio M. Merino Vidal
// Copyright (c) 2008 Ilya Sokolov, Boris Schaeling
// Copyright (c) 2009 Boris Schaeling
// Copyright (c) 2010 Felipe Tanus, Boris Schaeling
// Copyright (c) 2011, 2012 Jeff Flinn, Boris Schaeling
// Copyright (c) 2016 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROCESS_DETAIL_INITIALIZERS_THROW_ON_ERROR_HPP
#define BOOST_PROCESS_DETAIL_INITIALIZERS_THROW_ON_ERROR_HPP

#include <boost/process/v1/detail/config.hpp>
#include <boost/process/v1/detail/handler_base.hpp>

namespace boost { namespace process { BOOST_PROCESS_V1_INLINE namespace v1 { namespace detail {

struct throw_on_error_ : ::boost::process::v1::detail::handler
{
    template <class Executor>
    void on_error(Executor& exec, const std::error_code & ec) const
    {
        throw process_error(ec, "process creation failed");
    }

    const throw_on_error_ &operator()() const {return *this;}
};

}

constexpr boost::process::v1::detail::throw_on_error_ throw_on_error;

}}}

#endif
