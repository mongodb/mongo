/*
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   windows/ipc_sync_wrappers.hpp
 * \author Andrey Semashev
 * \date   23.01.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WINDOWS_IPC_SYNC_WRAPPERS_HPP_INCLUDED_
#define BOOST_LOG_WINDOWS_IPC_SYNC_WRAPPERS_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <boost/winapi/access_rights.hpp>
#include <boost/winapi/handles.hpp>
#include <boost/winapi/event.hpp>
#include <boost/winapi/semaphore.hpp>
#include <boost/winapi/wait.hpp>
#include <boost/winapi/dll.hpp>
#include <boost/winapi/time.hpp>
#include <boost/winapi/get_last_error.hpp>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <boost/assert.hpp>
#include <boost/throw_exception.hpp>
#include <boost/checked_delete.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/atomic.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/intrusive/set_hook.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/list_hook.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/utility/permissions.hpp>
#include "windows/auto_handle.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace ipc {

namespace aux {

// TODO: Port to Boost.Atomic when it supports extended atomic ops
#if defined(BOOST_MSVC) && (_MSC_VER >= 1400) && !defined(UNDER_CE)

#if _MSC_VER == 1400
extern "C" unsigned char _interlockedbittestandset(long *a, long b);
extern "C" unsigned char _interlockedbittestandreset(long *a, long b);
#else
extern "C" unsigned char _interlockedbittestandset(volatile long *a, long b);
extern "C" unsigned char _interlockedbittestandreset(volatile long *a, long b);
#endif

#pragma intrinsic(_interlockedbittestandset)
#pragma intrinsic(_interlockedbittestandreset)

BOOST_FORCEINLINE bool bit_test_and_set(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    return _interlockedbittestandset(reinterpret_cast< long* >(&x.storage()), static_cast< long >(bit)) != 0;
}

BOOST_FORCEINLINE bool bit_test_and_reset(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    return _interlockedbittestandreset(reinterpret_cast< long* >(&x.storage()), static_cast< long >(bit)) != 0;
}

#elif (defined(BOOST_MSVC) || defined(BOOST_INTEL_WIN)) && defined(_M_IX86)

BOOST_FORCEINLINE bool bit_test_and_set(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    boost::atomic< uint32_t >::storage_type* p = &x.storage();
    bool ret;
    __asm
    {
        mov eax, bit
        mov edx, p
        lock bts [edx], eax
        setc ret
    };
    return ret;
}

BOOST_FORCEINLINE bool bit_test_and_reset(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    boost::atomic< uint32_t >::storage_type* p = &x.storage();
    bool ret;
    __asm
    {
        mov eax, bit
        mov edx, p
        lock btr [edx], eax
        setc ret
    };
    return ret;
}

#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))

#if !defined(__CUDACC__)
#define BOOST_LOG_DETAIL_ASM_CLOBBER_CC_COMMA "cc",
#else
#define BOOST_LOG_DETAIL_ASM_CLOBBER_CC_COMMA
#endif

BOOST_FORCEINLINE bool bit_test_and_set(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    bool res;
    __asm__ __volatile__
    (
        "lock; bts %[bit_number], %[storage]\n\t"
        "setc %[result]\n\t"
        : [storage] "+m" (x.storage()), [result] "=q" (res)
        : [bit_number] "Kq" (bit)
        : BOOST_LOG_DETAIL_ASM_CLOBBER_CC_COMMA "memory"
    );
    return res;
}

BOOST_FORCEINLINE bool bit_test_and_reset(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    bool res;
    __asm__ __volatile__
    (
        "lock; btr %[bit_number], %[storage]\n\t"
        "setc %[result]\n\t"
        : [storage] "+m" (x.storage()), [result] "=q" (res)
        : [bit_number] "Kq" (bit)
        : BOOST_LOG_DETAIL_ASM_CLOBBER_CC_COMMA "memory"
    );
    return res;
}

#else

BOOST_FORCEINLINE bool bit_test_and_set(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    const uint32_t mask = uint32_t(1u) << bit;
    uint32_t old_val = x.fetch_or(mask, boost::memory_order_acq_rel);
    return (old_val & mask) != 0u;
}

BOOST_FORCEINLINE bool bit_test_and_reset(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    const uint32_t mask = uint32_t(1u) << bit;
    uint32_t old_val = x.fetch_and(~mask, boost::memory_order_acq_rel);
    return (old_val & mask) != 0u;
}

#endif

//! Interprocess event object
class interprocess_event
{
private:
    auto_handle m_event;

public:
    void create(const wchar_t* name, bool manual_reset, permissions const& perms = permissions());
    void create_or_open(const wchar_t* name, bool manual_reset, permissions const& perms = permissions());
    void open(const wchar_t* name);

    boost::winapi::HANDLE_ get_handle() const BOOST_NOEXCEPT { return m_event.get(); }

    void set()
    {
        if (BOOST_UNLIKELY(!boost::winapi::SetEvent(m_event.get())))
        {
            const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to set an interprocess event object", (err));
        }
    }

    void set_noexcept() BOOST_NOEXCEPT
    {
        BOOST_VERIFY(!!boost::winapi::SetEvent(m_event.get()));
    }

    void reset()
    {
        if (BOOST_UNLIKELY(!boost::winapi::ResetEvent(m_event.get())))
        {
            const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to reset an interprocess event object", (err));
        }
    }

    void wait()
    {
        const boost::winapi::DWORD_ retval = boost::winapi::WaitForSingleObject(m_event.get(), boost::winapi::infinite);
        if (BOOST_UNLIKELY(retval != boost::winapi::wait_object_0))
        {
            const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to block on an interprocess event object", (err));
        }
    }

    bool wait(boost::winapi::HANDLE_ abort_handle)
    {
        boost::winapi::HANDLE_ handles[2u] = { m_event.get(), abort_handle };
        const boost::winapi::DWORD_ retval = boost::winapi::WaitForMultipleObjects(2u, handles, false, boost::winapi::infinite);
        if (retval == (boost::winapi::wait_object_0 + 1u))
        {
            // Wait was interrupted
            return false;
        }
        else if (BOOST_UNLIKELY(retval != boost::winapi::wait_object_0))
        {
            const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to block on an interprocess event object", (err));
        }

        return true;
    }

    void swap(interprocess_event& that) BOOST_NOEXCEPT
    {
        m_event.swap(that.m_event);
    }
};

//! Interprocess semaphore object
class interprocess_semaphore
{
private:
    typedef boost::winapi::DWORD_ NTSTATUS_;
    struct semaphore_basic_information
    {
        boost::winapi::ULONG_ current_count; // current semaphore count
        boost::winapi::ULONG_ maximum_count; // max semaphore count
    };
    typedef NTSTATUS_ (__stdcall *nt_query_semaphore_t)(boost::winapi::HANDLE_ h, unsigned int info_class, semaphore_basic_information* pinfo, boost::winapi::ULONG_ info_size, boost::winapi::ULONG_* ret_len);
    typedef bool (*is_semaphore_zero_count_t)(boost::winapi::HANDLE_ h);

private:
    auto_handle m_sem;

    static boost::atomic< is_semaphore_zero_count_t > is_semaphore_zero_count;
    static nt_query_semaphore_t nt_query_semaphore;

public:
    void create_or_open(const wchar_t* name, permissions const& perms = permissions());
    void open(const wchar_t* name);

    boost::winapi::HANDLE_ get_handle() const BOOST_NOEXCEPT { return m_sem.get(); }

    void post(uint32_t count)
    {
        BOOST_ASSERT(count <= static_cast< uint32_t >((std::numeric_limits< boost::winapi::LONG_ >::max)()));

        if (BOOST_UNLIKELY(!boost::winapi::ReleaseSemaphore(m_sem.get(), static_cast< boost::winapi::LONG_ >(count), NULL)))
        {
            const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to post on an interprocess semaphore object", (err));
        }
    }

    bool is_zero_count() const
    {
        return is_semaphore_zero_count.load(boost::memory_order_acquire)(m_sem.get());
    }

    void wait()
    {
        const boost::winapi::DWORD_ retval = boost::winapi::WaitForSingleObject(m_sem.get(), boost::winapi::infinite);
        if (BOOST_UNLIKELY(retval != boost::winapi::wait_object_0))
        {
            const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to block on an interprocess semaphore object", (err));
        }
    }

    bool wait(boost::winapi::HANDLE_ abort_handle)
    {
        boost::winapi::HANDLE_ handles[2u] = { m_sem.get(), abort_handle };
        const boost::winapi::DWORD_ retval = boost::winapi::WaitForMultipleObjects(2u, handles, false, boost::winapi::infinite);
        if (retval == (boost::winapi::wait_object_0 + 1u))
        {
            // Wait was interrupted
            return false;
        }
        else if (BOOST_UNLIKELY(retval != boost::winapi::wait_object_0))
        {
            const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to block on an interprocess semaphore object", (err));
        }

        return true;
    }

    void swap(interprocess_semaphore& that) BOOST_NOEXCEPT
    {
        m_sem.swap(that.m_sem);
    }

private:
    static bool is_semaphore_zero_count_init(boost::winapi::HANDLE_ h);
    static bool is_semaphore_zero_count_nt_query_semaphore(boost::winapi::HANDLE_ h);
    static bool is_semaphore_zero_count_emulated(boost::winapi::HANDLE_ h);
};

//! Interprocess mutex. Implementation adopted from Boost.Sync.
class interprocess_mutex
{
public:
    //! Shared state that should be visible to all processes using the mutex
    struct shared_state
    {
        boost::atomic< uint32_t > m_lock_state;

        shared_state() BOOST_NOEXCEPT : m_lock_state(0u)
        {
        }
    };

    struct auto_unlock
    {
        explicit auto_unlock(interprocess_mutex& mutex) BOOST_NOEXCEPT : m_mutex(mutex) {}
        ~auto_unlock() { m_mutex.unlock(); }

        BOOST_DELETED_FUNCTION(auto_unlock(auto_unlock const&))
        BOOST_DELETED_FUNCTION(auto_unlock& operator=(auto_unlock const&))

    private:
        interprocess_mutex& m_mutex;
    };

    struct optional_unlock
    {
        optional_unlock() BOOST_NOEXCEPT : m_mutex(NULL) {}
        explicit optional_unlock(interprocess_mutex& mutex) BOOST_NOEXCEPT : m_mutex(&mutex) {}
        ~optional_unlock() { if (m_mutex) m_mutex->unlock(); }

        interprocess_mutex* disengage() BOOST_NOEXCEPT
        {
            interprocess_mutex* p = m_mutex;
            m_mutex = NULL;
            return p;
        }

        void engage(interprocess_mutex& mutex) BOOST_NOEXCEPT
        {
            BOOST_ASSERT(!m_mutex);
            m_mutex = &mutex;
        }

        BOOST_DELETED_FUNCTION(optional_unlock(optional_unlock const&))
        BOOST_DELETED_FUNCTION(optional_unlock& operator=(optional_unlock const&))

    private:
        interprocess_mutex* m_mutex;
    };

private:
    interprocess_event m_event;
    shared_state* m_shared_state;

#if !defined(BOOST_MSVC) || _MSC_VER >= 1800
    static BOOST_CONSTEXPR_OR_CONST uint32_t lock_flag_bit = 31u;
    static BOOST_CONSTEXPR_OR_CONST uint32_t event_set_flag_bit = 30u;
    static BOOST_CONSTEXPR_OR_CONST uint32_t lock_flag_value = 1u << lock_flag_bit;
    static BOOST_CONSTEXPR_OR_CONST uint32_t event_set_flag_value = 1u << event_set_flag_bit;
    static BOOST_CONSTEXPR_OR_CONST uint32_t waiter_count_mask = event_set_flag_value - 1u;
#else
    // MSVC 8-11, inclusively, fail to link if these constants are declared as static constants instead of an enum
    enum
    {
        lock_flag_bit = 31u,
        event_set_flag_bit = 30u,
        lock_flag_value = 1u << lock_flag_bit,
        event_set_flag_value = 1u << event_set_flag_bit,
        waiter_count_mask = event_set_flag_value - 1u
    };
#endif

public:
    interprocess_mutex() BOOST_NOEXCEPT : m_shared_state(NULL)
    {
    }

    void create(const wchar_t* name, shared_state* shared, permissions const& perms = permissions())
    {
        m_event.create(name, false, perms);
        m_shared_state = shared;
    }

    void open(const wchar_t* name, shared_state* shared)
    {
        m_event.open(name);
        m_shared_state = shared;
    }

    bool try_lock()
    {
        return !bit_test_and_set(m_shared_state->m_lock_state, lock_flag_bit);
    }

    void lock()
    {
        if (BOOST_UNLIKELY(!try_lock()))
            lock_slow();
    }

    bool lock(boost::winapi::HANDLE_ abort_handle)
    {
        if (BOOST_LIKELY(try_lock()))
            return true;
        return lock_slow(abort_handle);
    }

    void unlock() BOOST_NOEXCEPT
    {
        const uint32_t old_count = m_shared_state->m_lock_state.fetch_add(lock_flag_value, boost::memory_order_release);
        if ((old_count & event_set_flag_value) == 0u && (old_count > lock_flag_value))
        {
            if (!bit_test_and_set(m_shared_state->m_lock_state, event_set_flag_bit))
            {
                m_event.set_noexcept();
            }
        }
    }

    BOOST_DELETED_FUNCTION(interprocess_mutex(interprocess_mutex const&))
    BOOST_DELETED_FUNCTION(interprocess_mutex& operator=(interprocess_mutex const&))

private:
    void lock_slow();
    bool lock_slow(boost::winapi::HANDLE_ abort_handle);
    void mark_waiting_and_try_lock(uint32_t& old_state);
    void clear_waiting_and_try_lock(uint32_t& old_state);
};

//! A simple clock that corresponds to GetTickCount/GetTickCount64 timeline
struct tick_count_clock
{
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
    typedef boost::winapi::ULONGLONG_ time_point;
#else
    typedef boost::winapi::DWORD_ time_point;
#endif

    static time_point now() BOOST_NOEXCEPT
    {
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
        return boost::winapi::GetTickCount64();
#else
        return boost::winapi::GetTickCount();
#endif
    }
};

//! Interprocess condition variable
class interprocess_condition_variable
{
private:
    typedef boost::intrusive::list_base_hook<
        boost::intrusive::tag< struct for_sem_order_by_usage >,
        boost::intrusive::link_mode< boost::intrusive::safe_link >
    > semaphore_info_list_hook_t;

    typedef boost::intrusive::set_base_hook<
        boost::intrusive::tag< struct for_sem_lookup_by_id >,
        boost::intrusive::link_mode< boost::intrusive::safe_link >,
        boost::intrusive::optimize_size< true >
    > semaphore_info_set_hook_t;

    //! Information about a semaphore object
    struct semaphore_info :
        public semaphore_info_list_hook_t,
        public semaphore_info_set_hook_t
    {
        struct order_by_id
        {
            typedef bool result_type;

            result_type operator() (semaphore_info const& left, semaphore_info const& right) const BOOST_NOEXCEPT
            {
                return left.m_id < right.m_id;
            }
            result_type operator() (semaphore_info const& left, uint32_t right) const BOOST_NOEXCEPT
            {
                return left.m_id < right;
            }
            result_type operator() (uint32_t left, semaphore_info const& right) const BOOST_NOEXCEPT
            {
                return left < right.m_id;
            }
        };

        //! The semaphore
        interprocess_semaphore m_semaphore;
        //! Timestamp of the moment when the semaphore was checked for zero count and it was not zero. In milliseconds since epoch.
        tick_count_clock::time_point m_last_check_for_zero;
        //! The flag indicates that the semaphore has been checked for zero count and it was not zero
        bool m_checked_for_zero;
        //! The semaphore id
        const uint32_t m_id;

        explicit semaphore_info(uint32_t id) BOOST_NOEXCEPT : m_last_check_for_zero(0u), m_id(id)
        {
        }

        //! Checks if the semaphore is in 'non-zero' state for too long
        bool check_non_zero_timeout(tick_count_clock::time_point now) BOOST_NOEXCEPT
        {
            if (!m_checked_for_zero)
            {
                m_last_check_for_zero = now;
                m_checked_for_zero = true;
                return false;
            }

            return (now - m_last_check_for_zero) >= 2000u;
        }

        BOOST_DELETED_FUNCTION(semaphore_info(semaphore_info const&))
        BOOST_DELETED_FUNCTION(semaphore_info& operator=(semaphore_info const&))
    };

    typedef boost::intrusive::list<
        semaphore_info,
        boost::intrusive::base_hook< semaphore_info_list_hook_t >,
        boost::intrusive::constant_time_size< false >
    > semaphore_info_list;

    typedef boost::intrusive::set<
        semaphore_info,
        boost::intrusive::base_hook< semaphore_info_set_hook_t >,
        boost::intrusive::compare< semaphore_info::order_by_id >,
        boost::intrusive::constant_time_size< false >
    > semaphore_info_set;

public:
    struct shared_state
    {
        //! Number of waiters blocked on the semaphore if >0, 0 if no threads are blocked, <0 when the blocked threads were signalled
        int32_t m_waiters;
        //! The semaphore generation
        uint32_t m_generation;
        //! Id of the semaphore which is used to block threads on
        uint32_t m_semaphore_id;

        shared_state() BOOST_NOEXCEPT :
            m_waiters(0),
            m_generation(0u),
            m_semaphore_id(0u)
        {
        }
    };

private:
    //! The list of semaphores used for blocking. The list is in the order at which the semaphores are considered to be picked for being used.
    semaphore_info_list m_semaphore_info_list;
    //! The list of semaphores used for blocking. Used for searching for a particular semaphore by id.
    semaphore_info_set m_semaphore_info_set;
    //! The semaphore that is currently being used for blocking
    semaphore_info* m_current_semaphore;
    //! A string storage for formatting a semaphore name
    std::wstring m_semaphore_name;
    //! Permissions used to create new semaphores
    permissions m_perms;
    //! Process-shared state
    shared_state* m_shared_state;
    //! The next id for creating a new semaphore
    uint32_t m_next_semaphore_id;

public:
    interprocess_condition_variable() BOOST_NOEXCEPT :
        m_current_semaphore(NULL),
        m_shared_state(NULL),
        m_next_semaphore_id(0u)
    {
    }

    ~interprocess_condition_variable()
    {
        m_semaphore_info_set.clear();
        m_semaphore_info_list.clear_and_dispose(boost::checked_deleter< semaphore_info >());
    }

    void init(const wchar_t* name, shared_state* shared, permissions const& perms = permissions())
    {
        m_perms = perms;
        m_shared_state = shared;

        m_semaphore_name = name;
        // Reserve space for generate_semaphore_name()
        m_semaphore_name.append(L".sem00000000");

        m_current_semaphore = get_semaphore(m_shared_state->m_semaphore_id);
    }

    void notify_all()
    {
        const int32_t waiters = m_shared_state->m_waiters;
        if (waiters > 0)
        {
            const uint32_t id = m_shared_state->m_semaphore_id;
            if (m_current_semaphore->m_id != id)
                m_current_semaphore = get_semaphore(id);

            m_current_semaphore->m_semaphore.post(waiters);
            m_shared_state->m_waiters = -waiters;
        }
    }

    bool wait(interprocess_mutex::optional_unlock& lock, boost::winapi::HANDLE_ abort_handle);

    BOOST_DELETED_FUNCTION(interprocess_condition_variable(interprocess_condition_variable const&))
    BOOST_DELETED_FUNCTION(interprocess_condition_variable& operator=(interprocess_condition_variable const&))

private:
    //! Finds or opens a semaphore with the specified id
    semaphore_info* get_semaphore(uint32_t id);
    //! Finds or creates a semaphore with zero counter
    semaphore_info* get_unused_semaphore();

    //! Marks the semaphore info as unused and moves to the end of list
    void mark_unused(semaphore_info& info) BOOST_NOEXCEPT;

    //! Generates semaphore name according to id
    void generate_semaphore_name(uint32_t id) BOOST_NOEXCEPT;

    //! Returns \c true if \a left is less than \a right considering possible integer overflow
    static bool is_overflow_less(uint32_t left, uint32_t right) BOOST_NOEXCEPT
    {
        return ((left - right) & 0x80000000u) != 0u;
    }
};

} // namespace aux

} // namespace ipc

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WINDOWS_IPC_SYNC_WRAPPERS_HPP_INCLUDED_
