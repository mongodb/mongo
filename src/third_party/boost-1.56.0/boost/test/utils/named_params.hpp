//  (C) Copyright Gennadiy Rozental 2005-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision$
//
//  Description : facilities for named function parameters support
// ***************************************************************************

#ifndef BOOST_TEST_NAMED_PARAM_022505GER
#define BOOST_TEST_NAMED_PARAM_022505GER

// Boost
#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

// Boost.Test
#include <boost/test/utils/rtti.hpp>
#include <boost/test/utils/assign_op.hpp>

#include <boost/type_traits/remove_reference.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace nfp { // named function parameters

// ************************************************************************** //
// **************              forward declarations            ************** //
// ************************************************************************** //

template<typename T, typename unique_id,typename RefType>   struct named_parameter;
template<typename unique_id,bool required>                  struct keyword;

namespace nfp_detail {

template<typename NP1,typename NP2>        struct named_parameter_combine;

// ************************************************************************** //
// **************          access_to_invalid_parameter         ************** //
// ************************************************************************** //

struct access_to_invalid_parameter {};

//____________________________________________________________________________//

inline void 
report_access_to_invalid_parameter()
{
    throw access_to_invalid_parameter();
}

//____________________________________________________________________________//

// ************************************************************************** //
// **************                       nil                    ************** //
// ************************************************************************** //

struct nil {
    template<typename T>
#if defined(__GNUC__) || defined(__HP_aCC) || defined(__EDG__) || defined(__SUNPRO_CC)
    operator T() const
#else
    operator T const&() const
#endif
    { report_access_to_invalid_parameter(); static T* v = 0; return *v; }

    template<typename T>
    T any_cast() const
    { report_access_to_invalid_parameter(); static typename remove_reference<T>::type* v = 0; return *v; }

    template<typename Arg1>
    nil operator()( Arg1 const& )
    { report_access_to_invalid_parameter(); return nil(); }

    template<typename Arg1,typename Arg2>
    nil operator()( Arg1 const&, Arg2 const& )
    { report_access_to_invalid_parameter(); return nil(); }

    template<typename Arg1,typename Arg2,typename Arg3>
    nil operator()( Arg1 const&, Arg2 const&, Arg3 const& )
    { report_access_to_invalid_parameter(); return nil(); }

    // Visitation support
    template<typename Visitor>
    void            apply_to( Visitor& V ) const {}

    static nil&     inst() { static nil s_inst; return s_inst; }
private:
    nil() {}
};
    
// ************************************************************************** //
// **************              named_parameter_base            ************** //
// ************************************************************************** //

template<typename Derived>
struct named_parameter_base {
    template<typename NP>
    named_parameter_combine<NP,Derived>
    operator,( NP const& np ) const { return named_parameter_combine<NP,Derived>( np, *static_cast<Derived const*>(this) ); }
};

//____________________________________________________________________________//

// ************************************************************************** //
// **************             named_parameter_combine          ************** //
// ************************************************************************** //

template<typename NP, typename Rest = nil>
struct named_parameter_combine 
: Rest
, named_parameter_base<named_parameter_combine<NP,Rest> > {
    typedef typename NP::ref_type  res_type;
    typedef named_parameter_combine<NP,Rest> self_type;

    // Constructor
    named_parameter_combine( NP const& np, Rest const& r )
    : Rest( r )
    , m_param( np )
    {}

    // Access methods
    res_type    operator[]( keyword<typename NP::id,true> kw ) const    { return m_param[kw]; }
    res_type    operator[]( keyword<typename NP::id,false> kw ) const   { return m_param[kw]; }
    using       Rest::operator[];

    bool        has( keyword<typename NP::id,false> kw ) const          { return m_param.has( kw ); }
    using       Rest::has;

    void        erase( keyword<typename NP::id,false> kw ) const        { m_param.erase( kw ); }
    using       Rest::erase;

#if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3206)) || \
    BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x0610))
    template<typename NP>
    named_parameter_combine<NP,self_type> operator,( NP const& np ) const
    { return named_parameter_combine<NP,self_type>( np, *this ); }
#else
    using       named_parameter_base<named_parameter_combine<NP,Rest> >::operator,;
#endif

    // Visitation support
    template<typename Visitor>
    void            apply_to( Visitor& V ) const
    {
        m_param.apply_to( V );

        Rest::apply_to( V );
    }
private:
    // Data members
    NP          m_param;
};

} // namespace nfp_detail

// ************************************************************************** //
// **************                 named_parameter              ************** //
// ************************************************************************** //

template<typename T, typename unique_id,typename ReferenceType=T&>
struct named_parameter
: nfp_detail::named_parameter_base<named_parameter<T, unique_id,ReferenceType> >
{
    typedef nfp_detail::nil nil_t;
    typedef T               data_type;
    typedef ReferenceType   ref_type;
    typedef unique_id       id;

    // Constructor
    explicit        named_parameter( ref_type v ) 
    : m_value( v )
    , m_erased( false )
    {}
    named_parameter( named_parameter const& np )
    : m_value( np.m_value )
    , m_erased( np.m_erased )
    {}

    // Access methods
    ref_type        operator[]( keyword<unique_id,true> ) const     { return m_erased ? nil_t::inst().template any_cast<ref_type>() :  m_value; }
    ref_type        operator[]( keyword<unique_id,false> ) const    { return m_erased ? nil_t::inst().template any_cast<ref_type>() :  m_value; }
    template<typename UnknownId>
    nil_t           operator[]( keyword<UnknownId,false> ) const    { return nil_t::inst(); }

    bool            has( keyword<unique_id,false> ) const           { return !m_erased; }
    template<typename UnknownId>
    bool            has( keyword<UnknownId,false> ) const           { return false; }

    void            erase( keyword<unique_id,false> ) const         { m_erased = true; }
    template<typename UnknownId>
    void            erase( keyword<UnknownId,false> ) const         {}

    // Visitation support
    template<typename Visitor>
    void            apply_to( Visitor& V ) const
    {
        V.set_parameter( rtti::type_id<unique_id>(), m_value );
    }

private:
    // Data members
    ref_type        m_value;
    mutable bool    m_erased;
};

//____________________________________________________________________________//

// ************************************************************************** //
// **************                    no_params                 ************** //
// ************************************************************************** //

namespace nfp_detail {
typedef named_parameter<char, struct no_params_type_t,char> no_params_type;
} // namespace nfp_detail

namespace {
nfp_detail::no_params_type no_params( '\0' );
} // local namespace

//____________________________________________________________________________//

// ************************************************************************** //
// **************                     keyword                  ************** //
// ************************************************************************** //

template<typename unique_id, bool required = false>
struct keyword {
    typedef unique_id id;

    template<typename T>
    named_parameter<T const,unique_id>
    operator=( T const& t ) const       { return named_parameter<T const,unique_id>( t ); }

    template<typename T>
    named_parameter<T,unique_id>
    operator=( T& t ) const   { return named_parameter<T,unique_id>( t ); }

    named_parameter<char const*,unique_id,char const*>
    operator=( char const* t ) const   { return named_parameter<char const*,unique_id,char const*>( t ); }
};

//____________________________________________________________________________//

// ************************************************************************** //
// **************                  typed_keyword               ************** //
// ************************************************************************** //

template<typename T, typename unique_id, bool required = false>
struct typed_keyword : keyword<unique_id,required> {
    named_parameter<T const,unique_id>
    operator=( T const& t ) const       { return named_parameter<T const,unique_id>( t ); }

    named_parameter<T,unique_id>
    operator=( T& t ) const             { return named_parameter<T,unique_id>( t ); }
};

//____________________________________________________________________________//

template<typename unique_id>
struct typed_keyword<bool,unique_id,false>
: keyword<unique_id,false>
, named_parameter<bool,unique_id,bool> {
    typedef unique_id id;

    typed_keyword() : named_parameter<bool,unique_id,bool>( true ) {}

    named_parameter<bool,unique_id,bool>
    operator!() const           { return named_parameter<bool,unique_id,bool>( false ); }
};

//____________________________________________________________________________//

// ************************************************************************** //
// **************                optionally_assign             ************** //
// ************************************************************************** //

template<typename T>
inline void
optionally_assign( T&, nfp_detail::nil )
{
    nfp_detail::report_access_to_invalid_parameter();
}

//____________________________________________________________________________//

template<typename T, typename Source>
inline void
#if BOOST_WORKAROUND( __MWERKS__, BOOST_TESTED_AT( 0x3003 ) ) \
    || BOOST_WORKAROUND( __DECCXX_VER, BOOST_TESTED_AT(60590042) )
optionally_assign( T& target, Source src )
#else
optionally_assign( T& target, Source const& src )
#endif
{
    using namespace unit_test;

    assign_op( target, src, static_cast<int>(0) );
}

//____________________________________________________________________________//

template<typename T, typename Params, typename Keyword>
inline void
optionally_assign( T& target, Params const& p, Keyword k )
{
    if( p.has(k) )
        optionally_assign( target, p[k] );
}

//____________________________________________________________________________//

} // namespace nfp

} // namespace boost

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_NAMED_PARAM_022505GER

