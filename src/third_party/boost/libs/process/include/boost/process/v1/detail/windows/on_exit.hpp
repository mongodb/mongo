// Copyright (c) 2016 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROCESS_WINDOWS_ON_EXIT_HPP_
#define BOOST_PROCESS_WINDOWS_ON_EXIT_HPP_

#include <boost/process/v1/async.hpp>
#include <boost/process/v1/detail/config.hpp>
#include <boost/process/v1/detail/handler_base.hpp>
#include <boost/process/v1/detail/windows/async_handler.hpp>
#include <boost/asio/execution.hpp>
#include <system_error>
#include <functional>

namespace boost { namespace process { BOOST_PROCESS_V1_INLINE namespace v1 { namespace detail {

template<typename Tuple>
inline asio::io_context& get_io_context(const Tuple & tup);

namespace windows {

struct on_exit_ : boost::process::v1::detail::windows::async_handler
{
    std::function<void(int, const std::error_code&)> handler;
    on_exit_(const std::function<void(int, const std::error_code&)> & handler) : handler(handler)
    {

    }

    template<typename Executor>
    std::function<void(int, const std::error_code&)> on_exit_handler(Executor& exec)
    {
        auto v = boost::asio::prefer(boost::process::v1::detail::get_io_context(exec.seq).get_executor(),
                                     boost::asio::execution::outstanding_work.tracked);
        auto handler_ = this->handler;
        return [v, handler_](int exit_code, const std::error_code & ec)
               {
                    handler_(static_cast<int>(exit_code), ec);
               };

    }
};


}}}}}
#endif /* INCLUDE_BOOST_PROCESS_WINDOWS_ON_EXIT_HPP_ */
