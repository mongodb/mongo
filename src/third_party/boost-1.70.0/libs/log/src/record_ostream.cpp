/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   record_ostream.cpp
 * \author Andrey Semashev
 * \date   17.04.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <locale>
#include <utility>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/detail/singleton.hpp>
#include <boost/log/attributes/attribute_value_impl.hpp>
#include <boost/log/expressions/message.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/thread/tss.hpp>
#endif
#include "unique_ptr.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

//! The function initializes the stream and the stream buffer
template< typename CharT >
BOOST_LOG_API void basic_record_ostream< CharT >::init_stream()
{
    base_type::init_stream();
    base_type::imbue(std::locale());
    if (m_record)
    {
        typedef attributes::attribute_value_impl< string_type > message_impl_type;
        intrusive_ptr< message_impl_type > p = new message_impl_type(string_type());
        attribute_value value(p);

        // This may fail if the record already has Message attribute
        std::pair< attribute_value_set::const_iterator, bool > res =
            m_record->attribute_values().insert(expressions::tag::message::get_name(), value);
        if (!res.second)
            const_cast< attribute_value& >(res.first->second).swap(value);

        base_type::attach(const_cast< string_type& >(p->get()));
    }
}
//! The function resets the stream into a detached (default initialized) state
template< typename CharT >
BOOST_LOG_API void basic_record_ostream< CharT >::detach_from_record() BOOST_NOEXCEPT
{
    if (m_record)
    {
        base_type::detach();
        m_record = NULL;
        base_type::exceptions(base_type::goodbit);
    }
}

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! The pool of stream compounds
template< typename CharT >
class stream_compound_pool :
    public log::aux::lazy_singleton<
        stream_compound_pool< CharT >,
#if !defined(BOOST_LOG_NO_THREADS)
        thread_specific_ptr< stream_compound_pool< CharT > >
#else
        log::aux::unique_ptr< stream_compound_pool< CharT > >
#endif
    >
{
    //! Self type
    typedef stream_compound_pool< CharT > this_type;
#if !defined(BOOST_LOG_NO_THREADS)
    //! Thread-specific pointer type
    typedef thread_specific_ptr< this_type > tls_ptr_type;
#else
    //! Thread-specific pointer type
    typedef log::aux::unique_ptr< this_type > tls_ptr_type;
#endif
    //! Singleton base type
    typedef log::aux::lazy_singleton<
        this_type,
        tls_ptr_type
    > base_type;
    //! Stream compound type
    typedef typename stream_provider< CharT >::stream_compound stream_compound_t;

public:
    //! Pooled stream compounds
    stream_compound_t* m_Top;

    ~stream_compound_pool()
    {
        stream_compound_t* p = NULL;
        while ((p = m_Top) != NULL)
        {
            m_Top = p->next;
            delete p;
        }
    }

    //! The method returns pool instance
    static stream_compound_pool& get()
    {
        tls_ptr_type& ptr = base_type::get();
        this_type* p = ptr.get();
        if (!p)
        {
            log::aux::unique_ptr< this_type > pNew(new this_type());
            ptr.reset(pNew.get());
            p = pNew.release();
        }
        return *p;
    }

private:
    stream_compound_pool() : m_Top(NULL) {}
};

} // namespace

//! The method returns an allocated stream compound
template< typename CharT >
BOOST_LOG_API typename stream_provider< CharT >::stream_compound*
stream_provider< CharT >::allocate_compound(record& rec)
{
    stream_compound_pool< char_type >& pool = stream_compound_pool< char_type >::get();
    if (pool.m_Top)
    {
        stream_compound* p = pool.m_Top;
        pool.m_Top = p->next;
        p->next = NULL;
        p->stream.attach_record(rec);
        return p;
    }
    else
        return new stream_compound(rec);
}

//! The method releases a compound
template< typename CharT >
BOOST_LOG_API void stream_provider< CharT >::release_compound(stream_compound* compound) BOOST_NOEXCEPT
{
    stream_compound_pool< char_type >& pool = stream_compound_pool< char_type >::get();
    compound->next = pool.m_Top;
    pool.m_Top = compound;
    compound->stream.detach_from_record();
}

//! Explicitly instantiate stream_provider implementation
#ifdef BOOST_LOG_USE_CHAR
template struct stream_provider< char >;
#endif
#ifdef BOOST_LOG_USE_WCHAR_T
template struct stream_provider< wchar_t >;
#endif

} // namespace aux

//! Explicitly instantiate basic_record_ostream implementation
#ifdef BOOST_LOG_USE_CHAR
template class basic_record_ostream< char >;
#endif
#ifdef BOOST_LOG_USE_WCHAR_T
template class basic_record_ostream< wchar_t >;
#endif

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
