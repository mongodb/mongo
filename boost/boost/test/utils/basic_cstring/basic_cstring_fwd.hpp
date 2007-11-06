//  (C) Copyright Gennadiy Rozental 2004-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: basic_cstring_fwd.hpp,v $
//
//  Version     : $Revision: 1.4 $
//
//  Description : basic_cstring class wrap C string and provide std_string like 
//                interface
// ***************************************************************************

#ifndef BOOST_TEST_BASIC_CSTRING_FWD_HPP_071894GER
#define BOOST_TEST_BASIC_CSTRING_FWD_HPP_071894GER

#include <boost/detail/workaround.hpp>

namespace boost {

namespace unit_test {

template<typename CharT> class      basic_cstring;
typedef basic_cstring<char const>   const_string;
#if BOOST_WORKAROUND(__DECCXX_VER, BOOST_TESTED_AT(60590041))
typedef const_string                literal_string;
#else
typedef const_string const          literal_string;
#endif

typedef char const* const           c_literal_string;

} // namespace unit_test

} // namespace boost

// ***************************************************************************
//  Revision History :
//  
//  $Log: basic_cstring_fwd.hpp,v $
//  Revision 1.4  2005/04/12 06:49:05  rogeeff
//  assign_to -> assign_op
//
//  Revision 1.3  2005/02/20 08:27:09  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.2  2005/02/01 06:40:08  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.1  2005/01/22 18:21:40  rogeeff
//  moved sharable staff into utils
//
// ***************************************************************************

#endif // BOOST_TEST_BASIC_CSTRING_FWD_HPP_071894GER

