// thread_pool.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <list>

#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/thread/condition.hpp>

namespace mongo {

    namespace threadpool {
        class Worker;

        typedef boost::function<void(void)> Task; //nullary function or functor

        // exported to the mongo namespace
        class ThreadPool : boost::noncopyable {
        public:
            explicit ThreadPool(int nThreads=8);

            // blocks until all tasks are complete (tasks_remaining() == 0)
            // You should not call schedule while in the destructor
            ~ThreadPool();

            // blocks until all tasks are complete (tasks_remaining() == 0)
            // does not prevent new tasks from being scheduled so could wait forever.
            // Also, new tasks could be scheduled after this returns.
            void join();

            // task will be copied a few times so make sure it's relatively cheap
            void schedule(Task task);

            // Helpers that wrap schedule and boost::bind.
            // Functor and args will be copied a few times so make sure it's relatively cheap
            template<typename F, typename A>
            void schedule(F f, A a) { schedule(boost::bind(f,a)); }
            template<typename F, typename A, typename B>
            void schedule(F f, A a, B b) { schedule(boost::bind(f,a,b)); }
            template<typename F, typename A, typename B, typename C>
            void schedule(F f, A a, B b, C c) { schedule(boost::bind(f,a,b,c)); }
            template<typename F, typename A, typename B, typename C, typename D>
            void schedule(F f, A a, B b, C c, D d) { schedule(boost::bind(f,a,b,c,d)); }
            template<typename F, typename A, typename B, typename C, typename D, typename E>
            void schedule(F f, A a, B b, C c, D d, E e) { schedule(boost::bind(f,a,b,c,d,e)); }

            int tasks_remaining() { return _tasksRemaining; }

        private:
            mongo::mutex _mutex;
            boost::condition _condition;

            std::list<Worker*> _freeWorkers; //used as LIFO stack (always front)
            std::list<Task> _tasks; //used as FIFO queue (push_back, pop_front)
            int _tasksRemaining; // in queue + currently processing
            int _nThreads; // only used for sanity checking. could be removed in the future.

            // should only be called by a worker from the worker's thread
            void task_done(Worker* worker);
            friend class Worker;
        };

    } //namespace threadpool

    using threadpool::ThreadPool;

} //namespace mongo
