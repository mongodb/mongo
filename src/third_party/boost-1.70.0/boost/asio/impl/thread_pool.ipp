//
// impl/thread_pool.ipp
// ~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_THREAD_POOL_IPP
#define BOOST_ASIO_IMPL_THREAD_POOL_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/thread_pool.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

struct thread_pool::thread_function
{
  detail::scheduler* scheduler_;

  void operator()()
  {
    boost::system::error_code ec;
    scheduler_->run(ec);
  }
};

thread_pool::thread_pool()
  : scheduler_(add_scheduler(new detail::scheduler(*this, 0, false)))
{
  scheduler_.work_started();

  thread_function f = { &scheduler_ };
  std::size_t num_threads = detail::thread::hardware_concurrency() * 2;
  threads_.create_threads(f, num_threads ? num_threads : 2);
}

thread_pool::thread_pool(std::size_t num_threads)
  : scheduler_(add_scheduler(new detail::scheduler(
          *this, num_threads == 1 ? 1 : 0, false)))
{
  scheduler_.work_started();

  thread_function f = { &scheduler_ };
  threads_.create_threads(f, num_threads);
}

thread_pool::~thread_pool()
{
  stop();
  join();
}

void thread_pool::stop()
{
  scheduler_.stop();
}

void thread_pool::join()
{
  if (!threads_.empty())
  {
    scheduler_.work_finished();
    threads_.join();
  }
}

detail::scheduler& thread_pool::add_scheduler(detail::scheduler* s)
{
  detail::scoped_ptr<detail::scheduler> scoped_impl(s);
  boost::asio::add_service<detail::scheduler>(*this, scoped_impl.get());
  return *scoped_impl.release();
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_THREAD_POOL_IPP
