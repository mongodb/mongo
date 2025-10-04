/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   thread_id.cpp
 * \author Andrey Semashev
 * \date   08.1.2012
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>

#if !defined(BOOST_LOG_NO_THREADS)

#include <new>
#include <iostream>
#include <boost/throw_exception.hpp>
#if !defined(BOOST_WINDOWS)
#include <cstring>
#include <boost/predef/other/endian.h>
#endif
#include <boost/log/detail/thread_id.hpp>
#if defined(BOOST_LOG_USE_COMPILER_TLS)
#include <boost/aligned_storage.hpp>
#include <boost/type_traits/alignment_of.hpp>
#elif defined(BOOST_WINDOWS)
#include <boost/thread/thread.hpp> // at_thread_exit
#include <boost/log/detail/thread_specific.hpp>
#else
#include <boost/log/exceptions.hpp>
#include <boost/log/utility/once_block.hpp>
#endif
#if !defined(BOOST_LOG_USE_COMPILER_TLS)
#include <boost/log/detail/singleton.hpp>
#endif
#include "id_formatting.hpp"
#include <boost/log/detail/header.hpp>

#if defined(BOOST_WINDOWS)

#include <windows.h>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

enum { tid_size = sizeof(GetCurrentThreadId()) };

BOOST_LOG_ANONYMOUS_NAMESPACE {

    //! The function returns current process identifier
    inline thread::id get_id_impl()
    {
        return thread::id(GetCurrentThreadId());
    }

} // namespace

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#else // defined(BOOST_WINDOWS)

#include <pthread.h>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

enum
{
    headroom_size = sizeof(pthread_t) > sizeof(uintmax_t) ? 0u : (sizeof(uintmax_t) - sizeof(pthread_t)),
    tid_size = sizeof(uintmax_t) - headroom_size
};

BOOST_LOG_ANONYMOUS_NAMESPACE {

    //! The function returns current thread identifier
    inline thread::id get_id_impl()
    {
        // According to POSIX, pthread_t may not be an integer type:
        // http://pubs.opengroup.org/onlinepubs/009695399/basedefs/sys/types.h.html
        // For now we use the hackish cast to get some opaque number that hopefully correlates with system thread identification.
        thread::id::native_type int_id = 0;
        pthread_t pthread_id = pthread_self();
#if BOOST_ENDIAN_BIG_BYTE || BOOST_ENDIAN_BIG_WORD
        std::memcpy(reinterpret_cast< unsigned char* >(&int_id) + headroom_size, &pthread_id, tid_size);
#else
        std::memcpy(&int_id, &pthread_id, tid_size);
#endif
        return thread::id(int_id);
    }

} // namespace

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#endif // defined(BOOST_WINDOWS)


namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

namespace this_thread {

#if defined(BOOST_LOG_USE_COMPILER_TLS)

BOOST_LOG_ANONYMOUS_NAMESPACE {

struct id_storage
{
    aligned_storage< sizeof(thread::id), alignment_of< thread::id >::value >::type m_storage;
    bool m_initialized;
};

BOOST_LOG_TLS id_storage g_id_storage = {};

} // namespace

//! The function returns current thread identifier
BOOST_LOG_API thread::id const& get_id()
{
    id_storage& s = g_id_storage;
    if (BOOST_UNLIKELY(!s.m_initialized))
    {
        new (s.m_storage.address()) thread::id(get_id_impl());
        s.m_initialized = true;
    }

    return *static_cast< thread::id const* >(s.m_storage.address());
}

#elif defined(BOOST_WINDOWS)

BOOST_LOG_ANONYMOUS_NAMESPACE {

struct id_storage :
    lazy_singleton< id_storage >
{
    struct deleter
    {
        typedef void result_type;

        explicit deleter(thread::id const* p) : m_p(p) {}

        result_type operator() () const
        {
            delete m_p;
        }

    private:
        thread::id const* m_p;
    };

    thread_specific< thread::id const* > m_id;
};

} // namespace

//! The function returns current thread identifier
BOOST_LOG_API thread::id const& get_id()
{
    id_storage& s = id_storage::get();
    thread::id const* p = s.m_id.get();
    if (BOOST_UNLIKELY(!p))
    {
        p = new thread::id(get_id_impl());
        s.m_id.set(p);
        boost::this_thread::at_thread_exit(id_storage::deleter(p));
    }

    return *p;
}

#else

BOOST_LOG_ANONYMOUS_NAMESPACE {

pthread_key_t g_key;

void deleter(void* p)
{
    delete static_cast< thread::id* >(p);
}

} // namespace

//! The function returns current thread identifier
BOOST_LOG_API thread::id const& get_id()
{
    BOOST_LOG_ONCE_BLOCK()
    {
        if (int err = pthread_key_create(&g_key, &deleter))
        {
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to create a thread-specific storage for thread id", (err));
        }
    }

    thread::id* p = static_cast< thread::id* >(pthread_getspecific(g_key));
    if (BOOST_UNLIKELY(!p))
    {
        p = new thread::id(get_id_impl());
        pthread_setspecific(g_key, p);
    }

    return *p;
}

#endif

} // namespace this_thread

// Used in default_sink.cpp
void format_thread_id(char* buf, std::size_t size, thread::id tid)
{
    format_id< tid_size >(buf, size, tid.native_id(), false);
}

template< typename CharT, typename TraitsT >
BOOST_LOG_API std::basic_ostream< CharT, TraitsT >& operator<< (std::basic_ostream< CharT, TraitsT >& strm, thread::id const& tid)
{
    if (strm.good())
    {
        CharT buf[tid_size * 2 + 3]; // 2 chars per byte + 3 chars for the leading 0x and terminating zero
        format_id< tid_size >(buf, sizeof(buf) / sizeof(*buf), tid.native_id(), (strm.flags() & std::ios_base::uppercase) != 0);

        strm << buf;
    }

    return strm;
}

#if defined(BOOST_LOG_USE_CHAR)
template BOOST_LOG_API
std::basic_ostream< char, std::char_traits< char > >&
operator<< (std::basic_ostream< char, std::char_traits< char > >& strm, thread::id const& tid);
#endif // defined(BOOST_LOG_USE_CHAR)

#if defined(BOOST_LOG_USE_WCHAR_T)
template BOOST_LOG_API
std::basic_ostream< wchar_t, std::char_traits< wchar_t > >&
operator<< (std::basic_ostream< wchar_t, std::char_traits< wchar_t > >& strm, thread::id const& tid);
#endif // defined(BOOST_LOG_USE_WCHAR_T)

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // !defined(BOOST_LOG_NO_THREADS)
