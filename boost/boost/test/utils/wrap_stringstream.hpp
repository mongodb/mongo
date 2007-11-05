//  (C) Copyright Gennadiy Rozental 2002-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: wrap_stringstream.hpp,v $
//
//  Version     : $Revision: 1.9 $
//
//  Description : wraps strstream and stringstream (depends with one is present)
//                to provide the unified interface
// ***************************************************************************

#ifndef BOOST_WRAP_STRINGSTREAM_HPP_071894GER
#define BOOST_WRAP_STRINGSTREAM_HPP_071894GER

// Boost.Test
#include <boost/test/detail/config.hpp>

// STL
#ifdef BOOST_NO_STRINGSTREAM
#include <strstream>        // for std::ostrstream
#else
#include <sstream>          // for std::ostringstream
#endif // BOOST_NO_STRINGSTREAM

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

// ************************************************************************** //
// **************            basic_wrap_stringstream           ************** //
// ************************************************************************** //

template<typename CharT>
class basic_wrap_stringstream {
public:
#if defined(BOOST_CLASSIC_IOSTREAMS)
    typedef std::ostringstream               wrapped_stream;
#elif defined(BOOST_NO_STRINGSTREAM)
    typedef std::basic_ostrstream<CharT>     wrapped_stream;
#else
    typedef std::basic_ostringstream<CharT>  wrapped_stream;
#endif // BOOST_NO_STRINGSTREAM
    // Access methods
    basic_wrap_stringstream&        ref();
    wrapped_stream&                 stream();
    std::basic_string<CharT> const& str();

private:
    // Data members
    wrapped_stream                  m_stream;
    std::basic_string<CharT>        m_str;
};

//____________________________________________________________________________//

template <typename CharT, typename T>
inline basic_wrap_stringstream<CharT>&
operator<<( basic_wrap_stringstream<CharT>& targ, T const& t )
{
    targ.stream() << t;
    return targ;
}

//____________________________________________________________________________//

template <typename CharT>
inline typename basic_wrap_stringstream<CharT>::wrapped_stream&
basic_wrap_stringstream<CharT>::stream()
{
    return m_stream;
}

//____________________________________________________________________________//

template <typename CharT>
inline basic_wrap_stringstream<CharT>&
basic_wrap_stringstream<CharT>::ref()
{ 
    return *this;
}

//____________________________________________________________________________//

template <typename CharT>
inline std::basic_string<CharT> const&
basic_wrap_stringstream<CharT>::str()
{

#ifdef BOOST_NO_STRINGSTREAM
    m_str.assign( m_stream.str(), m_stream.pcount() );
    m_stream.freeze( false );
#else
    m_str = m_stream.str();
#endif

    return m_str;
}

//____________________________________________________________________________//

template <typename CharT>
inline basic_wrap_stringstream<CharT>&
operator<<( basic_wrap_stringstream<CharT>& targ, basic_wrap_stringstream<CharT>& src )
{
    targ << src.str();
    return targ;
}

//____________________________________________________________________________//

#if !defined(BOOST_NO_STD_LOCALE) &&                                    \
    (!defined(BOOST_MSVC) || BOOST_WORKAROUND(BOOST_MSVC, >= 1310))  && \
    !defined(__MWERKS__) && !BOOST_WORKAROUND(__GNUC__, < 3)

template <typename CharT>
inline basic_wrap_stringstream<CharT>&
operator<<( basic_wrap_stringstream<CharT>& targ, std::ios_base& (BOOST_TEST_CALL_DECL *man)(std::ios_base&) )
{
    targ.stream() << man;
    return targ;
}

//____________________________________________________________________________//

template<typename CharT,typename Elem,typename Tr>
inline basic_wrap_stringstream<CharT>&
operator<<( basic_wrap_stringstream<CharT>& targ, std::basic_ostream<Elem,Tr>& (BOOST_TEST_CALL_DECL *man)(std::basic_ostream<Elem, Tr>&) )
{
    targ.stream() << man;
    return targ;
}

//____________________________________________________________________________//

template<typename CharT,typename Elem,typename Tr>
inline basic_wrap_stringstream<CharT>&
operator<<( basic_wrap_stringstream<CharT>& targ, std::basic_ios<Elem, Tr>& (BOOST_TEST_CALL_DECL *man)(std::basic_ios<Elem, Tr>&) )
{
    targ.stream() << man;
    return targ;
}

//____________________________________________________________________________//

#endif

// ************************************************************************** //
// **************               wrap_stringstream              ************** //
// ************************************************************************** //

typedef basic_wrap_stringstream<char>       wrap_stringstream;
typedef basic_wrap_stringstream<wchar_t>    wrap_wstringstream;

}  // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: wrap_stringstream.hpp,v $
//  Revision 1.9  2005/05/13 05:55:46  rogeeff
//  gcc 2.95 fix
//
//  Revision 1.8  2005/05/08 08:55:09  rogeeff
//  typos and missing descriptions fixed
//
//  Revision 1.7  2005/04/30 17:55:15  rogeeff
//  disable manipulator output for cw
//
//  Revision 1.6  2005/02/20 08:27:08  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.5  2005/02/01 06:40:07  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.4  2005/01/31 07:50:06  rogeeff
//  cdecl portability fix
//
//  Revision 1.3  2005/01/31 06:02:15  rogeeff
//  BOOST_TEST_CALL_DECL correctness fixes
//
//  Revision 1.2  2005/01/30 01:43:57  rogeeff
//  warnings suppressed
//
//  Revision 1.1  2005/01/22 18:21:40  rogeeff
//  moved sharable staff into utils
//
// ***************************************************************************

#endif  // BOOST_WRAP_STRINGSTREAM_HPP_071894GER
