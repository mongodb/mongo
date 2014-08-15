// Copyright (C) 2013 Vicente J. Botet Escriba
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// 2013/11 Vicente J. Botet Escriba
//    first implementation of a simple serial scheduler.

#ifndef BOOST_THREAD_SERIAL_EXECUTOR_HPP
#define BOOST_THREAD_SERIAL_EXECUTOR_HPP

#include <boost/thread/detail/config.hpp>
#include <boost/thread/detail/delete.hpp>
#include <boost/thread/detail/move.hpp>
#include <boost/thread/sync_queue.hpp>
#include <boost/thread/executors/work.hpp>
#include <boost/thread/executors/executor.hpp>
#include <boost/thread/future.hpp>
#include <boost/thread/scoped_thread.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
namespace executors
{
  class serial_executor
  {
  public:
    /// type-erasure to store the works to do
    typedef  executors::work work;
  private:
    typedef  scoped_thread<> thread_t;

    /// the thread safe work queue
    sync_queue<work > work_queue;
    executor& ex;
    thread_t thr;

    struct try_executing_one_task {
      work& task;
      boost::promise<void> &p;
      try_executing_one_task(work& task, boost::promise<void> &p)
      : task(task), p(p) {}
      void operator()() {
        task(); // if task() throws promise is not set but as the the program terminates and should terminate there is no need to use try-catch here.
        p.set_value();
      }
    };
  public:
    /**
     * Effects: try to execute one task.
     * Returns: whether a task has been executed.
     * Throws: whatever the current task constructor throws or the task() throws.
     */
    bool try_executing_one()
    {
      work task;
      try
      {
        if (work_queue.try_pull_front(task) == queue_op_status::success)
        {
          boost::promise<void> p;
          try_executing_one_task tmp(task,p);
          ex.submit(tmp);
//          ex.submit([&task, &p]()
//          {
//            task(); // if task() throws promise is not set but as the the program terminates and should terminate there is no need to use try-catch here.
//            p.set_value();
//          });
          p.get_future().wait();
          return true;
        }
        return false;
      }
      catch (std::exception& )
      {
        return false;
      }
      catch (...)
      {
        return false;
      }
    }
  private:
    /**
     * Effects: schedule one task or yields
     * Throws: whatever the current task constructor throws or the task() throws.
     */
    void schedule_one_or_yield()
    {
        if ( ! try_executing_one())
        {
          this_thread::yield();
        }
    }

    /**
     * The main loop of the worker thread
     */
    void worker_thread()
    {
      while (!closed())
      {
        schedule_one_or_yield();
      }
      while (try_executing_one())
      {
      }
    }

  public:
    /// serial_executor is not copyable.
    BOOST_THREAD_NO_COPYABLE(serial_executor)

    /**
     * \b Effects: creates a thread pool that runs closures using one of its closure-executing methods.
     *
     * \b Throws: Whatever exception is thrown while initializing the needed resources.
     */
    serial_executor(executor& ex)
    : ex(ex), thr(&serial_executor::worker_thread, this)
    {
    }
    /**
     * \b Effects: Destroys the thread pool.
     *
     * \b Synchronization: The completion of all the closures happen before the completion of the \c serial_executor destructor.
     */
    ~serial_executor()
    {
      // signal to all the worker thread that there will be no more submissions.
      close();
    }

    /**
     * \b Effects: close the \c serial_executor for submissions.
     * The loop will work until there is no more closures to run.
     */
    void close()
    {
      work_queue.close();
    }

    /**
     * \b Returns: whether the pool is closed for submissions.
     */
    bool closed()
    {
      return work_queue.closed();
    }

    /**
     * \b Requires: \c Closure is a model of \c Callable(void()) and a model of \c CopyConstructible/MoveConstructible.
     *
     * \b Effects: The specified \c closure will be scheduled for execution at some point in the future.
     * If invoked closure throws an exception the \c serial_executor will call \c std::terminate, as is the case with threads.
     *
     * \b Synchronization: completion of \c closure on a particular thread happens before destruction of thread's thread local variables.
     *
     * \b Throws: \c sync_queue_is_closed if the thread pool is closed.
     * Whatever exception that can be throw while storing the closure.
     */

#if defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
    template <typename Closure>
    void submit(Closure & closure)
    {
      work w ((closure));
      work_queue.push_back(boost::move(w));
      //work_queue.push(work(closure)); // todo check why this doesn't work
    }
#endif
    void submit(void (*closure)())
    {
      work w ((closure));
      work_queue.push_back(boost::move(w));
      //work_queue.push_back(work(closure)); // todo check why this doesn't work
    }

    template <typename Closure>
    void submit(BOOST_THREAD_RV_REF(Closure) closure)
    {
      work w =boost::move(closure);
      work_queue.push_back(boost::move(w));
      //work_queue.push_back(work(boost::move(closure))); // todo check why this doesn't work
    }

    /**
     * \b Requires: This must be called from an scheduled task.
     *
     * \b Effects: reschedule functions until pred()
     */
    template <typename Pred>
    bool reschedule_until(Pred const& pred)
    {
      do {
        if ( ! try_executing_one())
        {
          return false;
        }
      } while (! pred());
      return true;
    }

  };
}
using executors::serial_executor;
}

#include <boost/config/abi_suffix.hpp>

#endif
