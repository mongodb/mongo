/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   thread_specific.cpp
 * \author Andrey Semashev
 * \date   01.03.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <string>
#include <stdexcept>
#include <boost/log/exceptions.hpp>
#include <boost/log/detail/thread_specific.hpp>

#if !defined(BOOST_LOG_NO_THREADS)

#if defined(BOOST_THREAD_PLATFORM_WIN32)

#include <windows.h>
#include <boost/system/error_code.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

thread_specific_base::thread_specific_base()
{
    m_Key = TlsAlloc();
    if (BOOST_UNLIKELY(m_Key == TLS_OUT_OF_INDEXES))
    {
        BOOST_LOG_THROW_DESCR_PARAMS(system_error, "TLS capacity depleted", (boost::system::errc::not_enough_memory));
    }
}

thread_specific_base::~thread_specific_base()
{
    TlsFree(m_Key);
}

void* thread_specific_base::get_content() const
{
    return TlsGetValue(m_Key);
}

void thread_specific_base::set_content(void* value) const
{
    TlsSetValue(m_Key, value);
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#elif defined(BOOST_THREAD_PLATFORM_PTHREAD)

#include <cstddef>
#include <cstring>
#include <pthread.h>
#include <boost/cstdint.hpp>
#include <boost/type_traits/conditional.hpp>
#include <boost/type_traits/is_integral.hpp>
#include <boost/type_traits/is_signed.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! Some portability magic to detect how to store the TLS key
template< typename KeyT, bool IsStoreableV = sizeof(KeyT) <= sizeof(void*), bool IsIntegralV = boost::is_integral< KeyT >::value >
struct pthread_key_traits
{
    typedef KeyT pthread_key_type;

    static void allocate(void*& stg)
    {
        pthread_key_type* pkey = new pthread_key_type();
        const int res = pthread_key_create(pkey, NULL);
        if (BOOST_UNLIKELY(res != 0))
        {
            delete pkey;
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "TLS capacity depleted", (res));
        }
        stg = pkey;
    }

    static void deallocate(void* stg)
    {
        pthread_key_type* pkey = static_cast< pthread_key_type* >(stg);
        pthread_key_delete(*pkey);
        delete pkey;
    }

    static void set_value(void* stg, void* value)
    {
        const int res = pthread_setspecific(*static_cast< pthread_key_type* >(stg), value);
        if (BOOST_UNLIKELY(res != 0))
        {
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to set TLS value", (res));
        }
    }

    static void* get_value(void* stg)
    {
        return pthread_getspecific(*static_cast< pthread_key_type* >(stg));
    }
};

template< typename KeyT >
struct pthread_key_traits< KeyT, true, true >
{
    typedef KeyT pthread_key_type;

#if defined(BOOST_HAS_INTPTR_T)
    typedef typename boost::conditional<
        boost::is_signed< pthread_key_type >::value,
        intptr_t,
        uintptr_t
    >::type intptr_type;
#else
    typedef typename boost::conditional<
        boost::is_signed< pthread_key_type >::value,
        std::ptrdiff_t,
        std::size_t
    >::type intptr_type;
#endif

    static void allocate(void*& stg)
    {
        pthread_key_type key;
        const int res = pthread_key_create(&key, NULL);
        if (BOOST_UNLIKELY(res != 0))
        {
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "TLS capacity depleted", (res));
        }
        stg = (void*)(intptr_type)key;
    }

    static void deallocate(void* stg)
    {
        pthread_key_delete((pthread_key_type)(intptr_type)stg);
    }

    static void set_value(void* stg, void* value)
    {
        const int res = pthread_setspecific((pthread_key_type)(intptr_type)stg, value);
        if (BOOST_UNLIKELY(res != 0))
        {
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to set TLS value", (res));
        }
    }

    static void* get_value(void* stg)
    {
        return pthread_getspecific((pthread_key_type)(intptr_type)stg);
    }
};

template< typename KeyT >
struct pthread_key_traits< KeyT, true, false >
{
    typedef KeyT pthread_key_type;

    static void allocate(void*& stg)
    {
        pthread_key_type key;
        const int res = pthread_key_create(&key, NULL);
        if (BOOST_UNLIKELY(res != 0))
        {
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "TLS capacity depleted", (res));
        }
        std::memset(&stg, 0, sizeof(stg));
        std::memcpy(&stg, &key, sizeof(pthread_key_type));
    }

    static void deallocate(void* stg)
    {
        pthread_key_type key;
        std::memcpy(&key, &stg, sizeof(pthread_key_type));
        pthread_key_delete(key);
    }

    static void set_value(void* stg, void* value)
    {
        pthread_key_type key;
        std::memcpy(&key, &stg, sizeof(pthread_key_type));
        const int res = pthread_setspecific(key, value);
        if (BOOST_UNLIKELY(res != 0))
        {
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to set TLS value", (res));
        }
    }

    static void* get_value(void* stg)
    {
        pthread_key_type key;
        std::memcpy(&key, &stg, sizeof(pthread_key_type));
        return pthread_getspecific(key);
    }
};

template< typename KeyT >
struct pthread_key_traits< KeyT*, true, false >
{
    typedef KeyT* pthread_key_type;

    static void allocate(void*& stg)
    {
        pthread_key_type key = NULL;
        const int res = pthread_key_create(&key, NULL);
        if (BOOST_UNLIKELY(res != 0))
        {
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "TLS capacity depleted", (res));
        }
        stg = static_cast< void* >(key);
    }

    static void deallocate(void* stg)
    {
        pthread_key_delete(static_cast< pthread_key_type >(stg));
    }

    static void set_value(void* stg, void* value)
    {
        const int res = pthread_setspecific(static_cast< pthread_key_type >(stg), value);
        if (BOOST_UNLIKELY(res != 0))
        {
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to set TLS value", (res));
        }
    }

    static void* get_value(void* stg)
    {
        return pthread_getspecific(static_cast< pthread_key_type >(stg));
    }
};

} // namespace

thread_specific_base::thread_specific_base()
{
    typedef pthread_key_traits< pthread_key_t > traits_t;
    traits_t::allocate(m_Key);
}

thread_specific_base::~thread_specific_base()
{
    typedef pthread_key_traits< pthread_key_t > traits_t;
    traits_t::deallocate(m_Key);
}

void* thread_specific_base::get_content() const
{
    typedef pthread_key_traits< pthread_key_t > traits_t;
    return traits_t::get_value(m_Key);
}

void thread_specific_base::set_content(void* value) const
{
    typedef pthread_key_traits< pthread_key_t > traits_t;
    traits_t::set_value(m_Key, value);
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#else
#error Boost.Log: unsupported threading API
#endif

#include <boost/log/detail/footer.hpp>

#endif // !defined(BOOST_LOG_NO_THREADS)
