/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   text_ostream_backend.cpp
 * \author Andrey Semashev
 * \date   19.04.2007
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <vector>
#include <algorithm>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace sinks {

//! Sink implementation
template< typename CharT >
struct basic_text_ostream_backend< CharT >::implementation
{
    //! Type of the container that holds all aggregated streams
    typedef std::vector< shared_ptr< stream_type > > ostream_sequence;

    //! Output stream list
    ostream_sequence m_Streams;
    //! Auto-flush flag
    bool m_fAutoFlush;

    implementation() : m_fAutoFlush(false)
    {
    }
};


//! Constructor
template< typename CharT >
BOOST_LOG_API basic_text_ostream_backend< CharT >::basic_text_ostream_backend() : m_pImpl(new implementation())
{
}

//! Destructor (just to make it link from the shared library)
template< typename CharT >
BOOST_LOG_API basic_text_ostream_backend< CharT >::~basic_text_ostream_backend()
{
    delete m_pImpl;
}

//! The method adds a new stream to the sink
template< typename CharT >
BOOST_LOG_API void basic_text_ostream_backend< CharT >::add_stream(shared_ptr< stream_type > const& strm)
{
    typename implementation::ostream_sequence::iterator it =
        std::find(m_pImpl->m_Streams.begin(), m_pImpl->m_Streams.end(), strm);
    if (it == m_pImpl->m_Streams.end())
    {
        m_pImpl->m_Streams.push_back(strm);
    }
}

//! The method removes a stream from the sink
template< typename CharT >
BOOST_LOG_API void basic_text_ostream_backend< CharT >::remove_stream(shared_ptr< stream_type > const& strm)
{
    typename implementation::ostream_sequence::iterator it =
        std::find(m_pImpl->m_Streams.begin(), m_pImpl->m_Streams.end(), strm);
    if (it != m_pImpl->m_Streams.end())
        m_pImpl->m_Streams.erase(it);
}

//! Sets the flag to automatically flush buffers after each logged line
template< typename CharT >
BOOST_LOG_API void basic_text_ostream_backend< CharT >::auto_flush(bool f)
{
    m_pImpl->m_fAutoFlush = f;
}

//! The method writes the message to the sink
template< typename CharT >
BOOST_LOG_API void basic_text_ostream_backend< CharT >::consume(record_view const&, string_type const& message)
{
    typename string_type::const_pointer const p = message.data();
    typename string_type::size_type const s = message.size();
    typename implementation::ostream_sequence::const_iterator
        it = m_pImpl->m_Streams.begin(), end = m_pImpl->m_Streams.end();
    for (; it != end; ++it)
    {
        stream_type* const strm = it->get();
        if (strm->good())
        {
            strm->write(p, static_cast< std::streamsize >(s));
            strm->put(static_cast< char_type >('\n'));

            if (m_pImpl->m_fAutoFlush)
                strm->flush();
        }
    }
}

//! The method flushes the associated streams
template< typename CharT >
BOOST_LOG_API void basic_text_ostream_backend< CharT >::flush()
{
    typename implementation::ostream_sequence::const_iterator
        it = m_pImpl->m_Streams.begin(), end = m_pImpl->m_Streams.end();
    for (; it != end; ++it)
    {
        stream_type* const strm = it->get();
        if (strm->good())
            strm->flush();
    }
}

//! Explicitly instantiate sink backend implementation
#ifdef BOOST_LOG_USE_CHAR
template class basic_text_ostream_backend< char >;
#endif
#ifdef BOOST_LOG_USE_WCHAR_T
template class basic_text_ostream_backend< wchar_t >;
#endif

} // namespace sinks

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
