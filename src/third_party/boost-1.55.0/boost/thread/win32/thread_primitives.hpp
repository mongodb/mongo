#ifndef BOOST_WIN32_THREAD_PRIMITIVES_HPP
#define BOOST_WIN32_THREAD_PRIMITIVES_HPP

//  win32_thread_primitives.hpp
//
//  (C) Copyright 2005-7 Anthony Williams
//  (C) Copyright 2007 David Deakins
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/thread/detail/config.hpp>
#include <boost/throw_exception.hpp>
#include <boost/assert.hpp>
#include <boost/thread/exceptions.hpp>
#include <boost/detail/interlocked.hpp>
//#include <boost/detail/winapi/synchronization.hpp>
#include <algorithm>

#ifndef BOOST_THREAD_WIN32_HAS_GET_TICK_COUNT_64
#if _WIN32_WINNT >= 0x0600 && ! defined _WIN32_WINNT_WS08
#define BOOST_THREAD_WIN32_HAS_GET_TICK_COUNT_64
#endif
#endif

#if defined( BOOST_USE_WINDOWS_H )
# include <windows.h>

namespace boost
{
    namespace detail
    {
        namespace win32
        {
#ifdef BOOST_THREAD_WIN32_HAS_GET_TICK_COUNT_64
            typedef unsigned long long ticks_type;
#else
            typedef unsigned long ticks_type;
#endif
            typedef ULONG_PTR ulong_ptr;
            typedef HANDLE handle;
            unsigned const infinite=INFINITE;
            unsigned const timeout=WAIT_TIMEOUT;
            handle const invalid_handle_value=INVALID_HANDLE_VALUE;
            unsigned const event_modify_state=EVENT_MODIFY_STATE;
            unsigned const synchronize=SYNCHRONIZE;
            unsigned const wait_abandoned=WAIT_ABANDONED;


# ifdef BOOST_NO_ANSI_APIS
            using ::CreateMutexW;
            using ::CreateEventW;
            using ::OpenEventW;
            using ::CreateSemaphoreW;
# else
            using ::CreateMutexA;
            using ::CreateEventA;
            using ::OpenEventA;
            using ::CreateSemaphoreA;
# endif
            using ::CloseHandle;
            using ::ReleaseMutex;
            using ::ReleaseSemaphore;
            using ::SetEvent;
            using ::ResetEvent;
            using ::WaitForMultipleObjects;
            using ::WaitForSingleObject;
            using ::GetCurrentProcessId;
            using ::GetCurrentThreadId;
            using ::GetCurrentThread;
            using ::GetCurrentProcess;
            using ::DuplicateHandle;
            using ::SleepEx;
            using ::Sleep;
            using ::QueueUserAPC;
            using ::GetTickCount;
#ifdef BOOST_THREAD_WIN32_HAS_GET_TICK_COUNT_64
            using ::GetTickCount64;
#else
            inline ticks_type GetTickCount64() { return GetTickCount(); }
#endif
        }
    }
}
#elif defined( WIN32 ) || defined( _WIN32 ) || defined( __WIN32__ )

# ifdef UNDER_CE
#  ifndef WINAPI
#   ifndef _WIN32_WCE_EMULATION
#    define WINAPI  __cdecl     // Note this doesn't match the desktop definition
#   else
#    define WINAPI  __stdcall
#   endif
#  endif

#  ifdef __cplusplus
extern "C" {
#  endif
typedef int BOOL;
typedef unsigned long DWORD;
typedef void* HANDLE;

#  include <kfuncs.h>
#  ifdef __cplusplus
}
#  endif
# endif


namespace boost
{
    namespace detail
    {
        namespace win32
        {
#ifdef BOOST_THREAD_WIN32_HAS_GET_TICK_COUNT_64
            typedef unsigned long long ticks_type;
#else
            typedef unsigned long ticks_type;
#endif
# ifdef _WIN64
            typedef unsigned __int64 ulong_ptr;
# else
            typedef unsigned long ulong_ptr;
# endif
            typedef void* handle;
            unsigned const infinite=~0U;
            unsigned const timeout=258U;
            handle const invalid_handle_value=(handle)(-1);
            unsigned const event_modify_state=2;
            unsigned const synchronize=0x100000u;
            unsigned const wait_abandoned=0x00000080u;

            extern "C"
            {
                struct _SECURITY_ATTRIBUTES;
# ifdef BOOST_NO_ANSI_APIS
                __declspec(dllimport) void* __stdcall CreateMutexW(_SECURITY_ATTRIBUTES*,int,wchar_t const*);
                __declspec(dllimport) void* __stdcall CreateSemaphoreW(_SECURITY_ATTRIBUTES*,long,long,wchar_t const*);
                __declspec(dllimport) void* __stdcall CreateEventW(_SECURITY_ATTRIBUTES*,int,int,wchar_t const*);
                __declspec(dllimport) void* __stdcall OpenEventW(unsigned long,int,wchar_t const*);
# else
                __declspec(dllimport) void* __stdcall CreateMutexA(_SECURITY_ATTRIBUTES*,int,char const*);
                __declspec(dllimport) void* __stdcall CreateSemaphoreA(_SECURITY_ATTRIBUTES*,long,long,char const*);
                __declspec(dllimport) void* __stdcall CreateEventA(_SECURITY_ATTRIBUTES*,int,int,char const*);
                __declspec(dllimport) void* __stdcall OpenEventA(unsigned long,int,char const*);
# endif
                __declspec(dllimport) int __stdcall CloseHandle(void*);
                __declspec(dllimport) int __stdcall ReleaseMutex(void*);
                __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void*,unsigned long);
                __declspec(dllimport) unsigned long __stdcall WaitForMultipleObjects(unsigned long nCount,void* const * lpHandles,int bWaitAll,unsigned long dwMilliseconds);
                __declspec(dllimport) int __stdcall ReleaseSemaphore(void*,long,long*);
                __declspec(dllimport) int __stdcall DuplicateHandle(void*,void*,void*,void**,unsigned long,int,unsigned long);
                __declspec(dllimport) unsigned long __stdcall SleepEx(unsigned long,int);
                __declspec(dllimport) void __stdcall Sleep(unsigned long);
                typedef void (__stdcall *queue_user_apc_callback_function)(ulong_ptr);
                __declspec(dllimport) unsigned long __stdcall QueueUserAPC(queue_user_apc_callback_function,void*,ulong_ptr);

                __declspec(dllimport) unsigned long __stdcall GetTickCount();
# ifdef BOOST_THREAD_WIN32_HAS_GET_TICK_COUNT_64
                __declspec(dllimport) ticks_type __stdcall GetTickCount64();
# endif
# ifndef UNDER_CE
                __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId();
                __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
                __declspec(dllimport) void* __stdcall GetCurrentThread();
                __declspec(dllimport) void* __stdcall GetCurrentProcess();
                __declspec(dllimport) int __stdcall SetEvent(void*);
                __declspec(dllimport) int __stdcall ResetEvent(void*);
# else
                using ::GetCurrentProcessId;
                using ::GetCurrentThreadId;
                using ::GetCurrentThread;
                using ::GetCurrentProcess;
                using ::SetEvent;
                using ::ResetEvent;
# endif
            }
# ifndef BOOST_THREAD_WIN32_HAS_GET_TICK_COUNT_64
            inline ticks_type GetTickCount64() { return GetTickCount(); }
# endif
        }
    }
}
#else
# error "Win32 functions not available"
#endif

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    namespace detail
    {
        namespace win32
        {
            enum event_type
            {
                auto_reset_event=false,
                manual_reset_event=true
            };

            enum initial_event_state
            {
                event_initially_reset=false,
                event_initially_set=true
            };

            inline handle create_anonymous_event(event_type type,initial_event_state state)
            {
#if !defined(BOOST_NO_ANSI_APIS)
                handle const res=win32::CreateEventA(0,type,state,0);
#else
                handle const res=win32::CreateEventW(0,type,state,0);
#endif
                if(!res)
                {
                    boost::throw_exception(thread_resource_error());
                }
                return res;
            }

            inline handle create_anonymous_semaphore(long initial_count,long max_count)
            {
#if !defined(BOOST_NO_ANSI_APIS)
                handle const res=CreateSemaphoreA(0,initial_count,max_count,0);
#else
                handle const res=CreateSemaphoreW(0,initial_count,max_count,0);
#endif
                if(!res)
                {
                    boost::throw_exception(thread_resource_error());
                }
                return res;
            }
            inline handle create_anonymous_semaphore_nothrow(long initial_count,long max_count)
            {
#if !defined(BOOST_NO_ANSI_APIS)
                handle const res=CreateSemaphoreA(0,initial_count,max_count,0);
#else
                handle const res=CreateSemaphoreW(0,initial_count,max_count,0);
#endif
                return res;
            }

            inline handle duplicate_handle(handle source)
            {
                handle const current_process=GetCurrentProcess();
                long const same_access_flag=2;
                handle new_handle=0;
                bool const success=DuplicateHandle(current_process,source,current_process,&new_handle,0,false,same_access_flag)!=0;
                if(!success)
                {
                    boost::throw_exception(thread_resource_error());
                }
                return new_handle;
            }

            inline void release_semaphore(handle semaphore,long count)
            {
                BOOST_VERIFY(ReleaseSemaphore(semaphore,count,0)!=0);
            }

            class BOOST_THREAD_DECL handle_manager
            {
            private:
                handle handle_to_manage;
                handle_manager(handle_manager&);
                handle_manager& operator=(handle_manager&);

                void cleanup()
                {
                    if(handle_to_manage && handle_to_manage!=invalid_handle_value)
                    {
                        BOOST_VERIFY(CloseHandle(handle_to_manage));
                    }
                }

            public:
                explicit handle_manager(handle handle_to_manage_):
                    handle_to_manage(handle_to_manage_)
                {}
                handle_manager():
                    handle_to_manage(0)
                {}

                handle_manager& operator=(handle new_handle)
                {
                    cleanup();
                    handle_to_manage=new_handle;
                    return *this;
                }

                operator handle() const
                {
                    return handle_to_manage;
                }

                handle duplicate() const
                {
                    return duplicate_handle(handle_to_manage);
                }

                void swap(handle_manager& other)
                {
                    std::swap(handle_to_manage,other.handle_to_manage);
                }

                handle release()
                {
                    handle const res=handle_to_manage;
                    handle_to_manage=0;
                    return res;
                }

                bool operator!() const
                {
                    return !handle_to_manage;
                }

                ~handle_manager()
                {
                    cleanup();
                }
            };

        }
    }
}

#if defined(BOOST_MSVC) && (_MSC_VER>=1400)  && !defined(UNDER_CE)

namespace boost
{
    namespace detail
    {
        namespace win32
        {
#if _MSC_VER==1400
            extern "C" unsigned char _interlockedbittestandset(long *a,long b);
            extern "C" unsigned char _interlockedbittestandreset(long *a,long b);
#else
            extern "C" unsigned char _interlockedbittestandset(volatile long *a,long b);
            extern "C" unsigned char _interlockedbittestandreset(volatile long *a,long b);
#endif

#pragma intrinsic(_interlockedbittestandset)
#pragma intrinsic(_interlockedbittestandreset)

            inline bool interlocked_bit_test_and_set(long* x,long bit)
            {
                return _interlockedbittestandset(x,bit)!=0;
            }

            inline bool interlocked_bit_test_and_reset(long* x,long bit)
            {
                return _interlockedbittestandreset(x,bit)!=0;
            }

        }
    }
}
#define BOOST_THREAD_BTS_DEFINED
#elif (defined(BOOST_MSVC) || defined(BOOST_INTEL_WIN)) && defined(_M_IX86)
namespace boost
{
    namespace detail
    {
        namespace win32
        {
            inline bool interlocked_bit_test_and_set(long* x,long bit)
            {
#ifndef BOOST_INTEL_CXX_VERSION
                __asm {
                    mov eax,bit;
                    mov edx,x;
                    lock bts [edx],eax;
                    setc al;
                };
#else
                bool ret;
                __asm {
                    mov eax,bit
                    mov edx,x
                    lock bts [edx],eax
                    setc al
                    mov ret, al
                };
                return ret;

#endif
            }

            inline bool interlocked_bit_test_and_reset(long* x,long bit)
            {
#ifndef BOOST_INTEL_CXX_VERSION
                __asm {
                    mov eax,bit;
                    mov edx,x;
                    lock btr [edx],eax;
                    setc al;
                };
#else
                bool ret;
                __asm {
                    mov eax,bit
                    mov edx,x
                    lock btr [edx],eax
                    setc al
                    mov ret, al
                };
                return ret;

#endif
            }

        }
    }
}
#define BOOST_THREAD_BTS_DEFINED
#endif

#ifndef BOOST_THREAD_BTS_DEFINED

namespace boost
{
    namespace detail
    {
        namespace win32
        {
            inline bool interlocked_bit_test_and_set(long* x,long bit)
            {
                long const value=1<<bit;
                long old=*x;
                do
                {
                    long const current=BOOST_INTERLOCKED_COMPARE_EXCHANGE(x,old|value,old);
                    if(current==old)
                    {
                        break;
                    }
                    old=current;
                }
                while(true);
                return (old&value)!=0;
            }

            inline bool interlocked_bit_test_and_reset(long* x,long bit)
            {
                long const value=1<<bit;
                long old=*x;
                do
                {
                    long const current=BOOST_INTERLOCKED_COMPARE_EXCHANGE(x,old&~value,old);
                    if(current==old)
                    {
                        break;
                    }
                    old=current;
                }
                while(true);
                return (old&value)!=0;
            }
        }
    }
}
#endif

#include <boost/config/abi_suffix.hpp>

#endif
