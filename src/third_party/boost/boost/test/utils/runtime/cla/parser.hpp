//  (C) Copyright Gennadiy Rozental 2005-2008.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision: 57992 $
//
//  Description : defines parser - public interface for CLA parsing and accessing
// ***************************************************************************

#ifndef BOOST_RT_CLA_PARSER_HPP_062604GER
#define BOOST_RT_CLA_PARSER_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>
#include <boost/test/utils/runtime/fwd.hpp>
#include <boost/test/utils/runtime/argument.hpp>

#include <boost/test/utils/runtime/cla/fwd.hpp>
#include <boost/test/utils/runtime/cla/modifier.hpp>
#include <boost/test/utils/runtime/cla/argv_traverser.hpp>

// Boost
#include <boost/optional.hpp>

// STL
#include <list>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************             runtime::cla::parser             ************** //
// ************************************************************************** //

namespace cla_detail {

template<typename Modifier>
class global_mod_parser {
public:
    global_mod_parser( parser& p, Modifier const& m )
    : m_parser( p )
    , m_modifiers( m )
    {}

    template<typename Param>
    global_mod_parser const&
    operator<<( shared_ptr<Param> param ) const
    {
        param->accept_modifier( m_modifiers );

        m_parser << param;

        return *this;
    }

private:
    // Data members;
    parser&             m_parser;
    Modifier const&     m_modifiers;
};

}

// ************************************************************************** //
// **************             runtime::cla::parser             ************** //
// ************************************************************************** //

class parser {
public:
    typedef std::list<parameter_ptr>::const_iterator param_iterator;

    // Constructor
    explicit            parser( cstring program_name = cstring() );

    // parameter list construction interface
    parser&             operator<<( parameter_ptr param );

    // parser and global parameters modifiers
    template<typename Modifier>
    cla_detail::global_mod_parser<Modifier>
    operator-( Modifier const& m )
    {
        nfp::optionally_assign( m_traverser.p_separator.value, m, input_separator );
        nfp::optionally_assign( m_traverser.p_ignore_mismatch.value, m, ignore_mismatch_m );

        return cla_detail::global_mod_parser<Modifier>( *this, m );
    }

    // input processing method
    void                parse( int& argc, char_type** argv );

    // parameters access
    param_iterator      first_param() const;
    param_iterator      last_param() const;

    // arguments access
    const_argument_ptr  operator[]( cstring string_id ) const;
    cstring             get( cstring string_id ) const;    

    template<typename T>
    T const&            get( cstring string_id ) const
    {
        return arg_value<T>( valid_argument( string_id ) );
    }

    template<typename T>
    void                get( cstring string_id, boost::optional<T>& res ) const
    {
        const_argument_ptr actual_arg = (*this)[string_id];

        if( actual_arg )
            res = arg_value<T>( *actual_arg );
        else
            res.reset();
    }

    // help/usage
    void                usage( out_stream& ostr );
    void                help(  out_stream& ostr );

private:
    argument const&     valid_argument( cstring string_id ) const;

    // Data members
    argv_traverser              m_traverser;
    std::list<parameter_ptr>    m_parameters;
    dstring                     m_program_name;
};

//____________________________________________________________________________//

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

#ifndef BOOST_RT_PARAM_OFFLINE

#  define BOOST_RT_PARAM_INLINE inline
#  include <boost/test/utils/runtime/cla/parser.ipp>

#endif

#endif // BOOST_RT_CLA_PARSER_HPP_062604GER
