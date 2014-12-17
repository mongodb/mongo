#ifndef BOOST_THREAD_CONDITION_VARIABLE_WIN32_HPP
#define BOOST_THREAD_CONDITION_VARIABLE_WIN32_HPP
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2007-8 Anthony Williams

#include <boost/thread/mutex.hpp>
#include <boost/thread/win32/thread_primitives.hpp>
#include <limits.h>
#include <boost/assert.hpp>
#include <algorithm>
#include <boost/thread/thread.hpp>
#include <boost/thread/thread_time.hpp>
#include <boost/thread/win32/interlocked_read.hpp>
#include <boost/thread/xtime.hpp>
#include <vector>
#include <boost/intrusive_ptr.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    namespace detail
    {
        class basic_cv_list_entry;
        void intrusive_ptr_add_ref(basic_cv_list_entry * p);
        void intrusive_ptr_release(basic_cv_list_entry * p);

        class basic_cv_list_entry
        {
        private:
            detail::win32::handle_manager semaphore;
            detail::win32::handle_manager wake_sem;
            long waiters;
            bool notified;
            long references;

            basic_cv_list_entry(basic_cv_list_entry&);
            void operator=(basic_cv_list_entry&);

        public:
            explicit basic_cv_list_entry(detail::win32::handle_manager const& wake_sem_):
                semaphore(detail::win32::create_anonymous_semaphore(0,LONG_MAX)),
                wake_sem(wake_sem_.duplicate()),
                waiters(1),notified(false),references(0)
            {}

            static bool no_waiters(boost::intrusive_ptr<basic_cv_list_entry> const& entry)
            {
                return !detail::interlocked_read_acquire(&entry->waiters);
            }

            void add_waiter()
            {
                BOOST_INTERLOCKED_INCREMENT(&waiters);
            }

            void remove_waiter()
            {
                BOOST_INTERLOCKED_DECREMENT(&waiters);
            }

            void release(unsigned count_to_release)
            {
                notified=true;
                detail::win32::ReleaseSemaphore(semaphore,count_to_release,0);
            }

            void release_waiters()
            {
                release(detail::interlocked_read_acquire(&waiters));
            }

            bool is_notified() const
            {
                return notified;
            }

            bool wait(timeout wait_until)
            {
                return this_thread::interruptible_wait(semaphore,wait_until);
            }

            bool woken()
            {
                unsigned long const woken_result=detail::win32::WaitForSingleObject(wake_sem,0);
                BOOST_ASSERT((woken_result==detail::win32::timeout) || (woken_result==0));
                return woken_result==0;
            }

            friend void intrusive_ptr_add_ref(basic_cv_list_entry * p);
            friend void intrusive_ptr_release(basic_cv_list_entry * p);
        };

        inline void intrusive_ptr_add_ref(basic_cv_list_entry * p)
        {
            BOOST_INTERLOCKED_INCREMENT(&p->references);
        }

        inline void intrusive_ptr_release(basic_cv_list_entry * p)
        {
            if(!BOOST_INTERLOCKED_DECREMENT(&p->references))
            {
                delete p;
            }
        }

        class basic_condition_variable
        {
            boost::mutex internal_mutex;
            long total_count;
            unsigned active_generation_count;

            typedef basic_cv_list_entry list_entry;

            typedef boost::intrusive_ptr<list_entry> entry_ptr;
            typedef std::vector<entry_ptr> generation_list;

            generation_list generations;
            detail::win32::handle_manager wake_sem;

            void wake_waiters(long count_to_wake)
            {
                detail::interlocked_write_release(&total_count,total_count-count_to_wake);
                detail::win32::ReleaseSemaphore(wake_sem,count_to_wake,0);
            }

            template<typename lock_type>
            struct relocker
            {
                lock_type& lock;
                bool unlocked;

                relocker(lock_type& lock_):
                    lock(lock_),unlocked(false)
                {}
                void unlock()
                {
                    lock.unlock();
                    unlocked=true;
                }
                ~relocker()
                {
                    if(unlocked)
                    {
                        lock.lock();
                    }

                }
            private:
                relocker(relocker&);
                void operator=(relocker&);
            };


            entry_ptr get_wait_entry()
            {
                boost::lock_guard<boost::mutex> internal_lock(internal_mutex);

                if(!wake_sem)
                {
                    wake_sem=detail::win32::create_anonymous_semaphore(0,LONG_MAX);
                    BOOST_ASSERT(wake_sem);
                }

                detail::interlocked_write_release(&total_count,total_count+1);
                if(generations.empty() || generations.back()->is_notified())
                {
                    entry_ptr new_entry(new list_entry(wake_sem));
                    generations.push_back(new_entry);
                    return new_entry;
                }
                else
                {
                    generations.back()->add_waiter();
                    return generations.back();
                }
            }

            struct entry_manager
            {
                entry_ptr const entry;

                entry_manager(entry_ptr const& entry_):
                    entry(entry_)
                {}

                ~entry_manager()
                {
                    entry->remove_waiter();
                }

                list_entry* operator->()
                {
                    return entry.get();
                }

            private:
                void operator=(entry_manager&);
                entry_manager(entry_manager&);
            };


        protected:
            template<typename lock_type>
            bool do_wait(lock_type& lock,timeout wait_until)
            {
                relocker<lock_type> locker(lock);

                entry_manager entry(get_wait_entry());

                locker.unlock();

                bool woken=false;
                while(!woken)
                {
                    if(!entry->wait(wait_until))
                    {
                        return false;
                    }

                    woken=entry->woken();
                }
                return woken;
            }

            template<typename lock_type,typename predicate_type>
            bool do_wait(lock_type& m,timeout const& wait_until,predicate_type pred)
            {
                while (!pred())
                {
                    if(!do_wait(m, wait_until))
                        return pred();
                }
                return true;
            }

            basic_condition_variable(const basic_condition_variable& other);
            basic_condition_variable& operator=(const basic_condition_variable& other);

        public:
            basic_condition_variable():
                total_count(0),active_generation_count(0),wake_sem(0)
            {}

            ~basic_condition_variable()
            {}

            void notify_one()
            {
                if(detail::interlocked_read_acquire(&total_count))
                {
                    boost::lock_guard<boost::mutex> internal_lock(internal_mutex);
                    if(!total_count)
                    {
                        return;
                    }
                    wake_waiters(1);

                    for(generation_list::iterator it=generations.begin(),
                            end=generations.end();
                        it!=end;++it)
                    {
                        (*it)->release(1);
                    }
                    generations.erase(std::remove_if(generations.begin(),generations.end(),&basic_cv_list_entry::no_waiters),generations.end());
                }
            }

            void notify_all()
            {
                if(detail::interlocked_read_acquire(&total_count))
                {
                    boost::lock_guard<boost::mutex> internal_lock(internal_mutex);
                    if(!total_count)
                    {
                        return;
                    }
                    wake_waiters(total_count);
                    for(generation_list::iterator it=generations.begin(),
                            end=generations.end();
                        it!=end;++it)
                    {
                        (*it)->release_waiters();
                    }
                    generations.clear();
                    wake_sem=detail::win32::handle(0);
                }
            }

        };
    }

    class condition_variable:
        private detail::basic_condition_variable
    {
    private:
        condition_variable(condition_variable&);
        void operator=(condition_variable&);
    public:
        condition_variable()
        {}

        using detail::basic_condition_variable::notify_one;
        using detail::basic_condition_variable::notify_all;

        void wait(unique_lock<mutex>& m)
        {
            do_wait(m,detail::timeout::sentinel());
        }

        template<typename predicate_type>
        void wait(unique_lock<mutex>& m,predicate_type pred)
        {
            while(!pred()) wait(m);
        }


        bool timed_wait(unique_lock<mutex>& m,boost::system_time const& wait_until)
        {
            return do_wait(m,wait_until);
        }

        bool timed_wait(unique_lock<mutex>& m,boost::xtime const& wait_until)
        {
            return do_wait(m,system_time(wait_until));
        }
        template<typename duration_type>
        bool timed_wait(unique_lock<mutex>& m,duration_type const& wait_duration)
        {
            return do_wait(m,wait_duration.total_milliseconds());
        }

        template<typename predicate_type>
        bool timed_wait(unique_lock<mutex>& m,boost::system_time const& wait_until,predicate_type pred)
        {
            return do_wait(m,wait_until,pred);
        }
        template<typename predicate_type>
        bool timed_wait(unique_lock<mutex>& m,boost::xtime const& wait_until,predicate_type pred)
        {
            return do_wait(m,system_time(wait_until),pred);
        }
        template<typename duration_type,typename predicate_type>
        bool timed_wait(unique_lock<mutex>& m,duration_type const& wait_duration,predicate_type pred)
        {
            return do_wait(m,wait_duration.total_milliseconds(),pred);
        }
    };

    class condition_variable_any:
        private detail::basic_condition_variable
    {
    private:
        condition_variable_any(condition_variable_any&);
        void operator=(condition_variable_any&);
    public:
        condition_variable_any()
        {}

        using detail::basic_condition_variable::notify_one;
        using detail::basic_condition_variable::notify_all;

        template<typename lock_type>
        void wait(lock_type& m)
        {
            do_wait(m,detail::timeout::sentinel());
        }

        template<typename lock_type,typename predicate_type>
        void wait(lock_type& m,predicate_type pred)
        {
            while(!pred()) wait(m);
        }

        template<typename lock_type>
        bool timed_wait(lock_type& m,boost::system_time const& wait_until)
        {
            return do_wait(m,wait_until);
        }

        template<typename lock_type>
        bool timed_wait(lock_type& m,boost::xtime const& wait_until)
        {
            return do_wait(m,system_time(wait_until));
        }

        template<typename lock_type,typename duration_type>
        bool timed_wait(lock_type& m,duration_type const& wait_duration)
        {
            return do_wait(m,wait_duration.total_milliseconds());
        }

        template<typename lock_type,typename predicate_type>
        bool timed_wait(lock_type& m,boost::system_time const& wait_until,predicate_type pred)
        {
            return do_wait(m,wait_until,pred);
        }

        template<typename lock_type,typename predicate_type>
        bool timed_wait(lock_type& m,boost::xtime const& wait_until,predicate_type pred)
        {
            return do_wait(m,system_time(wait_until),pred);
        }

        template<typename lock_type,typename duration_type,typename predicate_type>
        bool timed_wait(lock_type& m,duration_type const& wait_duration,predicate_type pred)
        {
            return do_wait(m,wait_duration.total_milliseconds(),pred);
        }
    };

}

#include <boost/config/abi_suffix.hpp>

#endif
