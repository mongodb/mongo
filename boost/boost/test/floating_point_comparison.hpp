//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: floating_point_comparison.hpp,v $
//
//  Version     : $Revision: 1.26.2.2 $
//
//  Description : defines algoirthms for comparing 2 floating point values
// ***************************************************************************

#ifndef BOOST_TEST_FLOATING_POINT_COMPARISON_HPP_071894GER
#define BOOST_TEST_FLOATING_POINT_COMPARISON_HPP_071894GER

#include <boost/limits.hpp>  // for std::numeric_limits

#include <boost/test/utils/class_properties.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace test_tools {

using unit_test::readonly_property;

// ************************************************************************** //
// **************        floating_point_comparison_type        ************** //
// ************************************************************************** //

enum floating_point_comparison_type {
    FPC_STRONG, // "Very close"   - equation 1' in docs, the default
    FPC_WEAK    // "Close enough" - equation 2' in docs.

};

// ************************************************************************** //
// **************                    details                   ************** //
// ************************************************************************** //

namespace tt_detail {

// FPT is Floating-Point type, float, double, long double or User-Defined.

template<typename FPT>
inline FPT
fpt_abs( FPT arg ) 
{
    return arg < static_cast<FPT>(0) ? -arg : arg;
}

//____________________________________________________________________________//

// both f1 and f2 are unsigned here
template<typename FPT>
inline FPT 
safe_fpt_division( FPT f1, FPT f2 )
{
    // Avoid overflow.
    if( f2 < static_cast<FPT>(1)  && f1 > f2 * (std::numeric_limits<FPT>::max)() )
        return (std::numeric_limits<FPT>::max)();

    // Avoid underflow.
    if( f1 == static_cast<FPT>(0) || 
        f2 > static_cast<FPT>(1) && f1 < f2 * (std::numeric_limits<FPT>::min)() )
        return static_cast<FPT>(0);

    return f1/f2;
}

//____________________________________________________________________________//

} // namespace tt_detail

// ************************************************************************** //
// **************         tolerance presentation types         ************** //
// ************************************************************************** //

template<typename FPT>
struct percent_tolerance_t {
    explicit    percent_tolerance_t( FPT v ) : m_value( v ) {}

    FPT m_value;
};

//____________________________________________________________________________//

template<typename Out,typename FPT>
Out& operator<<( Out& out, percent_tolerance_t<FPT> t )
{
    return out << t.m_value;
}

//____________________________________________________________________________//

template<typename FPT>
inline percent_tolerance_t<FPT>
percent_tolerance( FPT v )
{
    return percent_tolerance_t<FPT>( v );
}

//____________________________________________________________________________//

template<typename FPT>
struct fraction_tolerance_t {
    explicit fraction_tolerance_t( FPT v ) : m_value( v ) {}

    FPT m_value;
};

//____________________________________________________________________________//

template<typename Out,typename FPT>
Out& operator<<( Out& out, fraction_tolerance_t<FPT> t )
{
    return out << t.m_value;
}

//____________________________________________________________________________//

template<typename FPT>
inline fraction_tolerance_t<FPT>
fraction_tolerance( FPT v )
{
    return fraction_tolerance_t<FPT>( v );
}

//____________________________________________________________________________//

// ************************************************************************** //
// **************             close_at_tolerance               ************** //
// ************************************************************************** //

template<typename FPT>
class close_at_tolerance {
public:
    // Public typedefs
    typedef bool result_type;

    // Constructor
    template<typename ToleranceBaseType>
    explicit    close_at_tolerance( percent_tolerance_t<ToleranceBaseType>  tolerance, 
                                    floating_point_comparison_type          fpc_type = FPC_STRONG ) 
    : p_fraction_tolerance( tt_detail::fpt_abs( static_cast<FPT>(0.01)*tolerance.m_value ) )
    , p_strong_or_weak( fpc_type ==  FPC_STRONG )
    {}
    template<typename ToleranceBaseType>
    explicit    close_at_tolerance( fraction_tolerance_t<ToleranceBaseType> tolerance, 
                                    floating_point_comparison_type          fpc_type = FPC_STRONG ) 
    : p_fraction_tolerance( tt_detail::fpt_abs( tolerance.m_value ) )
    , p_strong_or_weak( fpc_type ==  FPC_STRONG )
    {}

    bool        operator()( FPT left, FPT right ) const
    {
        FPT diff = tt_detail::fpt_abs( left - right );
        FPT d1   = tt_detail::safe_fpt_division( diff, tt_detail::fpt_abs( right ) );
        FPT d2   = tt_detail::safe_fpt_division( diff, tt_detail::fpt_abs( left ) );
        
        return p_strong_or_weak 
                   ? (d1 <= p_fraction_tolerance && d2 <= p_fraction_tolerance) 
                   : (d1 <= p_fraction_tolerance || d2 <= p_fraction_tolerance);
    }

    // Public properties
    readonly_property<FPT>  p_fraction_tolerance;
    readonly_property<bool> p_strong_or_weak;
};

//____________________________________________________________________________//

// ************************************************************************** //
// **************               check_is_close                 ************** //
// ************************************************************************** //

struct BOOST_TEST_DECL check_is_close_t {
    // Public typedefs
    typedef bool result_type;

    template<typename FPT, typename ToleranceBaseType>
    bool
    operator()( FPT left, FPT right, percent_tolerance_t<ToleranceBaseType> tolerance, 
                floating_point_comparison_type fpc_type = FPC_STRONG )
    {
        close_at_tolerance<FPT> pred( tolerance, fpc_type );

        return pred( left, right );
    }
    template<typename FPT, typename ToleranceBaseType>
    bool
    operator()( FPT left, FPT right, fraction_tolerance_t<ToleranceBaseType> tolerance, 
                floating_point_comparison_type fpc_type = FPC_STRONG )
    {
        close_at_tolerance<FPT> pred( tolerance, fpc_type );

        return pred( left, right );
    }
};

namespace {
check_is_close_t check_is_close;
}

//____________________________________________________________________________//

// ************************************************************************** //
// **************               check_is_small                 ************** //
// ************************************************************************** //

struct BOOST_TEST_DECL check_is_small_t {
    // Public typedefs
    typedef bool result_type;

    template<typename FPT>
    bool
    operator()( FPT fpv, FPT tolerance )
    {
        return tt_detail::fpt_abs( fpv ) < tt_detail::fpt_abs( tolerance );
    }
};

namespace {
check_is_small_t check_is_small;
}

//____________________________________________________________________________//

} // namespace test_tools

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: floating_point_comparison.hpp,v $
//  Revision 1.26.2.2  2006/11/30 14:41:21  jhunold
//  Merge from HEAD: Remove unnecessary export makro.
//
//  Revision 1.26.2.1  2006/05/22 17:39:12  johnmaddock
//  Fix min/max guidelines violation.
//
//  Revision 1.26  2006/03/16 07:31:06  vladimir_prus
//  Fix compile error on MSVC due to max and min being defined as macros.
//
//  Revision 1.25  2006/03/13 18:28:25  rogeeff
//  warnings eliminated
//
//  Revision 1.24  2005/12/14 05:07:28  rogeeff
//  introduced an ability to test on closeness based on either percentage dirven tolerance or fraction driven one
//
//  Revision 1.23  2005/05/29 08:54:57  rogeeff
//  allow bind usage
//
//  Revision 1.22  2005/02/21 10:21:40  rogeeff
//  check_is_small implemented
//  check functions implemented as function objects
//
//  Revision 1.21  2005/02/20 08:27:05  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.20  2005/02/01 06:40:06  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.19  2005/01/22 19:22:12  rogeeff
//  implementation moved into headers section to eliminate dependency of included/minimal component on src directory
//
// ***************************************************************************

#endif // BOOST_FLOATING_POINT_COMAPARISON_HPP_071894GER

