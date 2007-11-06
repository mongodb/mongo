//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: wrap_io_saver.hpp,v $
//
//  Version     : $Revision: 1.2 $
//
//  Description : wraps io savers staff to be provide workaround for classic iostreams
// ***************************************************************************

#ifndef BOOST_WRAP_IO_SAVER_HPP_011605GER
#define BOOST_WRAP_IO_SAVER_HPP_011605GER

#include <boost/test/detail/suppress_warnings.hpp>

#if defined(BOOST_STANDARD_IOSTREAMS)
#include <boost/io/ios_state.hpp>
#endif

namespace boost {

namespace unit_test {

#if defined(BOOST_STANDARD_IOSTREAMS)

typedef ::boost::io::ios_base_all_saver io_saver_type;

#else

struct io_saver_type {
    explicit io_saver_type( std::ostream& ) {}
    void     restore() {}
};

#endif

} // namespace unit_test

} // namespace boost

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: wrap_io_saver.hpp,v $
//  Revision 1.2  2005/12/14 04:59:11  rogeeff
//  *** empty log message ***
//
//  Revision 1.1  2005/04/30 16:48:21  rogeeff
//  io saver warkaround for classic io is shared
//
//  Revision 1.1  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_WRAP_IO_SAVER_HPP_011605GER

