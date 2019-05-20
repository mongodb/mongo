/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2011 Helge Bahmann
 * Copyright (c) 2013-2014 Andrey Semashev
 */
/*!
 * \file   lockpool.cpp
 *
 * This file contains implementation of the lockpool used to emulate atomic ops.
 */

#include <cstddef>
#include <boost/config.hpp>
#include <boost/assert.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/capabilities.hpp>

#if BOOST_ATOMIC_FLAG_LOCK_FREE == 2
#include <boost/atomic/detail/operations_lockfree.hpp>
#elif !defined(BOOST_HAS_PTHREADS)
#error Boost.Atomic: Unsupported target platform, POSIX threads are required when native atomic operations are not available
#else
#include <pthread.h>
#define BOOST_ATOMIC_USE_PTHREAD
#endif

#include <boost/atomic/detail/lockpool.hpp>
#include <boost/atomic/detail/pause.hpp>

#if defined(BOOST_MSVC)
#pragma warning(push)
// 'struct_name' : structure was padded due to __declspec(align())
#pragma warning(disable: 4324)
#endif

namespace boost {
namespace atomics {
namespace detail {

namespace {

// Cache line size, in bytes
// NOTE: This constant is made as a macro because some compilers (gcc 4.4 for one) don't allow enums or namespace scope constants in alignment attributes
#if defined(__s390__) || defined(__s390x__)
#define BOOST_ATOMIC_CACHE_LINE_SIZE 256
#elif defined(powerpc) || defined(__powerpc__) || defined(__ppc__)
#define BOOST_ATOMIC_CACHE_LINE_SIZE 128
#else
#define BOOST_ATOMIC_CACHE_LINE_SIZE 64
#endif

#if defined(BOOST_ATOMIC_USE_PTHREAD)
typedef pthread_mutex_t lock_type;
#else
typedef atomics::detail::operations< 1u, false > lock_operations;
typedef lock_operations::storage_type lock_type;
#endif

enum
{
    padding_size = (sizeof(lock_type) <= BOOST_ATOMIC_CACHE_LINE_SIZE ?
        (BOOST_ATOMIC_CACHE_LINE_SIZE - sizeof(lock_type)) :
        (BOOST_ATOMIC_CACHE_LINE_SIZE - sizeof(lock_type) % BOOST_ATOMIC_CACHE_LINE_SIZE))
};

template< unsigned int PaddingSize >
struct BOOST_ALIGNMENT(BOOST_ATOMIC_CACHE_LINE_SIZE) padded_lock
{
    lock_type lock;
    // The additional padding is needed to avoid false sharing between locks
    char padding[PaddingSize];
};

template< >
struct BOOST_ALIGNMENT(BOOST_ATOMIC_CACHE_LINE_SIZE) padded_lock< 0u >
{
    lock_type lock;
};

typedef padded_lock< padding_size > padded_lock_t;

static padded_lock_t g_lock_pool[41]
#if defined(BOOST_ATOMIC_USE_PTHREAD)
=
{
    { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER },
    { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER },
    { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER },
    { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER },
    { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER },
    { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER },
    { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER },
    { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER }, { PTHREAD_MUTEX_INITIALIZER },
    { PTHREAD_MUTEX_INITIALIZER }
}
#endif
;

} // namespace


#if !defined(BOOST_ATOMIC_USE_PTHREAD)

// NOTE: This function must NOT be inline. Otherwise MSVC 9 will sometimes generate broken code for modulus operation which result in crashes.
BOOST_ATOMIC_DECL lockpool::scoped_lock::scoped_lock(const volatile void* addr) BOOST_NOEXCEPT :
    m_lock(&g_lock_pool[reinterpret_cast< std::size_t >(addr) % (sizeof(g_lock_pool) / sizeof(*g_lock_pool))].lock)
{
    while (lock_operations::test_and_set(*static_cast< lock_type* >(m_lock), memory_order_acquire))
    {
        do
        {
            atomics::detail::pause();
        }
        while (!!lock_operations::load(*static_cast< lock_type* >(m_lock), memory_order_relaxed));
    }
}

BOOST_ATOMIC_DECL lockpool::scoped_lock::~scoped_lock() BOOST_NOEXCEPT
{
    lock_operations::clear(*static_cast< lock_type* >(m_lock), memory_order_release);
}

BOOST_ATOMIC_DECL void signal_fence() BOOST_NOEXCEPT;

#else // !defined(BOOST_ATOMIC_USE_PTHREAD)

BOOST_ATOMIC_DECL lockpool::scoped_lock::scoped_lock(const volatile void* addr) BOOST_NOEXCEPT :
    m_lock(&g_lock_pool[reinterpret_cast< std::size_t >(addr) % (sizeof(g_lock_pool) / sizeof(*g_lock_pool))].lock)
{
    BOOST_VERIFY(pthread_mutex_lock(static_cast< pthread_mutex_t* >(m_lock)) == 0);
}

BOOST_ATOMIC_DECL lockpool::scoped_lock::~scoped_lock() BOOST_NOEXCEPT
{
    BOOST_VERIFY(pthread_mutex_unlock(static_cast< pthread_mutex_t* >(m_lock)) == 0);
}

#endif // !defined(BOOST_ATOMIC_USE_PTHREAD)

BOOST_ATOMIC_DECL void lockpool::thread_fence() BOOST_NOEXCEPT
{
#if BOOST_ATOMIC_THREAD_FENCE > 0
    atomics::detail::thread_fence(memory_order_seq_cst);
#else
    // Emulate full fence by locking/unlocking a mutex
    scoped_lock lock(0);
#endif
}

BOOST_ATOMIC_DECL void lockpool::signal_fence() BOOST_NOEXCEPT
{
    // This function is intentionally non-inline, even if empty. This forces the compiler to treat its call as a compiler barrier.
#if BOOST_ATOMIC_SIGNAL_FENCE > 0
    atomics::detail::signal_fence(memory_order_seq_cst);
#endif
}

} // namespace detail
} // namespace atomics
} // namespace boost

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif
