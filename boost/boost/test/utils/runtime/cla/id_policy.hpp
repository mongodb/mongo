//  (C) Copyright Gennadiy Rozental 2005.
//  Use, modification, and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: id_policy.hpp,v $
//
//  Version     : $Revision: 1.1 $
//
//  Description : some generic identification policies definition
// ***************************************************************************

#ifndef BOOST_RT_CLA_ID_POLICY_HPP_062604GER
#define BOOST_RT_CLA_ID_POLICY_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

#include <boost/test/utils/runtime/cla/fwd.hpp>
#include <boost/test/utils/runtime/cla/modifier.hpp>
#include <boost/test/utils/runtime/cla/argv_traverser.hpp>

#include <boost/test/utils/runtime/cla/iface/id_policy.hpp>

// Boost.Test
#include <boost/test/utils/class_properties.hpp>
#include <boost/test/utils/rtti.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************               naming_policy_base             ************** //
// ************************************************************************** //
// model: <prefix> <name> <separtor>

class basic_naming_policy : public identification_policy {
public:
    // Policy interface
    virtual bool    responds_to( cstring name ) const       { return m_name == name; }
    virtual cstring id_2_report() const                     { return m_name; }
    virtual void    usage_info( format_stream& fs ) const;
    virtual bool    matching( parameter const& p, argv_traverser& tr, bool primary ) const;

    // Accept modifer
    template<typename Modifier>
    void            accept_modifier( Modifier const& m )
    {
        nfp::optionally_assign( m_prefix,    m, prefix );
        nfp::optionally_assign( m_name,      m, name );
        nfp::optionally_assign( m_separator, m, separator );
    }

protected:
    explicit basic_naming_policy( rtti::id_t const& dyn_type )
    : identification_policy( dyn_type )
    {}
    BOOST_RT_PARAM_UNNEEDED_VIRTUAL ~basic_naming_policy() {}

    // Naming policy interface
    virtual bool    match_prefix( argv_traverser& tr ) const;
    virtual bool    match_name( argv_traverser& tr ) const;
    virtual bool    match_separator( argv_traverser& tr ) const;

    // Data members
    dstring      m_prefix;
    dstring      m_name;
    dstring      m_separator;
};

// ************************************************************************** //
// **************                 dual_id_policy               ************** //
// ************************************************************************** //

template<typename MostDerived,typename PrimaryId,typename SecondId>
class dual_id_policy : public identification_policy {
public:
    // Constructor
    dual_id_policy()
    : identification_policy( rtti::type_id<MostDerived>() )
    , m_primary()
    , m_secondary()
    {}

    // Policy interface
    virtual bool    responds_to( cstring name ) const
    {
        return m_primary.responds_to( name ) || m_secondary.responds_to( name );
    }
    virtual bool    conflict_with( identification_policy const& id_p ) const
    {
        return m_primary.conflict_with( id_p ) || m_secondary.conflict_with( id_p );
    }
    virtual cstring id_2_report() const
    {
        return m_primary.id_2_report();
    }
    virtual void    usage_info( format_stream& fs ) const
    {
        fs << BOOST_RT_PARAM_LITERAL( '{' );
        m_primary.usage_info( fs );
        fs << BOOST_RT_PARAM_LITERAL( '|' );
        m_secondary.usage_info( fs );
        fs << BOOST_RT_PARAM_LITERAL( '}' );
    }
    virtual bool    matching( parameter const& p, argv_traverser& tr, bool primary ) const
    {
        return m_primary.matching( p, tr, primary ) || m_secondary.matching( p, tr, primary );
    }

protected:
    BOOST_RT_PARAM_UNNEEDED_VIRTUAL ~dual_id_policy() {}

    // Data members
    PrimaryId       m_primary;
    SecondId        m_secondary;
};

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

#ifndef BOOST_RT_PARAM_OFFLINE

#  define BOOST_RT_PARAM_INLINE inline
#  include <boost/test/utils/runtime/cla/id_policy.ipp>

#endif

// ************************************************************************** //
//   Revision History:
//
//   $Log: id_policy.hpp,v $
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_CLA_ID_POLICY_HPP_062604GER
