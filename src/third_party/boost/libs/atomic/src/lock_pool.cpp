/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2011 Helge Bahmann
 * Copyright (c) 2013-2014, 2020 Andrey Semashev
 */
/*!
 * \file   lock_pool.cpp
 *
 * This file contains implementation of the lock pool used to emulate atomic ops.
 */

#include <boost/predef/os/windows.h>
#if BOOST_OS_WINDOWS
// Include boost/winapi/config.hpp first to make sure target Windows version is selected by Boost.WinAPI
#include <boost/winapi/config.hpp>
#include <boost/predef/platform.h>
#endif
#include <boost/predef/architecture/x86.h>
#include <boost/predef/hardware/simd/x86.h>

#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <new>
#include <limits>
#include <boost/config.hpp>
#include <boost/assert.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/capabilities.hpp>
#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/intptr.hpp>
#include <boost/atomic/detail/int_sizes.hpp>
#include <boost/atomic/detail/aligned_variable.hpp>
#include <boost/atomic/detail/core_operations.hpp>
#include <boost/atomic/detail/extra_operations.hpp>
#include <boost/atomic/detail/fence_operations.hpp>
#include <boost/atomic/detail/lock_pool.hpp>
#include <boost/atomic/detail/pause.hpp>
#include <boost/atomic/detail/once_flag.hpp>
#include <boost/atomic/detail/type_traits/alignment_of.hpp>

#include <boost/align/aligned_alloc.hpp>

#include <boost/preprocessor/config/limits.hpp>
#include <boost/preprocessor/iteration/iterate.hpp>

#if BOOST_OS_WINDOWS
#include <boost/winapi/basic_types.hpp>
#include <boost/winapi/thread.hpp>
#include <boost/winapi/wait_constants.hpp>
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
#include <boost/winapi/srw_lock.hpp>
#include <boost/winapi/condition_variable.hpp>
#else // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
#include <boost/winapi/critical_section.hpp>
#include <boost/winapi/semaphore.hpp>
#include <boost/winapi/handles.hpp>
#include <boost/winapi/wait.hpp>
#endif // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
#define BOOST_ATOMIC_USE_WINAPI
#else // BOOST_OS_WINDOWS
#include <boost/atomic/detail/futex.hpp>
#if defined(BOOST_ATOMIC_DETAIL_HAS_FUTEX) && BOOST_ATOMIC_INT32_LOCK_FREE == 2
#define BOOST_ATOMIC_USE_FUTEX
#else // BOOST_OS_LINUX
#include <pthread.h>
#define BOOST_ATOMIC_USE_PTHREAD
#endif // BOOST_OS_LINUX
#include <cerrno>
#endif // BOOST_OS_WINDOWS

#include "find_address.hpp"

#if BOOST_ARCH_X86 && (defined(BOOST_ATOMIC_USE_SSE2) || defined(BOOST_ATOMIC_USE_SSE41)) && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && \
    (\
        (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8 && BOOST_HW_SIMD_X86 < BOOST_HW_SIMD_X86_SSE4_1_VERSION) || \
        (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 4 && BOOST_HW_SIMD_X86 < BOOST_HW_SIMD_X86_SSE2_VERSION) \
    )
#include "cpuid.hpp"
#define BOOST_ATOMIC_DETAIL_X86_USE_RUNTIME_DISPATCH
#endif

#include <boost/atomic/detail/header.hpp>

// Cache line size, in bytes
// NOTE: This constant is made as a macro because some compilers (gcc 4.4 for one) don't allow enums or namespace scope constants in alignment attributes
#if defined(__s390__) || defined(__s390x__)
#define BOOST_ATOMIC_CACHE_LINE_SIZE 256
#elif defined(powerpc) || defined(__powerpc__) || defined(__ppc__)
#define BOOST_ATOMIC_CACHE_LINE_SIZE 128
#else
#define BOOST_ATOMIC_CACHE_LINE_SIZE 64
#endif

namespace boost {
namespace atomics {
namespace detail {

//! \c find_address generic implementation
std::size_t find_address_generic(const volatile void* addr, const volatile void* const* addrs, std::size_t size)
{
    for (std::size_t i = 0u; i < size; ++i)
    {
        if (addrs[i] == addr)
            return i;
    }

    return size;
}

namespace lock_pool {

namespace {

#if BOOST_ARCH_X86 && (defined(BOOST_ATOMIC_USE_SSE2) || defined(BOOST_ATOMIC_USE_SSE41)) && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8 || BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 4)

typedef atomics::detail::core_operations< sizeof(find_address_t*), false, false > func_ptr_operations;
static_assert(func_ptr_operations::is_always_lock_free, "Boost.Atomic unsupported target platform: native atomic operations not implemented for function pointers");

#if defined(BOOST_ATOMIC_DETAIL_X86_USE_RUNTIME_DISPATCH)
std::size_t find_address_dispatch(const volatile void* addr, const volatile void* const* addrs, std::size_t size);
#endif

union find_address_ptr
{
    find_address_t* as_ptr;
    func_ptr_operations::storage_type as_storage;
}
g_find_address =
{
#if defined(BOOST_ATOMIC_USE_SSE41) && BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8 && BOOST_HW_SIMD_X86 >= BOOST_HW_SIMD_X86_SSE4_1_VERSION
    &find_address_sse41
#elif defined(BOOST_ATOMIC_USE_SSE2) && BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 4 && BOOST_HW_SIMD_X86 >= BOOST_HW_SIMD_X86_SSE2_VERSION
    &find_address_sse2
#else
    &find_address_dispatch
#endif
};

#if defined(BOOST_ATOMIC_DETAIL_X86_USE_RUNTIME_DISPATCH)

std::size_t find_address_dispatch(const volatile void* addr, const volatile void* const* addrs, std::size_t size)
{
    find_address_t* find_addr = &find_address_generic;

#if defined(BOOST_ATOMIC_USE_SSE2)
    // First, check the max available cpuid function
    uint32_t eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    atomics::detail::cpuid(eax, ebx, ecx, edx);

    const uint32_t max_cpuid_function = eax;
    if (max_cpuid_function >= 1u)
    {
        // Obtain CPU features
        eax = 1u;
        ebx = ecx = edx = 0u;
        atomics::detail::cpuid(eax, ebx, ecx, edx);

        if ((edx & (1u << 26)) != 0u)
            find_addr = &find_address_sse2;

#if defined(BOOST_ATOMIC_USE_SSE41) && BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8
        if ((ecx & (1u << 19)) != 0u)
            find_addr = &find_address_sse41;
#endif
    }
#endif // defined(BOOST_ATOMIC_USE_SSE2)

    find_address_ptr ptr = {};
    ptr.as_ptr = find_addr;
    func_ptr_operations::store(g_find_address.as_storage, ptr.as_storage, boost::memory_order_relaxed);

    return find_addr(addr, addrs, size);
}

#endif // defined(BOOST_ATOMIC_DETAIL_X86_USE_RUNTIME_DISPATCH)

inline std::size_t find_address(const volatile void* addr, const volatile void* const* addrs, std::size_t size)
{
    find_address_ptr ptr;
    ptr.as_storage = func_ptr_operations::load(g_find_address.as_storage, boost::memory_order_relaxed);
    return ptr.as_ptr(addr, addrs, size);
}

#else // BOOST_ARCH_X86 && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8 || BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 4)

inline std::size_t find_address(const volatile void* addr, const volatile void* const* addrs, std::size_t size)
{
    return atomics::detail::find_address_generic(addr, addrs, size);
}

#endif // BOOST_ARCH_X86 && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8 || BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 4)

struct wait_state;
struct lock_state;

//! Base class for a wait state
struct wait_state_base
{
    //! Number of waiters referencing this state
    std::size_t m_ref_count;
    //! Index of this wait state in the list
    std::size_t m_index;

    explicit wait_state_base(std::size_t index) BOOST_NOEXCEPT :
        m_ref_count(0u),
        m_index(index)
    {
    }

    BOOST_DELETED_FUNCTION(wait_state_base(wait_state_base const&))
    BOOST_DELETED_FUNCTION(wait_state_base& operator= (wait_state_base const&))
};

//! List of wait states. Must be a POD structure.
struct wait_state_list
{
    //! List header
    struct header
    {
        //! List size
        std::size_t size;
        //! List capacity
        std::size_t capacity;
    };

    /*!
     * \brief Pointer to the list header
     *
     * The list buffer consists of three adjacent areas: header object, array of atomic pointers and array of pointers to the wait_state structures.
     * Each of the arrays have header.capacity elements, of which the first header.size elements correspond to the currently ongoing wait operations
     * and the rest are spare elements. Spare wait_state structures may still be allocated (in which case the wait_state pointer is not null) and
     * can be reused on future requests. Spare atomic pointers are null and unused.
     *
     * This memory layout was designed to optimize wait state lookup by atomic address and also support memory pooling to reduce dynamic memory allocations.
     */
    header* m_header;
    //! The flag indicates that memory pooling is disabled. Set on process cleanup.
    bool m_free_memory;

    //! Buffer alignment, in bytes
    static BOOST_CONSTEXPR_OR_CONST std::size_t buffer_alignment = 16u;
    //! Alignment of pointer arrays in the buffer, in bytes. This should align atomic pointers to the vector size used in \c find_address implementation.
    static BOOST_CONSTEXPR_OR_CONST std::size_t entries_alignment = atomics::detail::alignment_of< void* >::value < 16u ? 16u : atomics::detail::alignment_of< void* >::value;
    //! Offset from the list header to the beginning of the array of atomic pointers in the buffer, in bytes
    static BOOST_CONSTEXPR_OR_CONST std::size_t entries_offset = (sizeof(header) + entries_alignment - 1u) & ~static_cast< std::size_t >(entries_alignment - 1u);
    //! Initial buffer capacity, in elements. This should be at least as large as a vector size used in \c find_address implementation.
    static BOOST_CONSTEXPR_OR_CONST std::size_t initial_capacity = (16u / sizeof(void*)) < 2u ? 2u : (16u / sizeof(void*));

    //! Returns a pointer to the array of atomic pointers
    static const volatile void** get_atomic_pointers(header* p) BOOST_NOEXCEPT
    {
        BOOST_ASSERT(p != NULL);
        return reinterpret_cast< const volatile void** >(reinterpret_cast< unsigned char* >(p) + entries_offset);
    }

    //! Returns a pointer to the array of atomic pointers
    const volatile void** get_atomic_pointers() const BOOST_NOEXCEPT
    {
        return get_atomic_pointers(m_header);
    }

    //! Returns a pointer to the array of pointers to the wait states
    static wait_state** get_wait_states(const volatile void** ptrs, std::size_t capacity) BOOST_NOEXCEPT
    {
        return reinterpret_cast< wait_state** >(const_cast< void** >(ptrs + capacity));
    }

    //! Returns a pointer to the array of pointers to the wait states
    static wait_state** get_wait_states(header* p) BOOST_NOEXCEPT
    {
        return get_wait_states(get_atomic_pointers(p), p->capacity);
    }

    //! Returns a pointer to the array of pointers to the wait states
    wait_state** get_wait_states() const BOOST_NOEXCEPT
    {
        return get_wait_states(m_header);
    }

    //! Finds an element with the given pointer to the atomic object
    wait_state* find(const volatile void* addr) const BOOST_NOEXCEPT
    {
        wait_state* ws = NULL;
        if (BOOST_LIKELY(m_header != NULL))
        {
            const volatile void* const* addrs = get_atomic_pointers();
            const std::size_t size = m_header->size;
            std::size_t pos = find_address(addr, addrs, size);
            if (pos < size)
                ws = get_wait_states()[pos];
        }

        return ws;
    }

    //! Finds an existing element with the given pointer to the atomic object or allocates a new one. Returns NULL in case of failure.
    wait_state* find_or_create(const volatile void* addr) BOOST_NOEXCEPT;
    //! Releases the previously created wait state
    void erase(wait_state* w) BOOST_NOEXCEPT;

    //! Deallocates spare entries and the list buffer if no allocated entries are left
    void free_spare() BOOST_NOEXCEPT;
    //! Allocates new buffer for the list entries. Returns NULL in case of failure.
    static header* allocate_buffer(std::size_t new_capacity, header* old_header = NULL) BOOST_NOEXCEPT;
};

#define BOOST_ATOMIC_WAIT_STATE_LIST_INIT { NULL, false }

// In the platform-specific definitions below, lock_state must be a POD structure and wait_state must derive from wait_state_base.

#if defined(BOOST_ATOMIC_USE_PTHREAD)

//! State of a wait operation associated with an atomic object
struct wait_state :
    public wait_state_base
{
    //! Condition variable
    pthread_cond_t m_cond;

    explicit wait_state(std::size_t index) BOOST_NOEXCEPT :
        wait_state_base(index)
    {
        BOOST_VERIFY(pthread_cond_init(&m_cond, NULL) == 0);
    }

    ~wait_state() BOOST_NOEXCEPT
    {
        pthread_cond_destroy(&m_cond);
    }

    //! Blocks in the wait operation until notified
    void wait(lock_state& state) BOOST_NOEXCEPT;

    //! Wakes up one thread blocked in the wait operation
    void notify_one(lock_state&) BOOST_NOEXCEPT
    {
        BOOST_VERIFY(pthread_cond_signal(&m_cond) == 0);
    }
    //! Wakes up all threads blocked in the wait operation
    void notify_all(lock_state&) BOOST_NOEXCEPT
    {
        BOOST_VERIFY(pthread_cond_broadcast(&m_cond) == 0);
    }
};

//! Lock pool entry
struct lock_state
{
    //! Mutex
    pthread_mutex_t m_mutex;
    //! Wait states
    wait_state_list m_wait_states;

    //! Locks the mutex for a short duration
    void short_lock() BOOST_NOEXCEPT
    {
        long_lock();
    }

    //! Locks the mutex for a long duration
    void long_lock() BOOST_NOEXCEPT
    {
        for (unsigned int i = 0u; i < 5u; ++i)
        {
            if (BOOST_LIKELY(pthread_mutex_trylock(&m_mutex) == 0))
                return;

            atomics::detail::pause();
        }

        BOOST_VERIFY(pthread_mutex_lock(&m_mutex) == 0);
    }

    //! Unlocks the mutex
    void unlock() BOOST_NOEXCEPT
    {
        BOOST_VERIFY(pthread_mutex_unlock(&m_mutex) == 0);
    }
};

#define BOOST_ATOMIC_LOCK_STATE_INIT { PTHREAD_MUTEX_INITIALIZER, BOOST_ATOMIC_WAIT_STATE_LIST_INIT }

//! Blocks in the wait operation until notified
inline void wait_state::wait(lock_state& state) BOOST_NOEXCEPT
{
    BOOST_VERIFY(pthread_cond_wait(&m_cond, &state.m_mutex) == 0);
}

#elif defined(BOOST_ATOMIC_USE_FUTEX)

typedef atomics::detail::core_operations< 4u, false, false > futex_operations;
// The storage type must be a 32-bit object, as required by futex API
static_assert(futex_operations::is_always_lock_free && sizeof(futex_operations::storage_type) == 4u, "Boost.Atomic unsupported target platform: native atomic operations not implemented for 32-bit integers");
typedef atomics::detail::extra_operations< futex_operations, futex_operations::storage_size, futex_operations::is_signed > futex_extra_operations;

namespace mutex_bits {

//! The bit indicates a locked mutex
BOOST_CONSTEXPR_OR_CONST futex_operations::storage_type locked = 1u;
//! The bit indicates that there is at least one thread blocked waiting for the mutex to be released
BOOST_CONSTEXPR_OR_CONST futex_operations::storage_type contended = 1u << 1;
//! The lowest bit of the counter bits used to mitigate ABA problem. This and any higher bits in the mutex state constitute the counter.
BOOST_CONSTEXPR_OR_CONST futex_operations::storage_type counter_one = 1u << 2;

} // namespace mutex_bits

//! State of a wait operation associated with an atomic object
struct wait_state :
    public wait_state_base
{
    //! Condition variable futex. Used as the counter of notify calls.
    BOOST_ATOMIC_DETAIL_ALIGNED_VAR(futex_operations::storage_alignment, futex_operations::storage_type, m_cond);
    //! Number of currently blocked waiters
    futex_operations::storage_type m_waiter_count;

    explicit wait_state(std::size_t index) BOOST_NOEXCEPT :
        wait_state_base(index),
        m_cond(0u),
        m_waiter_count(0u)
    {
    }

    //! Blocks in the wait operation until notified
    void wait(lock_state& state) BOOST_NOEXCEPT;

    //! Wakes up one thread blocked in the wait operation
    void notify_one(lock_state& state) BOOST_NOEXCEPT;
    //! Wakes up all threads blocked in the wait operation
    void notify_all(lock_state& state) BOOST_NOEXCEPT;
};

//! Lock pool entry
struct lock_state
{
    //! Mutex futex
    BOOST_ATOMIC_DETAIL_ALIGNED_VAR(futex_operations::storage_alignment, futex_operations::storage_type, m_mutex);
    //! Wait states
    wait_state_list m_wait_states;

    //! Locks the mutex for a short duration
    void short_lock() BOOST_NOEXCEPT
    {
        long_lock();
    }

    //! Locks the mutex for a long duration
    void long_lock() BOOST_NOEXCEPT
    {
        for (unsigned int i = 0u; i < 10u; ++i)
        {
            futex_operations::storage_type prev_state = futex_operations::load(m_mutex, boost::memory_order_relaxed);
            if (BOOST_LIKELY((prev_state & mutex_bits::locked) == 0u))
            {
                futex_operations::storage_type new_state = prev_state | mutex_bits::locked;
                if (BOOST_LIKELY(futex_operations::compare_exchange_strong(m_mutex, prev_state, new_state, boost::memory_order_acquire, boost::memory_order_relaxed)))
                    return;
            }

            atomics::detail::pause();
        }

        lock_slow_path();
    }

    //! Locks the mutex for a long duration
    void lock_slow_path() BOOST_NOEXCEPT
    {
        futex_operations::storage_type prev_state = futex_operations::load(m_mutex, boost::memory_order_relaxed);
        while (true)
        {
            if (BOOST_LIKELY((prev_state & mutex_bits::locked) == 0u))
            {
                futex_operations::storage_type new_state = prev_state | mutex_bits::locked;
                if (BOOST_LIKELY(futex_operations::compare_exchange_weak(m_mutex, prev_state, new_state, boost::memory_order_acquire, boost::memory_order_relaxed)))
                    return;
            }
            else
            {
                futex_operations::storage_type new_state = prev_state | mutex_bits::contended;
                if (BOOST_LIKELY(futex_operations::compare_exchange_weak(m_mutex, prev_state, new_state, boost::memory_order_relaxed, boost::memory_order_relaxed)))
                {
                    atomics::detail::futex_wait_private(&m_mutex, new_state);
                    prev_state = futex_operations::load(m_mutex, boost::memory_order_relaxed);
                }
            }
        }
    }

    //! Unlocks the mutex
    void unlock() BOOST_NOEXCEPT
    {
        futex_operations::storage_type prev_state = futex_operations::load(m_mutex, boost::memory_order_relaxed);
        futex_operations::storage_type new_state;
        while (true)
        {
            new_state = (prev_state & (~mutex_bits::locked)) + mutex_bits::counter_one;
            if (BOOST_LIKELY(futex_operations::compare_exchange_weak(m_mutex, prev_state, new_state, boost::memory_order_release, boost::memory_order_relaxed)))
                break;
        }

        if ((prev_state & mutex_bits::contended) != 0u)
        {
            int woken_count = atomics::detail::futex_signal_private(&m_mutex);
            if (woken_count == 0)
            {
                prev_state = new_state;
                new_state &= ~mutex_bits::contended;
                futex_operations::compare_exchange_strong(m_mutex, prev_state, new_state, boost::memory_order_relaxed, boost::memory_order_relaxed);
            }
        }
    }
};

#if !defined(BOOST_ATOMIC_DETAIL_NO_CXX11_ALIGNAS)
#define BOOST_ATOMIC_LOCK_STATE_INIT { 0u, BOOST_ATOMIC_WAIT_STATE_LIST_INIT }
#else
#define BOOST_ATOMIC_LOCK_STATE_INIT { { 0u }, BOOST_ATOMIC_WAIT_STATE_LIST_INIT }
#endif

//! Blocks in the wait operation until notified
inline void wait_state::wait(lock_state& state) BOOST_NOEXCEPT
{
    const futex_operations::storage_type prev_cond = m_cond;
    ++m_waiter_count;

    state.unlock();

    while (true)
    {
        int err = atomics::detail::futex_wait_private(&m_cond, prev_cond);
        if (BOOST_LIKELY(err != EINTR))
            break;
    }

    state.long_lock();

    --m_waiter_count;
}

//! Wakes up one thread blocked in the wait operation
inline void wait_state::notify_one(lock_state& state) BOOST_NOEXCEPT
{
    ++m_cond;

    if (BOOST_LIKELY(m_waiter_count > 0u))
    {
        // Move one blocked thread to the mutex futex and mark the mutex contended so that the thread is unblocked on unlock()
        atomics::detail::futex_requeue_private(&m_cond, &state.m_mutex, 0u, 1u);
        futex_extra_operations::opaque_or(state.m_mutex, mutex_bits::contended, boost::memory_order_relaxed);
    }
}

//! Wakes up all threads blocked in the wait operation
inline void wait_state::notify_all(lock_state& state) BOOST_NOEXCEPT
{
    ++m_cond;

    if (BOOST_LIKELY(m_waiter_count > 0u))
    {
        // Move blocked threads to the mutex futex and mark the mutex contended so that a thread is unblocked on unlock()
        atomics::detail::futex_requeue_private(&m_cond, &state.m_mutex, 0u);
        futex_extra_operations::opaque_or(state.m_mutex, mutex_bits::contended, boost::memory_order_relaxed);
    }
}

#else

#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

//! State of a wait operation associated with an atomic object
struct wait_state :
    public wait_state_base
{
    //! Condition variable
    boost::winapi::CONDITION_VARIABLE_ m_cond;

    explicit wait_state(std::size_t index) BOOST_NOEXCEPT :
        wait_state_base(index)
    {
        boost::winapi::InitializeConditionVariable(&m_cond);
    }

    //! Blocks in the wait operation until notified
    void wait(lock_state& state) BOOST_NOEXCEPT;

    //! Wakes up one thread blocked in the wait operation
    void notify_one(lock_state&) BOOST_NOEXCEPT
    {
        boost::winapi::WakeConditionVariable(&m_cond);
    }
    //! Wakes up all threads blocked in the wait operation
    void notify_all(lock_state&) BOOST_NOEXCEPT
    {
        boost::winapi::WakeAllConditionVariable(&m_cond);
    }
};

//! Lock pool entry
struct lock_state
{
    //! Mutex
    boost::winapi::SRWLOCK_ m_mutex;
    //! Wait states
    wait_state_list m_wait_states;

    //! Locks the mutex for a short duration
    void short_lock() BOOST_NOEXCEPT
    {
        long_lock();
    }

    //! Locks the mutex for a long duration
    void long_lock() BOOST_NOEXCEPT
    {
        // Presumably, AcquireSRWLockExclusive already implements spinning internally, so there's no point in doing this ourselves.
        boost::winapi::AcquireSRWLockExclusive(&m_mutex);
    }

    //! Unlocks the mutex
    void unlock() BOOST_NOEXCEPT
    {
        boost::winapi::ReleaseSRWLockExclusive(&m_mutex);
    }
};

#define BOOST_ATOMIC_LOCK_STATE_INIT { BOOST_WINAPI_SRWLOCK_INIT, BOOST_ATOMIC_WAIT_STATE_LIST_INIT }

//! Blocks in the wait operation until notified
inline void wait_state::wait(lock_state& state) BOOST_NOEXCEPT
{
    boost::winapi::SleepConditionVariableSRW(&m_cond, &state.m_mutex, boost::winapi::infinite, 0u);
}

#else // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

typedef atomics::detail::core_operations< 4u, false, false > mutex_operations;
static_assert(mutex_operations::is_always_lock_free, "Boost.Atomic unsupported target platform: native atomic operations not implemented for 32-bit integers");

namespace fallback_mutex_bits {

//! The bit indicates a locked mutex
BOOST_CONSTEXPR_OR_CONST mutex_operations::storage_type locked = 1u;
//! The bit indicates that the critical section is initialized and should be used instead of the fallback mutex
BOOST_CONSTEXPR_OR_CONST mutex_operations::storage_type critical_section_initialized = 1u << 1;

} // namespace mutex_bits

//! State of a wait operation associated with an atomic object
struct wait_state :
    public wait_state_base
{
    /*!
     * \brief A semaphore used to block one or more threads
     *
     * A semaphore can be used to block a thread if it has no ongoing notifications (i.e. \c m_notify_count is 0).
     * If there is no such semaphore, the thread has to allocate a new one to block on. This is to guarantee
     * that a thread that is blocked after a notification is not immediately released by the semaphore while
     * there are previously blocked threads.
     *
     * Semaphores are organized in a circular doubly linked list. A single semaphore object represents a list
     * of one semaphore and is said to be "singular".
     */
    struct semaphore
    {
        //! Pointer to the next semaphore in the list
        semaphore* m_next;
        //! Pointer to the previous semaphore in the list
        semaphore* m_prev;

        //! Semaphore handle
        boost::winapi::HANDLE_ m_semaphore;
        //! Number of threads blocked on the semaphore
        boost::winapi::ULONG_ m_waiter_count;
        //! Number of threads released by notifications
        boost::winapi::ULONG_ m_notify_count;

        semaphore() BOOST_NOEXCEPT :
            m_semaphore(boost::winapi::create_anonymous_semaphore(NULL, 0, (std::numeric_limits< boost::winapi::LONG_ >::max)())),
            m_waiter_count(0u),
            m_notify_count(0u)
        {
            m_next = m_prev = this;
        }

        ~semaphore() BOOST_NOEXCEPT
        {
            BOOST_ASSERT(is_singular());

            if (BOOST_LIKELY(m_semaphore != boost::winapi::invalid_handle_value))
                boost::winapi::CloseHandle(m_semaphore);
        }

        //! Creates a new semaphore or returns null in case of failure
        static semaphore* create() BOOST_NOEXCEPT
        {
            semaphore* p = new (std::nothrow) semaphore();
            if (BOOST_UNLIKELY(p != NULL && p->m_semaphore == boost::winapi::invalid_handle_value))
            {
                delete p;
                p = NULL;
            }
            return p;
        }

        //! Returns \c true if the semaphore is the single element of the list
        bool is_singular() const BOOST_NOEXCEPT
        {
            return m_next == this /* && m_prev == this */;
        }

        //! Inserts the semaphore list after the specified other semaphore
        void link_after(semaphore* that) BOOST_NOEXCEPT
        {
            link_before(that->m_next);
        }

        //! Inserts the semaphore list before the specified other semaphore
        void link_before(semaphore* that) BOOST_NOEXCEPT
        {
            semaphore* prev = that->m_prev;
            that->m_prev = m_prev;
            m_prev->m_next = that;
            m_prev = prev;
            prev->m_next = this;
        }

        //! Removes the semaphore from the list
        void unlink() BOOST_NOEXCEPT
        {
            // Load pointers beforehand, in case we are the only element in the list
            semaphore* next = m_next;
            semaphore* prev = m_prev;
            prev->m_next = next;
            next->m_prev = prev;
            m_next = m_prev = this;
        }

        BOOST_DELETED_FUNCTION(semaphore(semaphore const&))
        BOOST_DELETED_FUNCTION(semaphore& operator= (semaphore const&))
    };

    //! Doubly linked circular list of semaphores
    class semaphore_list
    {
    private:
        semaphore* m_head;

    public:
        semaphore_list() BOOST_NOEXCEPT :
            m_head(NULL)
        {
        }

        //! Returns \c true if the list is empty
        bool empty() const BOOST_NOEXCEPT
        {
            return m_head == NULL;
        }

        //! Returns the first semaphore in the list
        semaphore* front() const BOOST_NOEXCEPT
        {
            return m_head;
        }

        //! Returns the first semaphore in the list and leaves the list empty
        semaphore* eject() BOOST_NOEXCEPT
        {
            semaphore* sem = m_head;
            m_head = NULL;
            return sem;
        }

        //! Inserts the semaphore at the beginning of the list
        void push_front(semaphore* sem) BOOST_NOEXCEPT
        {
            if (m_head)
                sem->link_before(m_head);

            m_head = sem;
        }

        //! Removes the first semaphore from the beginning of the list
        semaphore* pop_front() BOOST_NOEXCEPT
        {
            BOOST_ASSERT(!empty());
            semaphore* sem = m_head;
            erase(sem);
            return sem;
        }

        //! Removes the semaphore from the list
        void erase(semaphore* sem) BOOST_NOEXCEPT
        {
            if (sem->is_singular())
            {
                BOOST_ASSERT(m_head == sem);
                m_head = NULL;
            }
            else
            {
                if (m_head == sem)
                    m_head = sem->m_next;
                sem->unlink();
            }
        }

        BOOST_DELETED_FUNCTION(semaphore_list(semaphore_list const&))
        BOOST_DELETED_FUNCTION(semaphore_list& operator= (semaphore_list const&))
    };

    //! List of semaphores used for notifying. Here, every semaphore has m_notify_count > 0 && m_waiter_count > 0.
    semaphore_list m_notify_semaphores;
    //! List of semaphores used for waiting. Here, every semaphore has m_notify_count == 0 && m_waiter_count > 0.
    semaphore_list m_wait_semaphores;
    //! List of free semaphores. Here, every semaphore has m_notify_count == 0 && m_waiter_count == 0.
    semaphore_list m_free_semaphores;

    explicit wait_state(std::size_t index) BOOST_NOEXCEPT :
        wait_state_base(index)
    {
    }

    ~wait_state() BOOST_NOEXCEPT
    {
        // All wait and notification operations must have been completed
        BOOST_ASSERT(m_notify_semaphores.empty());
        BOOST_ASSERT(m_wait_semaphores.empty());

        semaphore* sem = m_free_semaphores.eject();
        if (sem)
        {
            while (true)
            {
                bool was_last = sem->is_singular();
                semaphore* next = sem->m_next;
                sem->unlink();

                delete sem;

                if (was_last)
                    break;

                sem = next;
            }
        }
    }

    //! Blocks in the wait operation until notified
    void wait(lock_state& state) BOOST_NOEXCEPT;
    //! Fallback implementation of wait
    void wait_fallback(lock_state& state) BOOST_NOEXCEPT;

    //! Wakes up one thread blocked in the wait operation
    void notify_one(lock_state&) BOOST_NOEXCEPT
    {
        if (m_notify_semaphores.empty())
        {
            if (m_wait_semaphores.empty())
                return;

            // Move the semaphore with waiters to the notify list
            m_notify_semaphores.push_front(m_wait_semaphores.pop_front());
        }

        semaphore* sem = m_notify_semaphores.front();
        ++sem->m_notify_count;

        if (sem->m_notify_count == sem->m_waiter_count)
        {
            // Remove this semaphore from the list. The waiter will re-insert it into the waiter or free list once there are no more pending notifications in it.
            m_notify_semaphores.erase(sem);
        }

        boost::winapi::ReleaseSemaphore(sem->m_semaphore, 1, NULL);
    }

    //! Wakes up all threads blocked in the wait operation
    void notify_all(lock_state&) BOOST_NOEXCEPT
    {
        // Combine all notify and waiter semaphores in one list
        semaphore* sem = m_notify_semaphores.eject();
        if (sem)
        {
            if (!m_wait_semaphores.empty())
            {
                m_wait_semaphores.eject()->link_before(sem);
            }
        }
        else
        {
            sem = m_wait_semaphores.eject();
        }

        if (sem)
        {
            while (true)
            {
                bool was_last = sem->is_singular();
                semaphore* next = sem->m_next;
                sem->unlink();

                boost::winapi::ULONG_ count = sem->m_waiter_count - sem->m_notify_count;
                sem->m_notify_count += count;

                boost::winapi::ReleaseSemaphore(sem->m_semaphore, count, NULL);

                if (was_last)
                    break;

                sem = next;
            }
        }
    }
};

//! Lock pool entry
struct lock_state
{
    //! Mutex
    boost::winapi::CRITICAL_SECTION_ m_mutex;
    //! Fallback mutex. Used as indicator of critical section initialization state and a fallback mutex, if critical section cannot be initialized.
    BOOST_ATOMIC_DETAIL_ALIGNED_VAR(mutex_operations::storage_alignment, mutex_operations::storage_type, m_mutex_fallback);
    //! Wait states
    wait_state_list m_wait_states;

    //! Locks the mutex for a short duration
    void short_lock() BOOST_NOEXCEPT
    {
        long_lock();
    }

    //! Locks the mutex for a long duration
    void long_lock() BOOST_NOEXCEPT
    {
        mutex_operations::storage_type fallback_state = mutex_operations::load(m_mutex_fallback, boost::memory_order_relaxed);
        while (true)
        {
            if (BOOST_LIKELY(fallback_state == fallback_mutex_bits::critical_section_initialized))
            {
            lock_cs:
                boost::winapi::EnterCriticalSection(&m_mutex);
                return;
            }

            while (fallback_state == 0u)
            {
                if (!mutex_operations::compare_exchange_weak(m_mutex_fallback, fallback_state, fallback_mutex_bits::locked, boost::memory_order_acquire, boost::memory_order_relaxed))
                    continue;

                if (BOOST_LIKELY(!!boost::winapi::InitializeCriticalSectionAndSpinCount(&m_mutex, 100u)))
                {
                    mutex_operations::store(m_mutex_fallback, fallback_mutex_bits::critical_section_initialized, boost::memory_order_release);
                    goto lock_cs;
                }

                // We failed to init the critical section, leave the fallback mutex locked and return
                return;
            }

            if (fallback_state == fallback_mutex_bits::locked)
            {
                // Wait intil the fallback mutex is unlocked
                boost::winapi::SwitchToThread();
                fallback_state = mutex_operations::load(m_mutex_fallback, boost::memory_order_relaxed);
            }
        }
    }

    //! Unlocks the mutex
    void unlock() BOOST_NOEXCEPT
    {
        mutex_operations::storage_type fallback_state = mutex_operations::load(m_mutex_fallback, boost::memory_order_relaxed);
        if (BOOST_LIKELY(fallback_state == fallback_mutex_bits::critical_section_initialized))
        {
            boost::winapi::LeaveCriticalSection(&m_mutex);
            return;
        }

        mutex_operations::store(m_mutex_fallback, 0u, boost::memory_order_release);
    }
};

#if !defined(BOOST_ATOMIC_DETAIL_NO_CXX11_ALIGNAS)
#define BOOST_ATOMIC_LOCK_STATE_INIT { {}, 0u, BOOST_ATOMIC_WAIT_STATE_LIST_INIT }
#else
#define BOOST_ATOMIC_LOCK_STATE_INIT { {}, { 0u }, BOOST_ATOMIC_WAIT_STATE_LIST_INIT }
#endif

//! Blocks in the wait operation until notified
inline void wait_state::wait(lock_state& state) BOOST_NOEXCEPT
{
    // Find a semaphore to block on
    semaphore* sem = m_wait_semaphores.front();
    if (sem)
    {
        while (sem->m_waiter_count >= static_cast< boost::winapi::ULONG_ >((std::numeric_limits< boost::winapi::LONG_ >::max)()))
        {
            if (sem->m_next == m_wait_semaphores.front())
            {
                sem = NULL;
                break;
            }

            sem = sem->m_next;
        }
    }

    if (!sem)
    {
        if (BOOST_LIKELY(!m_free_semaphores.empty()))
        {
            sem = m_free_semaphores.pop_front();
        }
        else
        {
            sem = semaphore::create();
            if (BOOST_UNLIKELY(!sem))
            {
                wait_fallback(state);
                return;
            }
        }

        m_wait_semaphores.push_front(sem);
    }

    ++sem->m_waiter_count;

    state.unlock();

    boost::winapi::WaitForSingleObject(sem->m_semaphore, boost::winapi::infinite);

    state.long_lock();

    --sem->m_waiter_count;

    if (sem->m_notify_count > 0u)
    {
        // This semaphore is either in the notify list or not in a list at all
        if (--sem->m_notify_count == 0u)
        {
            if (!sem->is_singular() || sem == m_notify_semaphores.front())
                m_notify_semaphores.erase(sem);

            semaphore_list* list = sem->m_waiter_count == 0u ? &m_free_semaphores : &m_wait_semaphores;
            list->push_front(sem);
        }
    }
    else if (sem->m_waiter_count == 0u)
    {
        // Move the semaphore to the free list
        m_wait_semaphores.erase(sem);
        m_free_semaphores.push_front(sem);
    }
}

//! Fallback implementation of wait
inline void wait_state::wait_fallback(lock_state& state) BOOST_NOEXCEPT
{
    state.unlock();

    boost::winapi::Sleep(0);

    state.long_lock();
}

#endif // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

#endif

enum
{
    tail_size = sizeof(lock_state) % BOOST_ATOMIC_CACHE_LINE_SIZE,
    padding_size = tail_size > 0 ? BOOST_ATOMIC_CACHE_LINE_SIZE - tail_size : 0u
};

template< unsigned int PaddingSize >
struct BOOST_ALIGNMENT(BOOST_ATOMIC_CACHE_LINE_SIZE) padded_lock_state
{
    lock_state state;
    // The additional padding is needed to avoid false sharing between locks
    char padding[PaddingSize];
};

template< >
struct BOOST_ALIGNMENT(BOOST_ATOMIC_CACHE_LINE_SIZE) padded_lock_state< 0u >
{
    lock_state state;
};

typedef padded_lock_state< padding_size > padded_lock_state_t;

#if !defined(BOOST_ATOMIC_LOCK_POOL_SIZE_LOG2)
#define BOOST_ATOMIC_LOCK_POOL_SIZE_LOG2 8
#endif
#if (BOOST_ATOMIC_LOCK_POOL_SIZE_LOG2) < 0
#error "Boost.Atomic: BOOST_ATOMIC_LOCK_POOL_SIZE_LOG2 macro value is negative"
#endif
#define BOOST_ATOMIC_DETAIL_LOCK_POOL_SIZE (1ull << (BOOST_ATOMIC_LOCK_POOL_SIZE_LOG2))

//! Lock pool size. Must be a power of two.
BOOST_CONSTEXPR_OR_CONST std::size_t lock_pool_size = static_cast< std::size_t >(1u) << (BOOST_ATOMIC_LOCK_POOL_SIZE_LOG2);

static padded_lock_state_t g_lock_pool[lock_pool_size] =
{
#if BOOST_ATOMIC_DETAIL_LOCK_POOL_SIZE > 256u
#if (BOOST_ATOMIC_DETAIL_LOCK_POOL_SIZE / 256u) > BOOST_PP_LIMIT_ITERATION
#error "Boost.Atomic: BOOST_ATOMIC_LOCK_POOL_SIZE_LOG2 macro value is too large"
#endif
#define BOOST_PP_ITERATION_PARAMS_1 (3, (1, (BOOST_ATOMIC_DETAIL_LOCK_POOL_SIZE / 256u), "lock_pool_init256.ipp"))
#else // BOOST_ATOMIC_DETAIL_LOCK_POOL_SIZE > 256u
#define BOOST_PP_ITERATION_PARAMS_1 (3, (1, BOOST_ATOMIC_DETAIL_LOCK_POOL_SIZE, "lock_pool_init1.ipp"))
#endif // BOOST_ATOMIC_DETAIL_LOCK_POOL_SIZE > 256u
#include BOOST_PP_ITERATE()
#undef BOOST_PP_ITERATION_PARAMS_1
};

//! Pool cleanup function
void cleanup_lock_pool()
{
    for (std::size_t i = 0u; i < lock_pool_size; ++i)
    {
        lock_state& state = g_lock_pool[i].state;
        state.long_lock();
        state.m_wait_states.m_free_memory = true;
        state.m_wait_states.free_spare();
        state.unlock();
    }
}

static_assert(once_flag_operations::is_always_lock_free, "Boost.Atomic unsupported target platform: native atomic operations not implemented for bytes");
static once_flag g_pool_cleanup_registered = {};

//! Returns index of the lock pool entry for the given pointer value
BOOST_FORCEINLINE std::size_t get_lock_index(atomics::detail::uintptr_t h) BOOST_NOEXCEPT
{
    return h & (lock_pool_size - 1u);
}

//! Finds an existing element with the given pointer to the atomic object or allocates a new one
inline wait_state* wait_state_list::find_or_create(const volatile void* addr) BOOST_NOEXCEPT
{
    if (BOOST_UNLIKELY(m_header == NULL))
    {
        m_header = allocate_buffer(initial_capacity);
        if (BOOST_UNLIKELY(m_header == NULL))
            return NULL;
    }
    else
    {
        wait_state* ws = this->find(addr);
        if (BOOST_LIKELY(ws != NULL))
            return ws;

        if (BOOST_UNLIKELY(m_header->size == m_header->capacity))
        {
            header* new_header = allocate_buffer(m_header->capacity * 2u, m_header);
            if (BOOST_UNLIKELY(new_header == NULL))
                return NULL;
            boost::alignment::aligned_free(static_cast< void* >(m_header));
            m_header = new_header;
        }
    }

    const std::size_t index = m_header->size;
    BOOST_ASSERT(index < m_header->capacity);

    wait_state** pw = get_wait_states() + index;
    wait_state* w = *pw;
    if (BOOST_UNLIKELY(w == NULL))
    {
        w = new (std::nothrow) wait_state(index);
        if (BOOST_UNLIKELY(w == NULL))
            return NULL;
        *pw = w;
    }

    get_atomic_pointers()[index] = addr;

    ++m_header->size;

    return w;
}

//! Releases the previously created wait state
inline void wait_state_list::erase(wait_state* w) BOOST_NOEXCEPT
{
    BOOST_ASSERT(m_header != NULL);

    const volatile void** pa = get_atomic_pointers();
    wait_state** pw = get_wait_states();

    std::size_t index = w->m_index;

    BOOST_ASSERT(index < m_header->size);
    BOOST_ASSERT(pw[index] == w);

    std::size_t last_index = m_header->size - 1u;

    if (index != last_index)
    {
        pa[index] = pa[last_index];
        pa[last_index] = NULL;

        wait_state* last_w = pw[last_index];
        pw[index] = last_w;
        pw[last_index] = w;

        last_w->m_index = index;
        w->m_index = last_index;
    }
    else
    {
        pa[index] = NULL;
    }

    --m_header->size;

    if (BOOST_UNLIKELY(m_free_memory))
        free_spare();
}

//! Allocates new buffer for the list entries
wait_state_list::header* wait_state_list::allocate_buffer(std::size_t new_capacity, header* old_header) BOOST_NOEXCEPT
{
    if (BOOST_UNLIKELY(once_flag_operations::load(g_pool_cleanup_registered.m_flag, boost::memory_order_relaxed) == 0u))
    {
        if (once_flag_operations::exchange(g_pool_cleanup_registered.m_flag, 1u, boost::memory_order_relaxed) == 0u)
            std::atexit(&cleanup_lock_pool);
    }

    const std::size_t new_buffer_size = entries_offset + new_capacity * sizeof(void*) * 2u;

    void* p = boost::alignment::aligned_alloc(buffer_alignment, new_buffer_size);
    if (BOOST_UNLIKELY(p == NULL))
        return NULL;

    header* h = new (p) header;
    const volatile void** a = new (get_atomic_pointers(h)) const volatile void*[new_capacity];
    wait_state** w = new (get_wait_states(a, new_capacity)) wait_state*[new_capacity];

    if (BOOST_LIKELY(old_header != NULL))
    {
        BOOST_ASSERT(new_capacity >= old_header->capacity);

        h->size = old_header->size;

        const volatile void** old_a = get_atomic_pointers(old_header);
        std::memcpy(a, old_a, old_header->size * sizeof(const volatile void*));
        std::memset(a + old_header->size, 0, (new_capacity - old_header->size) * sizeof(const volatile void*));

        wait_state** old_w = get_wait_states(old_a, old_header->capacity);
        std::memcpy(w, old_w, old_header->capacity * sizeof(wait_state*)); // copy spare wait state pointers
        std::memset(w + old_header->capacity, 0, (new_capacity - old_header->capacity) * sizeof(wait_state*));
    }
    else
    {
        std::memset(p, 0, new_buffer_size);
    }

    h->capacity = new_capacity;

    return h;
}

//! Deallocates spare entries and the list buffer if no allocated entries are left
void wait_state_list::free_spare() BOOST_NOEXCEPT
{
    if (BOOST_LIKELY(m_header != NULL))
    {
        wait_state** ws = get_wait_states();
        for (std::size_t i = m_header->size, n = m_header->capacity; i < n; ++i)
        {
            wait_state* w = ws[i];
            if (!w)
                break;

            delete w;
            ws[i] = NULL;
        }

        if (m_header->size == 0u)
        {
            boost::alignment::aligned_free(static_cast< void* >(m_header));
            m_header = NULL;
        }
    }
}

} // namespace


BOOST_ATOMIC_DECL void* short_lock(atomics::detail::uintptr_t h) BOOST_NOEXCEPT
{
    lock_state& ls = g_lock_pool[get_lock_index(h)].state;
    ls.short_lock();
    return &ls;
}

BOOST_ATOMIC_DECL void* long_lock(atomics::detail::uintptr_t h) BOOST_NOEXCEPT
{
    lock_state& ls = g_lock_pool[get_lock_index(h)].state;
    ls.long_lock();
    return &ls;
}

BOOST_ATOMIC_DECL void unlock(void* vls) BOOST_NOEXCEPT
{
    static_cast< lock_state* >(vls)->unlock();
}


BOOST_ATOMIC_DECL void* allocate_wait_state(void* vls, const volatile void* addr) BOOST_NOEXCEPT
{
    BOOST_ASSERT(vls != NULL);

    lock_state* ls = static_cast< lock_state* >(vls);

    // Note: find_or_create may fail to allocate memory. However, C++20 specifies that wait/notify operations
    // are noexcept, so allocate_wait_state must succeed. To implement this we return NULL in case of failure and test for NULL
    // in other wait/notify functions so that all of them become nop (which is a conforming, though inefficient behavior).
    wait_state* ws = ls->m_wait_states.find_or_create(addr);

    if (BOOST_LIKELY(ws != NULL))
        ++ws->m_ref_count;

    return ws;
}

BOOST_ATOMIC_DECL void free_wait_state(void* vls, void* vws) BOOST_NOEXCEPT
{
    BOOST_ASSERT(vls != NULL);

    wait_state* ws = static_cast< wait_state* >(vws);
    if (BOOST_LIKELY(ws != NULL))
    {
        if (--ws->m_ref_count == 0u)
        {
            lock_state* ls = static_cast< lock_state* >(vls);
            ls->m_wait_states.erase(ws);
        }
    }
}

BOOST_ATOMIC_DECL void wait(void* vls, void* vws) BOOST_NOEXCEPT
{
    BOOST_ASSERT(vls != NULL);

    lock_state* ls = static_cast< lock_state* >(vls);
    wait_state* ws = static_cast< wait_state* >(vws);
    if (BOOST_LIKELY(ws != NULL))
    {
        ws->wait(*ls);
    }
    else
    {
        // A conforming wait operation must unlock and lock the mutex to allow a notify to complete
        ls->unlock();
        atomics::detail::wait_some();
        ls->long_lock();
    }
}

BOOST_ATOMIC_DECL void notify_one(void* vls, const volatile void* addr) BOOST_NOEXCEPT
{
    BOOST_ASSERT(vls != NULL);

    lock_state* ls = static_cast< lock_state* >(vls);
    wait_state* ws = ls->m_wait_states.find(addr);
    if (BOOST_LIKELY(ws != NULL))
        ws->notify_one(*ls);
}

BOOST_ATOMIC_DECL void notify_all(void* vls, const volatile void* addr) BOOST_NOEXCEPT
{
    BOOST_ASSERT(vls != NULL);

    lock_state* ls = static_cast< lock_state* >(vls);
    wait_state* ws = ls->m_wait_states.find(addr);
    if (BOOST_LIKELY(ws != NULL))
        ws->notify_all(*ls);
}


BOOST_ATOMIC_DECL void thread_fence() BOOST_NOEXCEPT
{
#if BOOST_ATOMIC_THREAD_FENCE == 2
    atomics::detail::fence_operations::thread_fence(memory_order_seq_cst);
#else
    // Emulate full fence by locking/unlocking a mutex
    lock_pool::unlock(lock_pool::short_lock(0u));
#endif
}

BOOST_ATOMIC_DECL void signal_fence() BOOST_NOEXCEPT
{
    // This function is intentionally non-inline, even if empty. This forces the compiler to treat its call as a compiler barrier.
#if BOOST_ATOMIC_SIGNAL_FENCE == 2
    atomics::detail::fence_operations::signal_fence(memory_order_seq_cst);
#endif
}

} // namespace lock_pool
} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>
