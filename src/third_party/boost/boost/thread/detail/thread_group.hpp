#ifndef BOOST_THREAD_DETAIL_THREAD_GROUP_HPP
#define BOOST_THREAD_DETAIL_THREAD_GROUP_HPP
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2007-9 Anthony Williams

#include <list>
#include <boost/thread/shared_mutex.hpp>
#include <boost/thread/mutex.hpp>

#include <boost/config/abi_prefix.hpp>

#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable:4251)
#endif

namespace boost
{
    class thread_group
    {
    private:
        thread_group(thread_group const&);
        thread_group& operator=(thread_group const&);        
    public:
        thread_group() {}
        ~thread_group()
        {
            for(std::list<thread*>::iterator it=threads.begin(),end=threads.end();
                it!=end;
                ++it)
            {
                delete *it;
            }
        }

        template<typename F>
        thread* create_thread(F threadfunc)
        {
            boost::lock_guard<shared_mutex> guard(m);
            std::auto_ptr<thread> new_thread(new thread(threadfunc));
            threads.push_back(new_thread.get());
            return new_thread.release();
        }
        
        void add_thread(thread* thrd)
        {
            if(thrd)
            {
                boost::lock_guard<shared_mutex> guard(m);
                threads.push_back(thrd);
            }
        }
            
        void remove_thread(thread* thrd)
        {
            boost::lock_guard<shared_mutex> guard(m);
            std::list<thread*>::iterator const it=std::find(threads.begin(),threads.end(),thrd);
            if(it!=threads.end())
            {
                threads.erase(it);
            }
        }
        
        void join_all()
        {
            boost::shared_lock<shared_mutex> guard(m);
            
            for(std::list<thread*>::iterator it=threads.begin(),end=threads.end();
                it!=end;
                ++it)
            {
                (*it)->join();
            }
        }
        
        void interrupt_all()
        {
            boost::shared_lock<shared_mutex> guard(m);
            
            for(std::list<thread*>::iterator it=threads.begin(),end=threads.end();
                it!=end;
                ++it)
            {
                (*it)->interrupt();
            }
        }
        
        size_t size() const
        {
            boost::shared_lock<shared_mutex> guard(m);
            return threads.size();
        }
        
    private:
        std::list<thread*> threads;
        mutable shared_mutex m;
    };
}

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

#include <boost/config/abi_suffix.hpp>

#endif
