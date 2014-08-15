//  (C) Copyright Gennadiy Rozental 2005-2008.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision$
//
//  Description : defines facility to hide input traversing details
// ***************************************************************************

#ifndef BOOST_RT_CLA_ARGV_TRAVERSER_HPP_062604GER
#define BOOST_RT_CLA_ARGV_TRAVERSER_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

// Boost.Test
#include <boost/test/utils/class_properties.hpp>

// Boost
#include <boost/noncopyable.hpp>
#include <boost/shared_array.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************          runtime::cla::argv_traverser        ************** //
// ************************************************************************** //

class argv_traverser : noncopyable {
    class parser;
public:
    // Constructor
    argv_traverser();

    // public_properties
    unit_test::readwrite_property<bool>         p_ignore_mismatch;
    unit_test::readwrite_property<char_type>    p_separator;

    // argc+argv <-> internal buffer exchange
    void            init( int argc, char_type** argv );
    void            remainder( int& argc, char_type** argv );

    // token based parsing
    cstring         token() const;
    void            next_token();

    // whole input parsing
    cstring         input() const;
    void            trim( std::size_t size );
    bool            match_front( cstring );
    bool            match_front( char_type c );
    bool            eoi() const;

    // transaction logic support
    void            commit();
    void            rollback();

    // current position access; used to save some reference points in input
    std::size_t     input_pos() const;

    // returns true if mismatch detected during input parsing handled successfully
    bool            handle_mismatch();

private:
    // Data members
    dstring                 m_buffer;
    cstring                 m_work_buffer;

    cstring                 m_token;
    cstring::iterator       m_commited_end;

    shared_array<char_type> m_remainder;
    std::size_t             m_remainder_size;
};

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

#ifndef BOOST_RT_PARAM_OFFLINE

#  define BOOST_RT_PARAM_INLINE inline
#  include <boost/test/utils/runtime/cla/argv_traverser.ipp>

#endif

#endif // BOOST_RT_CLA_ARGV_TRAVERSER_HPP_062604GER
