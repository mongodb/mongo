//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: output_test_stream.hpp,v $
//
//  Version     : $Revision: 1.5 $
//
//  Description : output_test_stream class definition
// ***************************************************************************

#ifndef BOOST_TEST_OUTPUT_TEST_STREAM_HPP_012705GER
#define BOOST_TEST_OUTPUT_TEST_STREAM_HPP_012705GER

// Boost.Test
#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/utils/wrap_stringstream.hpp>
#include <boost/test/predicate_result.hpp>

// STL
#include <cstddef>          // for std::size_t

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

// ************************************************************************** //
// **************               output_test_stream             ************** //
// ************************************************************************** //

// class to be used to simplify testing of ostream-based output operations

namespace boost {

namespace test_tools {

class BOOST_TEST_DECL output_test_stream : public wrap_stringstream::wrapped_stream {
    typedef unit_test::const_string const_string;
    typedef predicate_result        result_type;
public:
    // Constructor
    explicit        output_test_stream( const_string    pattern_file_name = const_string(),
                                        bool            match_or_save     = true,
                                        bool            text_or_binary    = true );

    // Destructor
    ~output_test_stream();

    // checking function
    result_type     is_empty( bool flush_stream = true );
    result_type     check_length( std::size_t length, bool flush_stream = true );
    result_type     is_equal( const_string arg_, bool flush_stream = true );
    result_type     match_pattern( bool flush_stream = true );

    // explicit flush
    void            flush();

private:
    // helper functions
    std::size_t     length();
    void            sync();

    struct Impl;
    Impl*           m_pimpl;
};

} // namespace test_tools

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: output_test_stream.hpp,v $
//  Revision 1.5  2005/12/14 05:10:34  rogeeff
//  dll support introduced
//  introduced an ability to match agains binary openned file
//
//  Revision 1.4  2005/03/23 21:02:15  rogeeff
//  Sunpro CC 5.3 fixes
//
//  Revision 1.3  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.2  2005/02/01 06:40:06  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.1  2005/01/30 03:25:24  rogeeff
//  output_test_stream moved into separate file
//
// ***************************************************************************

#endif // BOOST_TEST_OUTPUT_TEST_STREAM_HPP_012705GER
