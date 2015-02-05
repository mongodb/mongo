// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2007 Anthony Williams
// (C) Copyright 2007 David Deakins
// (C) Copyright 2011-2013 Vicente J. Botet Escriba

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x400
#endif

#ifndef WINVER
#define WINVER 0x400
#endif
//#define BOOST_THREAD_VERSION 3

#include <boost/thread/thread_only.hpp>
#include <boost/thread/once.hpp>
#include <boost/thread/tss.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/detail/tss_hooks.hpp>
#include <boost/thread/future.hpp>

#include <boost/assert.hpp>
#include <boost/cstdint.hpp>
#if defined BOOST_THREAD_USES_DATETIME
#include <boost/date_time/posix_time/conversion.hpp>
#endif
#include <memory>
#include <algorithm>
#ifndef UNDER_CE
#include <process.h>
#endif
#include <stdio.h>
#include <windows.h>

namespace boost
{
  namespace detail
  {
    thread_data_base::~thread_data_base()
    {
        for (notify_list_t::iterator i = notify.begin(), e = notify.end();
                i != e; ++i)
        {
            i->second->unlock();
            i->first->notify_all();
        }
        for (async_states_t::iterator i = async_states_.begin(), e = async_states_.end();
                i != e; ++i)
        {
            (*i)->make_ready();
        }
    }
  }

    namespace
    {
#ifdef BOOST_THREAD_PROVIDES_ONCE_CXX11
        boost::once_flag current_thread_tls_init_flag;
#else
        boost::once_flag current_thread_tls_init_flag=BOOST_ONCE_INIT;
#endif
#if defined(UNDER_CE)
        // Windows CE does not define the TLS_OUT_OF_INDEXES constant.
#define TLS_OUT_OF_INDEXES 0xFFFFFFFF
#endif
        DWORD current_thread_tls_key=TLS_OUT_OF_INDEXES;

        void create_current_thread_tls_key()
        {
            tss_cleanup_implemented(); // if anyone uses TSS, we need the cleanup linked in
            current_thread_tls_key=TlsAlloc();
            BOOST_ASSERT(current_thread_tls_key!=TLS_OUT_OF_INDEXES);
        }

        void cleanup_tls_key()
        {
            if(current_thread_tls_key!=TLS_OUT_OF_INDEXES)
            {
                TlsFree(current_thread_tls_key);
                current_thread_tls_key=TLS_OUT_OF_INDEXES;
            }
        }

        void set_current_thread_data(detail::thread_data_base* new_data)
        {
            boost::call_once(current_thread_tls_init_flag,create_current_thread_tls_key);
            if (current_thread_tls_key!=TLS_OUT_OF_INDEXES)
            {
                BOOST_VERIFY(TlsSetValue(current_thread_tls_key,new_data));
            }
            else
            {
                BOOST_VERIFY(false);
                //boost::throw_exception(thread_resource_error());
            }
        }

    }
    namespace detail
    {
      thread_data_base* get_current_thread_data()
      {
          if(current_thread_tls_key==TLS_OUT_OF_INDEXES)
          {
              return 0;
          }
          return (detail::thread_data_base*)TlsGetValue(current_thread_tls_key);
      }
    }
    namespace
    {
#ifndef BOOST_HAS_THREADEX
// Windows CE doesn't define _beginthreadex

        struct ThreadProxyData
        {
            typedef unsigned (__stdcall* func)(void*);
            func start_address_;
            void* arglist_;
            ThreadProxyData(func start_address,void* arglist) : start_address_(start_address), arglist_(arglist) {}
        };

        DWORD WINAPI ThreadProxy(LPVOID args)
        {
            std::auto_ptr<ThreadProxyData> data(reinterpret_cast<ThreadProxyData*>(args));
            DWORD ret=data->start_address_(data->arglist_);
            return ret;
        }

        //typedef void* uintptr_t;

        inline uintptr_t _beginthreadex(void* security, unsigned stack_size, unsigned (__stdcall* start_address)(void*),
                                              void* arglist, unsigned initflag, unsigned* thrdaddr)
        {
            DWORD threadID;
            ThreadProxyData* data = new ThreadProxyData(start_address,arglist);
            HANDLE hthread=CreateThread(static_cast<LPSECURITY_ATTRIBUTES>(security),stack_size,ThreadProxy,
                                        data,initflag,&threadID);
            if (hthread==0) {
              delete data;
              return 0;
            }
            *thrdaddr=threadID;
            return reinterpret_cast<uintptr_t const>(hthread);
        }

#endif

    }

    namespace detail
    {
        struct thread_exit_callback_node
        {
            boost::detail::thread_exit_function_base* func;
            thread_exit_callback_node* next;

            thread_exit_callback_node(boost::detail::thread_exit_function_base* func_,
                                      thread_exit_callback_node* next_):
                func(func_),next(next_)
            {}
        };

    }

    namespace
    {
        void run_thread_exit_callbacks()
        {
            detail::thread_data_ptr current_thread_data(detail::get_current_thread_data(),false);
            if(current_thread_data)
            {
                while(! current_thread_data->tss_data.empty() || current_thread_data->thread_exit_callbacks)
                {
                    while(current_thread_data->thread_exit_callbacks)
                    {
                        detail::thread_exit_callback_node* const current_node=current_thread_data->thread_exit_callbacks;
                        current_thread_data->thread_exit_callbacks=current_node->next;
                        if(current_node->func)
                        {
                            (*current_node->func)();
                            boost::detail::heap_delete(current_node->func);
                        }
                        boost::detail::heap_delete(current_node);
                    }
                    for(std::map<void const*,detail::tss_data_node>::iterator next=current_thread_data->tss_data.begin(),
                            current,
                            end=current_thread_data->tss_data.end();
                        next!=end;)
                    {
                        current=next;
                        ++next;
                        if(current->second.func && (current->second.value!=0))
                        {
                            (*current->second.func)(current->second.value);
                        }
                        current_thread_data->tss_data.erase(current);
                    }
                }

                set_current_thread_data(0);
            }
        }

        unsigned __stdcall thread_start_function(void* param)
        {
            detail::thread_data_base* const thread_info(reinterpret_cast<detail::thread_data_base*>(param));
            set_current_thread_data(thread_info);
#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
            BOOST_TRY
            {
#endif
                thread_info->run();
#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
            }
            BOOST_CATCH(thread_interrupted const&)
            {
            }
// Removed as it stops the debugger identifying the cause of the exception
// Unhandled exceptions still cause the application to terminate
//             BOOST_CATCH(...)
//             {
//                 std::terminate();
//             }
            BOOST_CATCH_END
#endif
            run_thread_exit_callbacks();
            return 0;
        }
    }

    thread::thread() BOOST_NOEXCEPT
    {}

    bool thread::start_thread_noexcept()
    {
        uintptr_t const new_thread=_beginthreadex(0,0,&thread_start_function,thread_info.get(),CREATE_SUSPENDED,&thread_info->id);
        if(!new_thread)
        {
            return false;
//            boost::throw_exception(thread_resource_error());
        }
        intrusive_ptr_add_ref(thread_info.get());
        thread_info->thread_handle=(detail::win32::handle)(new_thread);
        ResumeThread(thread_info->thread_handle);
        return true;
    }

    bool thread::start_thread_noexcept(const attributes& attr)
    {
      //uintptr_t const new_thread=_beginthreadex(attr.get_security(),attr.get_stack_size(),&thread_start_function,thread_info.get(),CREATE_SUSPENDED,&thread_info->id);
      uintptr_t const new_thread=_beginthreadex(0,static_cast<unsigned int>(attr.get_stack_size()),&thread_start_function,thread_info.get(),CREATE_SUSPENDED,&thread_info->id);
      if(!new_thread)
      {
        return false;
//          boost::throw_exception(thread_resource_error());
      }
      intrusive_ptr_add_ref(thread_info.get());
      thread_info->thread_handle=(detail::win32::handle)(new_thread);
      ResumeThread(thread_info->thread_handle);
      return true;
    }

    thread::thread(detail::thread_data_ptr data):
        thread_info(data)
    {}

    namespace
    {
        struct externally_launched_thread:
            detail::thread_data_base
        {
            externally_launched_thread()
            {
                ++count;
#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
                interruption_enabled=false;
#endif
            }
            ~externally_launched_thread() {
              BOOST_ASSERT(notify.empty());
              notify.clear();
              BOOST_ASSERT(async_states_.empty());
              async_states_.clear();
            }

            void run()
            {}
            void notify_all_at_thread_exit(condition_variable*, mutex*)
            {}

        private:
            externally_launched_thread(externally_launched_thread&);
            void operator=(externally_launched_thread&);
        };

        void make_external_thread_data()
        {
            externally_launched_thread* me=detail::heap_new<externally_launched_thread>();
            BOOST_TRY
            {
                set_current_thread_data(me);
            }
            BOOST_CATCH(...)
            {
                detail::heap_delete(me);
                BOOST_RETHROW
            }
            BOOST_CATCH_END
        }

        detail::thread_data_base* get_or_make_current_thread_data()
        {
            detail::thread_data_base* current_thread_data(detail::get_current_thread_data());
            if(!current_thread_data)
            {
                make_external_thread_data();
                current_thread_data=detail::get_current_thread_data();
            }
            return current_thread_data;
        }

    }

    thread::id thread::get_id() const BOOST_NOEXCEPT
    {
    #if defined BOOST_THREAD_PROVIDES_BASIC_THREAD_ID
      detail::thread_data_ptr local_thread_info=(get_thread_info)();
      return local_thread_info?local_thread_info->id:0;
      //return const_cast<thread*>(this)->native_handle();
    #else
        return thread::id((get_thread_info)());
    #endif
    }

    bool thread::joinable() const BOOST_NOEXCEPT
    {
        return (get_thread_info)() ? true : false;
    }
    bool thread::join_noexcept()
    {

        detail::thread_data_ptr local_thread_info=(get_thread_info)();
        if(local_thread_info)
        {
            this_thread::interruptible_wait(local_thread_info->thread_handle,detail::timeout::sentinel());
            release_handle();
            return true;
        }
        else
        {
          return false;
        }
    }

#if defined BOOST_THREAD_USES_DATETIME
    bool thread::timed_join(boost::system_time const& wait_until)
    {
      return do_try_join_until(get_milliseconds_until(wait_until));
    }
#endif
    bool thread::do_try_join_until_noexcept(uintmax_t milli, bool& res)
    {
      detail::thread_data_ptr local_thread_info=(get_thread_info)();
      if(local_thread_info)
      {
          if(!this_thread::interruptible_wait(local_thread_info->thread_handle,milli))
          {
            res=false;
            return true;
          }
          release_handle();
          res=true;
          return true;
      }
      else
      {
        return false;
      }
    }

    void thread::detach()
    {
        release_handle();
    }

    void thread::release_handle()
    {
        thread_info=0;
    }

#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
    void thread::interrupt()
    {
        detail::thread_data_ptr local_thread_info=(get_thread_info)();
        if(local_thread_info)
        {
            local_thread_info->interrupt();
        }
    }

    bool thread::interruption_requested() const BOOST_NOEXCEPT
    {
        detail::thread_data_ptr local_thread_info=(get_thread_info)();
        return local_thread_info.get() && (detail::win32::WaitForSingleObject(local_thread_info->interruption_handle,0)==0);
    }

#endif

    unsigned thread::hardware_concurrency() BOOST_NOEXCEPT
    {
        //SYSTEM_INFO info={{0}};
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwNumberOfProcessors;
    }

    unsigned thread::physical_concurrency() BOOST_NOEXCEPT
    {
        unsigned cores = 0;
#if !(defined(__MINGW32__) || defined (__MINGW64__))
        DWORD size = 0;

        GetLogicalProcessorInformation(NULL, &size);
        if (ERROR_INSUFFICIENT_BUFFER != GetLastError())
            return 0;

        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(size);
        if (GetLogicalProcessorInformation(&buffer.front(), &size) == FALSE)
            return 0;

        const size_t Elements = size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);

        for (size_t i = 0; i < Elements; ++i) {
            if (buffer[i].Relationship == RelationProcessorCore)
                ++cores;
        }
#endif
        return cores;
    }

    thread::native_handle_type thread::native_handle()
    {
        detail::thread_data_ptr local_thread_info=(get_thread_info)();
        return local_thread_info?(detail::win32::handle)local_thread_info->thread_handle:detail::win32::invalid_handle_value;
    }

    detail::thread_data_ptr thread::get_thread_info BOOST_PREVENT_MACRO_SUBSTITUTION () const
    {
        return thread_info;
    }

    namespace this_thread
    {
        namespace
        {
            LARGE_INTEGER get_due_time(detail::timeout const&  target_time)
            {
                LARGE_INTEGER due_time={{0,0}};
                if(target_time.relative)
                {
                    detail::win32::ticks_type const elapsed_milliseconds=detail::win32::GetTickCount64()()-target_time.start;
                    LONGLONG const remaining_milliseconds=(target_time.milliseconds-elapsed_milliseconds);
                    LONGLONG const hundred_nanoseconds_in_one_millisecond=10000;

                    if(remaining_milliseconds>0)
                    {
                        due_time.QuadPart=-(remaining_milliseconds*hundred_nanoseconds_in_one_millisecond);
                    }
                }
                else
                {
                    SYSTEMTIME target_system_time={0,0,0,0,0,0,0,0};
                    target_system_time.wYear=target_time.abs_time.date().year();
                    target_system_time.wMonth=target_time.abs_time.date().month();
                    target_system_time.wDay=target_time.abs_time.date().day();
                    target_system_time.wHour=(WORD)target_time.abs_time.time_of_day().hours();
                    target_system_time.wMinute=(WORD)target_time.abs_time.time_of_day().minutes();
                    target_system_time.wSecond=(WORD)target_time.abs_time.time_of_day().seconds();

                    if(!SystemTimeToFileTime(&target_system_time,((FILETIME*)&due_time)))
                    {
                        due_time.QuadPart=0;
                    }
                    else
                    {
                        long const hundred_nanoseconds_in_one_second=10000000;
                        posix_time::time_duration::tick_type const ticks_per_second=
                            target_time.abs_time.time_of_day().ticks_per_second();
                        if(ticks_per_second>hundred_nanoseconds_in_one_second)
                        {
                            posix_time::time_duration::tick_type const
                                ticks_per_hundred_nanoseconds=
                                ticks_per_second/hundred_nanoseconds_in_one_second;
                            due_time.QuadPart+=
                                target_time.abs_time.time_of_day().fractional_seconds()/
                                ticks_per_hundred_nanoseconds;
                        }
                        else
                        {
                            due_time.QuadPart+=
                                target_time.abs_time.time_of_day().fractional_seconds()*
                                (hundred_nanoseconds_in_one_second/ticks_per_second);
                        }
                    }
                }
                return due_time;
            }
        }


        bool interruptible_wait(detail::win32::handle handle_to_wait_for,detail::timeout target_time)
        {
            detail::win32::handle handles[3]={0};
            unsigned handle_count=0;
            unsigned wait_handle_index=~0U;
#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
            unsigned interruption_index=~0U;
#endif
            unsigned timeout_index=~0U;
            if(handle_to_wait_for!=detail::win32::invalid_handle_value)
            {
                wait_handle_index=handle_count;
                handles[handle_count++]=handle_to_wait_for;
            }
#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
            if(detail::get_current_thread_data() && detail::get_current_thread_data()->interruption_enabled)
            {
                interruption_index=handle_count;
                handles[handle_count++]=detail::get_current_thread_data()->interruption_handle;
            }
#endif
            detail::win32::handle_manager timer_handle;

#ifndef UNDER_CE
            unsigned const min_timer_wait_period=20;

            if(!target_time.is_sentinel())
            {
                detail::timeout::remaining_time const time_left=target_time.remaining_milliseconds();
                if(time_left.milliseconds > min_timer_wait_period)
                {
                    // for a long-enough timeout, use a waitable timer (which tracks clock changes)
                    timer_handle=CreateWaitableTimer(NULL,false,NULL);
                    if(timer_handle!=0)
                    {
                        LARGE_INTEGER due_time=get_due_time(target_time);

                        bool const set_time_succeeded=SetWaitableTimer(timer_handle,&due_time,0,0,0,false)!=0;
                        if(set_time_succeeded)
                        {
                            timeout_index=handle_count;
                            handles[handle_count++]=timer_handle;
                        }
                    }
                }
                else if(!target_time.relative)
                {
                    // convert short absolute-time timeouts into relative ones, so we don't race against clock changes
                    target_time=detail::timeout(time_left.milliseconds);
                }
            }
#endif

            bool const using_timer=timeout_index!=~0u;
            detail::timeout::remaining_time time_left(0);

            do
            {
                if(!using_timer)
                {
                    time_left=target_time.remaining_milliseconds();
                }

                if(handle_count)
                {
                    unsigned long const notified_index=detail::win32::WaitForMultipleObjects(handle_count,handles,false,using_timer?INFINITE:time_left.milliseconds);
                    if(notified_index<handle_count)
                    {
                        if(notified_index==wait_handle_index)
                        {
                            return true;
                        }
#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
                        else if(notified_index==interruption_index)
                        {
                            detail::win32::ResetEvent(detail::get_current_thread_data()->interruption_handle);
                            throw thread_interrupted();
                        }
#endif
                        else if(notified_index==timeout_index)
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    detail::win32::Sleep(time_left.milliseconds);
                }
                if(target_time.relative)
                {
                    target_time.milliseconds-=detail::timeout::max_non_infinite_wait;
                }
            }
            while(time_left.more);
            return false;
        }

        namespace no_interruption_point
        {
        bool non_interruptible_wait(detail::win32::handle handle_to_wait_for,detail::timeout target_time)
        {
            detail::win32::handle handles[3]={0};
            unsigned handle_count=0;
            unsigned wait_handle_index=~0U;
            unsigned timeout_index=~0U;
            if(handle_to_wait_for!=detail::win32::invalid_handle_value)
            {
                wait_handle_index=handle_count;
                handles[handle_count++]=handle_to_wait_for;
            }
            detail::win32::handle_manager timer_handle;

#ifndef UNDER_CE
            unsigned const min_timer_wait_period=20;

            if(!target_time.is_sentinel())
            {
                detail::timeout::remaining_time const time_left=target_time.remaining_milliseconds();
                if(time_left.milliseconds > min_timer_wait_period)
                {
                    // for a long-enough timeout, use a waitable timer (which tracks clock changes)
                    timer_handle=CreateWaitableTimer(NULL,false,NULL);
                    if(timer_handle!=0)
                    {
                        LARGE_INTEGER due_time=get_due_time(target_time);

                        bool const set_time_succeeded=SetWaitableTimer(timer_handle,&due_time,0,0,0,false)!=0;
                        if(set_time_succeeded)
                        {
                            timeout_index=handle_count;
                            handles[handle_count++]=timer_handle;
                        }
                    }
                }
                else if(!target_time.relative)
                {
                    // convert short absolute-time timeouts into relative ones, so we don't race against clock changes
                    target_time=detail::timeout(time_left.milliseconds);
                }
            }
#endif

            bool const using_timer=timeout_index!=~0u;
            detail::timeout::remaining_time time_left(0);

            do
            {
                if(!using_timer)
                {
                    time_left=target_time.remaining_milliseconds();
                }

                if(handle_count)
                {
                    unsigned long const notified_index=detail::win32::WaitForMultipleObjects(handle_count,handles,false,using_timer?INFINITE:time_left.milliseconds);
                    if(notified_index<handle_count)
                    {
                        if(notified_index==wait_handle_index)
                        {
                            return true;
                        }
                        else if(notified_index==timeout_index)
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    detail::win32::Sleep(time_left.milliseconds);
                }
                if(target_time.relative)
                {
                    target_time.milliseconds-=detail::timeout::max_non_infinite_wait;
                }
            }
            while(time_left.more);
            return false;
        }
        }

        thread::id get_id() BOOST_NOEXCEPT
        {
        #if defined BOOST_THREAD_PROVIDES_BASIC_THREAD_ID
          return detail::win32::GetCurrentThreadId();
        #else
            return thread::id(get_or_make_current_thread_data());
        #endif
        }

#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
        void interruption_point()
        {
            if(interruption_enabled() && interruption_requested())
            {
                detail::win32::ResetEvent(detail::get_current_thread_data()->interruption_handle);
                throw thread_interrupted();
            }
        }

        bool interruption_enabled() BOOST_NOEXCEPT
        {
            return detail::get_current_thread_data() && detail::get_current_thread_data()->interruption_enabled;
        }

        bool interruption_requested() BOOST_NOEXCEPT
        {
            return detail::get_current_thread_data() && (detail::win32::WaitForSingleObject(detail::get_current_thread_data()->interruption_handle,0)==0);
        }
#endif

        void yield() BOOST_NOEXCEPT
        {
            detail::win32::Sleep(0);
        }

#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
        disable_interruption::disable_interruption() BOOST_NOEXCEPT:
            interruption_was_enabled(interruption_enabled())
        {
            if(interruption_was_enabled)
            {
                detail::get_current_thread_data()->interruption_enabled=false;
            }
        }

        disable_interruption::~disable_interruption() BOOST_NOEXCEPT
        {
            if(detail::get_current_thread_data())
            {
                detail::get_current_thread_data()->interruption_enabled=interruption_was_enabled;
            }
        }

        restore_interruption::restore_interruption(disable_interruption& d) BOOST_NOEXCEPT
        {
            if(d.interruption_was_enabled)
            {
                detail::get_current_thread_data()->interruption_enabled=true;
            }
        }

        restore_interruption::~restore_interruption() BOOST_NOEXCEPT
        {
            if(detail::get_current_thread_data())
            {
                detail::get_current_thread_data()->interruption_enabled=false;
            }
        }
#endif
    }

    namespace detail
    {
        void add_thread_exit_function(thread_exit_function_base* func)
        {
            detail::thread_data_base* const current_thread_data(get_or_make_current_thread_data());
            thread_exit_callback_node* const new_node=
                heap_new<thread_exit_callback_node>(
                    func,current_thread_data->thread_exit_callbacks);
            current_thread_data->thread_exit_callbacks=new_node;
        }

        tss_data_node* find_tss_data(void const* key)
        {
            detail::thread_data_base* const current_thread_data(get_current_thread_data());
            if(current_thread_data)
            {
                std::map<void const*,tss_data_node>::iterator current_node=
                    current_thread_data->tss_data.find(key);
                if(current_node!=current_thread_data->tss_data.end())
                {
                    return &current_node->second;
                }
            }
            return NULL;
        }

        void* get_tss_data(void const* key)
        {
            if(tss_data_node* const current_node=find_tss_data(key))
            {
                return current_node->value;
            }
            return NULL;
        }

        void add_new_tss_node(void const* key,
                              boost::shared_ptr<tss_cleanup_function> func,
                              void* tss_data)
        {
            detail::thread_data_base* const current_thread_data(get_or_make_current_thread_data());
            current_thread_data->tss_data.insert(std::make_pair(key,tss_data_node(func,tss_data)));
        }

        void erase_tss_node(void const* key)
        {
            detail::thread_data_base* const current_thread_data(get_or_make_current_thread_data());
            current_thread_data->tss_data.erase(key);
        }

        void set_tss_data(void const* key,
                          boost::shared_ptr<tss_cleanup_function> func,
                          void* tss_data,bool cleanup_existing)
        {
            if(tss_data_node* const current_node=find_tss_data(key))
            {
                if(cleanup_existing && current_node->func && (current_node->value!=0))
                {
                    (*current_node->func)(current_node->value);
                }
                if(func || (tss_data!=0))
                {
                    current_node->func=func;
                    current_node->value=tss_data;
                }
                else
                {
                    erase_tss_node(key);
                }
            }
            else if(func || (tss_data!=0))
            {
                add_new_tss_node(key,func,tss_data);
            }
        }
    }
    BOOST_THREAD_DECL void __cdecl on_process_enter()
    {}

    BOOST_THREAD_DECL void __cdecl on_thread_enter()
    {}

    BOOST_THREAD_DECL void __cdecl on_process_exit()
    {
        boost::cleanup_tls_key();
    }

    BOOST_THREAD_DECL void __cdecl on_thread_exit()
    {
        boost::run_thread_exit_callbacks();
    }

    BOOST_THREAD_DECL void notify_all_at_thread_exit(condition_variable& cond, unique_lock<mutex> lk)
    {
      detail::thread_data_base* const current_thread_data(detail::get_current_thread_data());
      if(current_thread_data)
      {
        current_thread_data->notify_all_at_thread_exit(&cond, lk.release());
      }
    }
//namespace detail {
//
//    void BOOST_THREAD_DECL make_ready_at_thread_exit(shared_ptr<shared_state_base> as)
//    {
//      detail::thread_data_base* const current_thread_data(detail::get_current_thread_data());
//      if(current_thread_data)
//      {
//        current_thread_data->make_ready_at_thread_exit(as);
//      }
//    }
//}
}

