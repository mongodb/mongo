/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   once_block.cpp
 * \author Andrey Semashev
 * \date   23.06.2010
 *
 * \brief  This file is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 *
 * The code in this file is based on the \c call_once function implementation in Boost.Thread.
 */

#include <boost/log/detail/config.hpp>
#include <boost/log/utility/once_block.hpp>
#ifndef BOOST_LOG_NO_THREADS

#include <cstdlib>
#include <boost/assert.hpp>

#if defined(BOOST_THREAD_PLATFORM_WIN32)

#include <boost/winapi/wait.hpp> // INFINITE

#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

#include <boost/winapi/srw_lock.hpp>
#include <boost/winapi/condition_variable.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

boost::winapi::SRWLOCK_ g_OnceBlockMutex = BOOST_WINAPI_SRWLOCK_INIT;
boost::winapi::CONDITION_VARIABLE_ g_OnceBlockCond = BOOST_WINAPI_CONDITION_VARIABLE_INIT;

} // namespace

BOOST_LOG_API bool once_block_sentry::enter_once_block() const BOOST_NOEXCEPT
{
    boost::winapi::AcquireSRWLockExclusive(&g_OnceBlockMutex);

    once_block_flag volatile& flag = m_flag;
    while (flag.status != once_block_flag::initialized)
    {
        if (flag.status == once_block_flag::uninitialized)
        {
            flag.status = once_block_flag::being_initialized;
            boost::winapi::ReleaseSRWLockExclusive(&g_OnceBlockMutex);

            // Invoke the initializer block
            return false;
        }
        else
        {
            while (flag.status == once_block_flag::being_initialized)
            {
                BOOST_VERIFY(boost::winapi::SleepConditionVariableSRW(
                    &g_OnceBlockCond, &g_OnceBlockMutex, boost::winapi::INFINITE_, 0));
            }
        }
    }

    boost::winapi::ReleaseSRWLockExclusive(&g_OnceBlockMutex);

    return true;
}

BOOST_LOG_API void once_block_sentry::commit() BOOST_NOEXCEPT
{
    boost::winapi::AcquireSRWLockExclusive(&g_OnceBlockMutex);

    // The initializer executed successfully
    m_flag.status = once_block_flag::initialized;

    boost::winapi::ReleaseSRWLockExclusive(&g_OnceBlockMutex);
    boost::winapi::WakeAllConditionVariable(&g_OnceBlockCond);
}

BOOST_LOG_API void once_block_sentry::rollback() BOOST_NOEXCEPT
{
    boost::winapi::AcquireSRWLockExclusive(&g_OnceBlockMutex);

    // The initializer failed, marking the flag as if it hasn't run at all
    m_flag.status = once_block_flag::uninitialized;

    boost::winapi::ReleaseSRWLockExclusive(&g_OnceBlockMutex);
    boost::winapi::WakeAllConditionVariable(&g_OnceBlockCond);
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#else // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

#include <cstdlib> // atexit
#include <mutex>
#include <condition_variable>
#include <boost/detail/interlocked.hpp>
#include <boost/winapi/basic_types.hpp>
#include <boost/winapi/dll.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

    struct BOOST_LOG_NO_VTABLE once_block_impl_base
    {
        virtual ~once_block_impl_base() {}
        virtual bool enter_once_block(once_block_flag volatile& flag) = 0;
        virtual void commit(once_block_flag& flag) = 0;
        virtual void rollback(once_block_flag& flag) = 0;
    };

    class once_block_impl_nt6 :
        public once_block_impl_base
    {
    public:
        struct winapi_srwlock { void* p; };
        struct winapi_condition_variable { void* p; };

        typedef void (BOOST_WINAPI_WINAPI_CC *InitializeSRWLock_t)(winapi_srwlock*);
        typedef void (BOOST_WINAPI_WINAPI_CC *AcquireSRWLockExclusive_t)(winapi_srwlock*);
        typedef void (BOOST_WINAPI_WINAPI_CC *ReleaseSRWLockExclusive_t)(winapi_srwlock*);
        typedef void (BOOST_WINAPI_WINAPI_CC *InitializeConditionVariable_t)(winapi_condition_variable*);
        typedef boost::winapi::BOOL_ (BOOST_WINAPI_WINAPI_CC *SleepConditionVariableSRW_t)(winapi_condition_variable*, winapi_srwlock*, boost::winapi::DWORD_, boost::winapi::ULONG_);
        typedef void (BOOST_WINAPI_WINAPI_CC *WakeAllConditionVariable_t)(winapi_condition_variable*);

    private:
        winapi_srwlock m_Mutex;
        winapi_condition_variable m_Cond;

        AcquireSRWLockExclusive_t m_pAcquireSRWLockExclusive;
        ReleaseSRWLockExclusive_t m_pReleaseSRWLockExclusive;
        SleepConditionVariableSRW_t m_pSleepConditionVariableSRW;
        WakeAllConditionVariable_t m_pWakeAllConditionVariable;

    public:
        once_block_impl_nt6(
            InitializeSRWLock_t pInitializeSRWLock,
            AcquireSRWLockExclusive_t pAcquireSRWLockExclusive,
            ReleaseSRWLockExclusive_t pReleaseSRWLockExclusive,
            InitializeConditionVariable_t pInitializeConditionVariable,
            SleepConditionVariableSRW_t pSleepConditionVariableSRW,
            WakeAllConditionVariable_t pWakeAllConditionVariable
        ) :
            m_pAcquireSRWLockExclusive(pAcquireSRWLockExclusive),
            m_pReleaseSRWLockExclusive(pReleaseSRWLockExclusive),
            m_pSleepConditionVariableSRW(pSleepConditionVariableSRW),
            m_pWakeAllConditionVariable(pWakeAllConditionVariable)
        {
            pInitializeSRWLock(&m_Mutex);
            pInitializeConditionVariable(&m_Cond);
        }

        bool enter_once_block(once_block_flag volatile& flag)
        {
            m_pAcquireSRWLockExclusive(&m_Mutex);

            while (flag.status != once_block_flag::initialized)
            {
                if (flag.status == once_block_flag::uninitialized)
                {
                    flag.status = once_block_flag::being_initialized;
                    m_pReleaseSRWLockExclusive(&m_Mutex);

                    // Invoke the initializer block
                    return false;
                }
                else
                {
                    while (flag.status == once_block_flag::being_initialized)
                    {
                        BOOST_VERIFY(m_pSleepConditionVariableSRW(
                            &m_Cond, &m_Mutex, boost::winapi::INFINITE_, 0));
                    }
                }
            }

            m_pReleaseSRWLockExclusive(&m_Mutex);

            return true;
        }

        void commit(once_block_flag& flag)
        {
            m_pAcquireSRWLockExclusive(&m_Mutex);

            // The initializer executed successfully
            flag.status = once_block_flag::initialized;

            m_pReleaseSRWLockExclusive(&m_Mutex);
            m_pWakeAllConditionVariable(&m_Cond);
        }

        void rollback(once_block_flag& flag)
        {
            m_pAcquireSRWLockExclusive(&m_Mutex);

            // The initializer failed, marking the flag as if it hasn't run at all
            flag.status = once_block_flag::uninitialized;

            m_pReleaseSRWLockExclusive(&m_Mutex);
            m_pWakeAllConditionVariable(&m_Cond);
        }
    };

    class once_block_impl_nt5 :
        public once_block_impl_base
    {
    private:
        std::mutex m_Mutex;
        std::condition_variable m_Cond;

    public:
        bool enter_once_block(once_block_flag volatile& flag)
        {
            std::unique_lock< std::mutex > lock(m_Mutex);

            while (flag.status != once_block_flag::initialized)
            {
                if (flag.status == once_block_flag::uninitialized)
                {
                    flag.status = once_block_flag::being_initialized;

                    // Invoke the initializer block
                    return false;
                }
                else
                {
                    while (flag.status == once_block_flag::being_initialized)
                    {
                        m_Cond.wait(lock);
                    }
                }
            }

            return true;
        }

        void commit(once_block_flag& flag)
        {
            {
                std::lock_guard< std::mutex > lock(m_Mutex);
                flag.status = once_block_flag::initialized;
            }
            m_Cond.notify_all();
        }

        void rollback(once_block_flag& flag)
        {
            {
                std::lock_guard< std::mutex > lock(m_Mutex);
                flag.status = once_block_flag::uninitialized;
            }
            m_Cond.notify_all();
        }
    };

    once_block_impl_base* create_once_block_impl()
    {
        boost::winapi::HMODULE_ hKernel32 = boost::winapi::GetModuleHandleW(L"kernel32.dll");
        if (hKernel32)
        {
            once_block_impl_nt6::InitializeSRWLock_t pInitializeSRWLock =
                (once_block_impl_nt6::InitializeSRWLock_t)boost::winapi::get_proc_address(hKernel32, "InitializeSRWLock");
            if (pInitializeSRWLock)
            {
                once_block_impl_nt6::AcquireSRWLockExclusive_t pAcquireSRWLockExclusive =
                    (once_block_impl_nt6::AcquireSRWLockExclusive_t)boost::winapi::get_proc_address(hKernel32, "AcquireSRWLockExclusive");
                if (pAcquireSRWLockExclusive)
                {
                    once_block_impl_nt6::ReleaseSRWLockExclusive_t pReleaseSRWLockExclusive =
                        (once_block_impl_nt6::ReleaseSRWLockExclusive_t)boost::winapi::get_proc_address(hKernel32, "ReleaseSRWLockExclusive");
                    if (pReleaseSRWLockExclusive)
                    {
                        once_block_impl_nt6::InitializeConditionVariable_t pInitializeConditionVariable =
                            (once_block_impl_nt6::InitializeConditionVariable_t)boost::winapi::get_proc_address(hKernel32, "InitializeConditionVariable");
                        if (pInitializeConditionVariable)
                        {
                            once_block_impl_nt6::SleepConditionVariableSRW_t pSleepConditionVariableSRW =
                                (once_block_impl_nt6::SleepConditionVariableSRW_t)boost::winapi::get_proc_address(hKernel32, "SleepConditionVariableSRW");
                            if (pSleepConditionVariableSRW)
                            {
                                once_block_impl_nt6::WakeAllConditionVariable_t pWakeAllConditionVariable =
                                    (once_block_impl_nt6::WakeAllConditionVariable_t)boost::winapi::get_proc_address(hKernel32, "WakeAllConditionVariable");
                                if (pWakeAllConditionVariable)
                                {
                                    return new once_block_impl_nt6(
                                        pInitializeSRWLock,
                                        pAcquireSRWLockExclusive,
                                        pReleaseSRWLockExclusive,
                                        pInitializeConditionVariable,
                                        pSleepConditionVariableSRW,
                                        pWakeAllConditionVariable);
                                }
                            }
                        }
                    }
                }
            }
        }

        return new once_block_impl_nt5();
    }

    once_block_impl_base* g_pOnceBlockImpl = NULL;

    void destroy_once_block_impl()
    {
        once_block_impl_base* impl = (once_block_impl_base*)
            BOOST_INTERLOCKED_EXCHANGE_POINTER((void**)&g_pOnceBlockImpl, NULL);
        delete impl;
    }

    once_block_impl_base* get_once_block_impl() BOOST_NOEXCEPT
    {
        once_block_impl_base* impl = g_pOnceBlockImpl;
        if (!impl) try
        {
            once_block_impl_base* new_impl = create_once_block_impl();
            impl = (once_block_impl_base*)
                BOOST_INTERLOCKED_COMPARE_EXCHANGE_POINTER((void**)&g_pOnceBlockImpl, (void*)new_impl, NULL);
            if (impl)
            {
                delete new_impl;
            }
            else
            {
                std::atexit(&destroy_once_block_impl);
                return new_impl;
            }
        }
        catch (...)
        {
            BOOST_ASSERT_MSG(false, "Boost.Log: Failed to initialize the once block thread synchronization structures");
            std::abort();
        }

        return impl;
    }

} // namespace

BOOST_LOG_API bool once_block_sentry::enter_once_block() const BOOST_NOEXCEPT
{
    return get_once_block_impl()->enter_once_block(m_flag);
}

BOOST_LOG_API void once_block_sentry::commit() BOOST_NOEXCEPT
{
    get_once_block_impl()->commit(m_flag);
}

BOOST_LOG_API void once_block_sentry::rollback() BOOST_NOEXCEPT
{
    get_once_block_impl()->rollback(m_flag);
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

#elif defined(BOOST_THREAD_PLATFORM_PTHREAD)

#include <pthread.h>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

static pthread_mutex_t g_OnceBlockMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_OnceBlockCond = PTHREAD_COND_INITIALIZER;

} // namespace

BOOST_LOG_API bool once_block_sentry::enter_once_block() const BOOST_NOEXCEPT
{
    BOOST_VERIFY(!pthread_mutex_lock(&g_OnceBlockMutex));

    once_block_flag volatile& flag = m_flag;
    while (flag.status != once_block_flag::initialized)
    {
        if (flag.status == once_block_flag::uninitialized)
        {
            flag.status = once_block_flag::being_initialized;
            BOOST_VERIFY(!pthread_mutex_unlock(&g_OnceBlockMutex));

            // Invoke the initializer block
            return false;
        }
        else
        {
            while (flag.status == once_block_flag::being_initialized)
            {
                BOOST_VERIFY(!pthread_cond_wait(&g_OnceBlockCond, &g_OnceBlockMutex));
            }
        }
    }

    BOOST_VERIFY(!pthread_mutex_unlock(&g_OnceBlockMutex));

    return true;
}

BOOST_LOG_API void once_block_sentry::commit() BOOST_NOEXCEPT
{
    BOOST_VERIFY(!pthread_mutex_lock(&g_OnceBlockMutex));

    // The initializer executed successfully
    m_flag.status = once_block_flag::initialized;

    BOOST_VERIFY(!pthread_mutex_unlock(&g_OnceBlockMutex));
    BOOST_VERIFY(!pthread_cond_broadcast(&g_OnceBlockCond));
}

BOOST_LOG_API void once_block_sentry::rollback() BOOST_NOEXCEPT
{
    BOOST_VERIFY(!pthread_mutex_lock(&g_OnceBlockMutex));

    // The initializer failed, marking the flag as if it hasn't run at all
    m_flag.status = once_block_flag::uninitialized;

    BOOST_VERIFY(!pthread_mutex_unlock(&g_OnceBlockMutex));
    BOOST_VERIFY(!pthread_cond_broadcast(&g_OnceBlockCond));
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#else
#error Boost.Log: unsupported threading API
#endif

#endif // BOOST_LOG_NO_THREADS
