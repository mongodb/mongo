//
// detail/impl/task_io_service.ipp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2012 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_IMPL_TASK_IO_SERVICE_IPP
#define BOOST_ASIO_DETAIL_IMPL_TASK_IO_SERVICE_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if !defined(BOOST_ASIO_HAS_IOCP)

#include <boost/limits.hpp>
#include <boost/asio/detail/event.hpp>
#include <boost/asio/detail/reactor.hpp>
#include <boost/asio/detail/task_io_service.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

struct task_io_service::task_cleanup
{
  ~task_cleanup()
  {
    // Enqueue the completed operations and reinsert the task at the end of
    // the operation queue.
    lock_->lock();
    task_io_service_->task_interrupted_ = true;
    task_io_service_->op_queue_.push(*ops_);
    task_io_service_->op_queue_.push(&task_io_service_->task_operation_);
  }

  task_io_service* task_io_service_;
  mutex::scoped_lock* lock_;
  op_queue<operation>* ops_;
};

struct task_io_service::work_cleanup
{
  ~work_cleanup()
  {
    task_io_service_->work_finished();

#if defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS) 
    if (!ops_->empty())
    {
      lock_->lock();
      task_io_service_->op_queue_.push(*ops_);
    }
#endif // defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS)
  }

  task_io_service* task_io_service_;
  mutex::scoped_lock* lock_;
  op_queue<operation>* ops_;
};

struct task_io_service::thread_info
{
  event* wakeup_event;
  op_queue<operation>* private_op_queue;
  thread_info* next;
};

task_io_service::task_io_service(
    boost::asio::io_service& io_service, std::size_t concurrency_hint)
  : boost::asio::detail::service_base<task_io_service>(io_service),
    one_thread_(concurrency_hint == 1),
    mutex_(),
    task_(0),
    task_interrupted_(true),
    outstanding_work_(0),
    stopped_(false),
    shutdown_(false),
    first_idle_thread_(0)
{
  BOOST_ASIO_HANDLER_TRACKING_INIT;
}

void task_io_service::shutdown_service()
{
  mutex::scoped_lock lock(mutex_);
  shutdown_ = true;
  lock.unlock();

  // Destroy handler objects.
  while (!op_queue_.empty())
  {
    operation* o = op_queue_.front();
    op_queue_.pop();
    if (o != &task_operation_)
      o->destroy();
  }

  // Reset to initial state.
  task_ = 0;
}

void task_io_service::init_task()
{
  mutex::scoped_lock lock(mutex_);
  if (!shutdown_ && !task_)
  {
    task_ = &use_service<reactor>(this->get_io_service());
    op_queue_.push(&task_operation_);
    wake_one_thread_and_unlock(lock);
  }
}

std::size_t task_io_service::run(boost::system::error_code& ec)
{
  ec = boost::system::error_code();
  if (outstanding_work_ == 0)
  {
    stop();
    return 0;
  }

  thread_info this_thread;
  event wakeup_event;
  this_thread.wakeup_event = &wakeup_event;
  op_queue<operation> private_op_queue;
#if defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS) 
  this_thread.private_op_queue = one_thread_ == 1 ? &private_op_queue : 0;
#else // defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS)
  this_thread.private_op_queue = 0;
#endif // defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS)
  this_thread.next = 0;
  thread_call_stack::context ctx(this, this_thread);

  mutex::scoped_lock lock(mutex_);

  std::size_t n = 0;
  for (; do_run_one(lock, this_thread, private_op_queue, ec); lock.lock())
    if (n != (std::numeric_limits<std::size_t>::max)())
      ++n;
  return n;
}

std::size_t task_io_service::run_one(boost::system::error_code& ec)
{
  ec = boost::system::error_code();
  if (outstanding_work_ == 0)
  {
    stop();
    return 0;
  }

  thread_info this_thread;
  event wakeup_event;
  this_thread.wakeup_event = &wakeup_event;
  op_queue<operation> private_op_queue;
  this_thread.private_op_queue = 0;
  this_thread.next = 0;
  thread_call_stack::context ctx(this, this_thread);

  mutex::scoped_lock lock(mutex_);

  return do_run_one(lock, this_thread, private_op_queue, ec);
}

std::size_t task_io_service::poll(boost::system::error_code& ec)
{
  ec = boost::system::error_code();
  if (outstanding_work_ == 0)
  {
    stop();
    return 0;
  }

  thread_info this_thread;
  this_thread.wakeup_event = 0;
  op_queue<operation> private_op_queue;
#if defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS) 
  this_thread.private_op_queue = one_thread_ == 1 ? &private_op_queue : 0;
#else // defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS)
  this_thread.private_op_queue = 0;
#endif // defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS)
  this_thread.next = 0;
  thread_call_stack::context ctx(this, this_thread);

  mutex::scoped_lock lock(mutex_);

#if defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS) 
  // We want to support nested calls to poll() and poll_one(), so any handlers
  // that are already on a thread-private queue need to be put on to the main
  // queue now.
  if (one_thread_)
    if (thread_info* outer_thread_info = ctx.next_by_key())
      if (outer_thread_info->private_op_queue)
        op_queue_.push(*outer_thread_info->private_op_queue);
#endif // defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS)

  std::size_t n = 0;
  for (; do_poll_one(lock, private_op_queue, ec); lock.lock())
    if (n != (std::numeric_limits<std::size_t>::max)())
      ++n;
  return n;
}

std::size_t task_io_service::poll_one(boost::system::error_code& ec)
{
  ec = boost::system::error_code();
  if (outstanding_work_ == 0)
  {
    stop();
    return 0;
  }

  thread_info this_thread;
  this_thread.wakeup_event = 0;
  op_queue<operation> private_op_queue;
  this_thread.private_op_queue = 0;
  this_thread.next = 0;
  thread_call_stack::context ctx(this, this_thread);

  mutex::scoped_lock lock(mutex_);

#if defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS) 
  // We want to support nested calls to poll() and poll_one(), so any handlers
  // that are already on a thread-private queue need to be put on to the main
  // queue now.
  if (one_thread_)
    if (thread_info* outer_thread_info = ctx.next_by_key())
      if (outer_thread_info->private_op_queue)
        op_queue_.push(*outer_thread_info->private_op_queue);
#endif // defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS)

  return do_poll_one(lock, private_op_queue, ec);
}

void task_io_service::stop()
{
  mutex::scoped_lock lock(mutex_);
  stop_all_threads(lock);
}

bool task_io_service::stopped() const
{
  mutex::scoped_lock lock(mutex_);
  return stopped_;
}

void task_io_service::reset()
{
  mutex::scoped_lock lock(mutex_);
  stopped_ = false;
}

void task_io_service::post_immediate_completion(task_io_service::operation* op)
{
  work_started();
  post_deferred_completion(op);
}

void task_io_service::post_deferred_completion(task_io_service::operation* op)
{
#if defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS) 
  if (one_thread_)
  {
    if (thread_info* this_thread = thread_call_stack::contains(this))
    {
      if (this_thread->private_op_queue)
      {
        this_thread->private_op_queue->push(op);
        return;
      }
    }
  }
#endif // defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS)

  mutex::scoped_lock lock(mutex_);
  op_queue_.push(op);
  wake_one_thread_and_unlock(lock);
}

void task_io_service::post_deferred_completions(
    op_queue<task_io_service::operation>& ops)
{
  if (!ops.empty())
  {
#if defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS) 
    if (one_thread_)
    {
      if (thread_info* this_thread = thread_call_stack::contains(this))
      {
        if (this_thread->private_op_queue)
        {
          this_thread->private_op_queue->push(ops);
          return;
        }
      }
    }
#endif // defined(BOOST_HAS_THREADS) && !defined(BOOST_ASIO_DISABLE_THREADS)

    mutex::scoped_lock lock(mutex_);
    op_queue_.push(ops);
    wake_one_thread_and_unlock(lock);
  }
}

void task_io_service::abandon_operations(
    op_queue<task_io_service::operation>& ops)
{
  op_queue<task_io_service::operation> ops2;
  ops2.push(ops);
}

std::size_t task_io_service::do_run_one(mutex::scoped_lock& lock,
    task_io_service::thread_info& this_thread,
    op_queue<operation>& private_op_queue, const boost::system::error_code& ec)
{
  while (!stopped_)
  {
    if (!op_queue_.empty())
    {
      // Prepare to execute first handler from queue.
      operation* o = op_queue_.front();
      op_queue_.pop();
      bool more_handlers = (!op_queue_.empty());

      if (o == &task_operation_)
      {
        task_interrupted_ = more_handlers;

        if (more_handlers && !one_thread_)
        {
          if (!wake_one_idle_thread_and_unlock(lock))
            lock.unlock();
        }
        else
          lock.unlock();

        op_queue<operation> completed_ops;
        task_cleanup on_exit = { this, &lock, &completed_ops };
        (void)on_exit;

        // Run the task. May throw an exception. Only block if the operation
        // queue is empty and we're not polling, otherwise we want to return
        // as soon as possible.
        task_->run(!more_handlers, completed_ops);
      }
      else
      {
        std::size_t task_result = o->task_result_;

        if (more_handlers && !one_thread_)
          wake_one_thread_and_unlock(lock);
        else
          lock.unlock();

        // Ensure the count of outstanding work is decremented on block exit.
        work_cleanup on_exit = { this, &lock, &private_op_queue };
        (void)on_exit;

        // Complete the operation. May throw an exception. Deletes the object.
        o->complete(*this, ec, task_result);

        return 1;
      }
    }
    else
    {
      // Nothing to run right now, so just wait for work to do.
      this_thread.next = first_idle_thread_;
      first_idle_thread_ = &this_thread;
      this_thread.wakeup_event->clear(lock);
      this_thread.wakeup_event->wait(lock);
    }
  }

  return 0;
}

std::size_t task_io_service::do_poll_one(mutex::scoped_lock& lock,
    op_queue<operation>& private_op_queue, const boost::system::error_code& ec)
{
  if (stopped_)
    return 0;

  operation* o = op_queue_.front();
  if (o == &task_operation_)
  {
    op_queue_.pop();
    lock.unlock();

    {
      op_queue<operation> completed_ops;
      task_cleanup c = { this, &lock, &completed_ops };
      (void)c;

      // Run the task. May throw an exception. Only block if the operation
      // queue is empty and we're not polling, otherwise we want to return
      // as soon as possible.
      task_->run(false, completed_ops);
    }

    o = op_queue_.front();
    if (o == &task_operation_)
      return 0;
  }

  if (o == 0)
    return 0;

  op_queue_.pop();
  bool more_handlers = (!op_queue_.empty());

  std::size_t task_result = o->task_result_;

  if (more_handlers && !one_thread_)
    wake_one_thread_and_unlock(lock);
  else
    lock.unlock();

  // Ensure the count of outstanding work is decremented on block exit.
  work_cleanup on_exit = { this, &lock, &private_op_queue };
  (void)on_exit;

  // Complete the operation. May throw an exception. Deletes the object.
  o->complete(*this, ec, task_result);

  return 1;
}

void task_io_service::stop_all_threads(
    mutex::scoped_lock& lock)
{
  stopped_ = true;

  while (first_idle_thread_)
  {
    thread_info* idle_thread = first_idle_thread_;
    first_idle_thread_ = idle_thread->next;
    idle_thread->next = 0;
    idle_thread->wakeup_event->signal(lock);
  }

  if (!task_interrupted_ && task_)
  {
    task_interrupted_ = true;
    task_->interrupt();
  }
}

bool task_io_service::wake_one_idle_thread_and_unlock(
    mutex::scoped_lock& lock)
{
  if (first_idle_thread_)
  {
    thread_info* idle_thread = first_idle_thread_;
    first_idle_thread_ = idle_thread->next;
    idle_thread->next = 0;
    idle_thread->wakeup_event->signal_and_unlock(lock);
    return true;
  }
  return false;
}

void task_io_service::wake_one_thread_and_unlock(
    mutex::scoped_lock& lock)
{
  if (!wake_one_idle_thread_and_unlock(lock))
  {
    if (!task_interrupted_ && task_)
    {
      task_interrupted_ = true;
      task_->interrupt();
    }
    lock.unlock();
  }
}

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // !defined(BOOST_ASIO_HAS_IOCP)

#endif // BOOST_ASIO_DETAIL_IMPL_TASK_IO_SERVICE_IPP
