#ifndef BOOST_THREAD_WIN32_ONCE_HPP
#define BOOST_THREAD_WIN32_ONCE_HPP

//  once.hpp
//
//  (C) Copyright 2005-7 Anthony Williams 
//  (C) Copyright 2005 John Maddock
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <cstring>
#include <cstddef>
#include <boost/assert.hpp>
#include <boost/static_assert.hpp>
#include <boost/detail/interlocked.hpp>
#include <boost/thread/win32/thread_primitives.hpp>
#include <boost/thread/win32/interlocked_read.hpp>

#include <boost/config/abi_prefix.hpp>

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std
{
    using ::memcpy;
    using ::ptrdiff_t;
}
#endif

namespace boost
{
    struct once_flag
    {
        long status;
        long count;
    };

#define BOOST_ONCE_INIT {0,0}

    namespace detail
    {
#ifdef BOOST_NO_ANSI_APIS
        typedef wchar_t once_char_type;
#else
        typedef char once_char_type;
#endif
        unsigned const once_mutex_name_fixed_length=54;
        unsigned const once_mutex_name_length=once_mutex_name_fixed_length+
            sizeof(void*)*2+sizeof(unsigned long)*2+1;

        template <class I>
        void int_to_string(I p, once_char_type* buf)
        {
            for(unsigned i=0; i < sizeof(I)*2; ++i,++buf)
            {
#ifdef BOOST_NO_ANSI_APIS
                once_char_type const a=L'A';
#else
                once_char_type const a='A';
#endif
                *buf = a + static_cast<once_char_type>((p >> (i*4)) & 0x0f);
            }
            *buf = 0;
        }

        inline void name_once_mutex(once_char_type* mutex_name,void* flag_address)
        {
#ifdef BOOST_NO_ANSI_APIS
            static const once_char_type fixed_mutex_name[]=L"Local\\{C15730E2-145C-4c5e-B005-3BC753F42475}-once-flag";
#else
            static const once_char_type fixed_mutex_name[]="Local\\{C15730E2-145C-4c5e-B005-3BC753F42475}-once-flag";
#endif
            BOOST_STATIC_ASSERT(sizeof(fixed_mutex_name) == 
                                (sizeof(once_char_type)*(once_mutex_name_fixed_length+1)));
            
            std::memcpy(mutex_name,fixed_mutex_name,sizeof(fixed_mutex_name));
            detail::int_to_string(reinterpret_cast<std::ptrdiff_t>(flag_address), 
                                  mutex_name + once_mutex_name_fixed_length);
            detail::int_to_string(win32::GetCurrentProcessId(), 
                                  mutex_name + once_mutex_name_fixed_length + sizeof(void*)*2);
        }
                        
        inline void* open_once_event(once_char_type* mutex_name,void* flag_address)
        {
            if(!*mutex_name)
            {
                name_once_mutex(mutex_name,flag_address);
            }
            
#ifdef BOOST_NO_ANSI_APIS                        
            return ::boost::detail::win32::OpenEventW(
#else
            return ::boost::detail::win32::OpenEventA(
#endif
                ::boost::detail::win32::synchronize | 
                ::boost::detail::win32::event_modify_state,
                false,
                mutex_name);
        }

        inline void* create_once_event(once_char_type* mutex_name,void* flag_address)
        {
            if(!*mutex_name)
            {
                name_once_mutex(mutex_name,flag_address);
            }
#ifdef BOOST_NO_ANSI_APIS                        
            return ::boost::detail::win32::CreateEventW(
#else
            return ::boost::detail::win32::CreateEventA(
#endif
                0,::boost::detail::win32::manual_reset_event,
                ::boost::detail::win32::event_initially_reset,
                mutex_name);
        }
    }
    

    template<typename Function>
    void call_once(once_flag& flag,Function f)
    {
        // Try for a quick win: if the procedure has already been called
        // just skip through:
        long const function_complete_flag_value=0xc15730e2;
        long const running_value=0x7f0725e3;
        long status;
        bool counted=false;
        detail::win32::handle_manager event_handle;
        detail::once_char_type mutex_name[detail::once_mutex_name_length];
        mutex_name[0]=0;

        while((status=::boost::detail::interlocked_read_acquire(&flag.status))
              !=function_complete_flag_value)
        {
            status=BOOST_INTERLOCKED_COMPARE_EXCHANGE(&flag.status,running_value,0);
            if(!status)
            {
                try
                {
                    if(!event_handle)
                    {
                        event_handle=detail::open_once_event(mutex_name,&flag);
                    }
                    if(event_handle)
                    {
                        ::boost::detail::win32::ResetEvent(event_handle);
                    }
                    f();
                    if(!counted)
                    {
                        BOOST_INTERLOCKED_INCREMENT(&flag.count);
                        counted=true;
                    }
                    BOOST_INTERLOCKED_EXCHANGE(&flag.status,function_complete_flag_value);
                    if(!event_handle && 
                       (::boost::detail::interlocked_read_acquire(&flag.count)>1))
                    {
                        event_handle=detail::create_once_event(mutex_name,&flag);
                    }
                    if(event_handle)
                    {
                        ::boost::detail::win32::SetEvent(event_handle);
                    }
                    break;
                }
                catch(...)
                {
                    BOOST_INTERLOCKED_EXCHANGE(&flag.status,0);
                    if(!event_handle)
                    {
                        event_handle=detail::open_once_event(mutex_name,&flag);
                    }
                    if(event_handle)
                    {
                        ::boost::detail::win32::SetEvent(event_handle);
                    }
                    throw;
                }
            }

            if(!counted)
            {
                BOOST_INTERLOCKED_INCREMENT(&flag.count);
                counted=true;
                status=::boost::detail::interlocked_read_acquire(&flag.status);
                if(status==function_complete_flag_value)
                {
                    break;
                }
                if(!event_handle)
                {
                    event_handle=detail::create_once_event(mutex_name,&flag);
                    continue;
                }
            }
            BOOST_VERIFY(!::boost::detail::win32::WaitForSingleObject(
                             event_handle,::boost::detail::win32::infinite));
        }
    }
}

#include <boost/config/abi_suffix.hpp>

#endif
