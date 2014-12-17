//  (C) Copyright Gennadiy Rozental 2001-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision: 49312 $
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

#endif // BOOST_TEST_OUTPUT_TEST_STREAM_HPP_012705GER
