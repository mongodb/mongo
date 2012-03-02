//  (C) Copyright Gennadiy Rozental 2001-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision: 57992 $
//
//  Description : defines algoirthms for comparing 2 floating point values
// ***************************************************************************

#ifndef BOOST_TEST_FLOATING_POINT_COMPARISON_HPP_071894GER
#define BOOST_TEST_FLOATING_POINT_COMPARISON_HPP_071894GER

// Boost.Test
#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/utils/class_properties.hpp>
#include <boost/test/predicate_result.hpp>

// Boost
#include <boost/limits.hpp>  // for std::numeric_limits
#include <boost/numeric/conversion/conversion_traits.hpp> // for numeric::conversion_traits
#include <boost/static_assert.hpp>

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

// FPT is Floating-Point Type: float, double, long double or User-Defined.
template<typename FPT>
inline FPT
fpt_abs( FPT fpv ) 
{
    return fpv < static_cast<FPT>(0) ? -fpv : fpv;
}

//____________________________________________________________________________//

template<typename FPT>
struct fpt_limits {
    static FPT    min_value()
    {
        return std::numeric_limits<FPT>::is_specialized
                    ? (std::numeric_limits<FPT>::min)()
                    : 0;
    }
    static FPT    max_value()
    {
        return std::numeric_limits<FPT>::is_specialized
                    ? (std::numeric_limits<FPT>::max)()
                    : static_cast<FPT>(1000000); // for the our purpuses it doesn't really matter what value is returned here
    }
};

//____________________________________________________________________________//

// both f1 and f2 are unsigned here
template<typename FPT>
inline FPT
safe_fpt_division( FPT f1, FPT f2 )
{
    // Avoid overflow.
    if( (f2 < static_cast<FPT>(1))  && (f1 > f2*fpt_limits<FPT>::max_value()) )
        return fpt_limits<FPT>::max_value();

    // Avoid underflow.
    if( (f1 == static_cast<FPT>(0)) ||
        ((f2 > static_cast<FPT>(1)) && (f1 < f2*fpt_limits<FPT>::min_value())) )
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
    , m_report_modifier( 100. )
    {}
    template<typename ToleranceBaseType>
    explicit    close_at_tolerance( fraction_tolerance_t<ToleranceBaseType> tolerance, 
                                    floating_point_comparison_type          fpc_type = FPC_STRONG ) 
    : p_fraction_tolerance( tt_detail::fpt_abs( tolerance.m_value ) )
    , p_strong_or_weak( fpc_type ==  FPC_STRONG )
    , m_report_modifier( 1. )
    {}

    predicate_result        operator()( FPT left, FPT right ) const
    {
        FPT diff = tt_detail::fpt_abs( left - right );
        FPT d1   = tt_detail::safe_fpt_division( diff, tt_detail::fpt_abs( right ) );
        FPT d2   = tt_detail::safe_fpt_division( diff, tt_detail::fpt_abs( left ) );
        
        predicate_result res( p_strong_or_weak 
            ? (d1 <= p_fraction_tolerance.get() && d2 <= p_fraction_tolerance.get()) 
            : (d1 <= p_fraction_tolerance.get() || d2 <= p_fraction_tolerance.get()) );

        if( !res )
            res.message() << (( d1 <= p_fraction_tolerance.get() ? d2 : d1 ) * m_report_modifier);

        return res;
    }

    // Public properties
    readonly_property<FPT>  p_fraction_tolerance;
    readonly_property<bool> p_strong_or_weak;
private:
    // Data members
    FPT                     m_report_modifier;
};

//____________________________________________________________________________//

// ************************************************************************** //
// **************               check_is_close                 ************** //
// ************************************************************************** //

struct BOOST_TEST_DECL check_is_close_t {
    // Public typedefs
    typedef bool result_type;

    template<typename FPT1, typename FPT2, typename ToleranceBaseType>
    predicate_result
    operator()( FPT1 left, FPT2 right, percent_tolerance_t<ToleranceBaseType> tolerance, 
                floating_point_comparison_type fpc_type = FPC_STRONG ) const
    {
        // deduce "better" type from types of arguments being compared
        // if one type is floating and the second integral we use floating type and 
        // value of integral type is promoted to the floating. The same for float and double
        // But we don't want to compare two values of integral types using this tool.
        typedef typename numeric::conversion_traits<FPT1,FPT2>::supertype FPT;
        BOOST_STATIC_ASSERT( !is_integral<FPT>::value );

        close_at_tolerance<FPT> pred( tolerance, fpc_type );

        return pred( left, right );
    }
    template<typename FPT1, typename FPT2, typename ToleranceBaseType>
    predicate_result
    operator()( FPT1 left, FPT2 right, fraction_tolerance_t<ToleranceBaseType> tolerance, 
                floating_point_comparison_type fpc_type = FPC_STRONG ) const
    {
        // same as in a comment above
        typedef typename numeric::conversion_traits<FPT1,FPT2>::supertype FPT;
        BOOST_STATIC_ASSERT( !is_integral<FPT>::value );

        close_at_tolerance<FPT> pred( tolerance, fpc_type );

        return pred( left, right );
    }
};

namespace {
check_is_close_t const& check_is_close = unit_test::ut_detail::static_constant<check_is_close_t>::value;
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
    operator()( FPT fpv, FPT tolerance ) const
    {
        return tt_detail::fpt_abs( fpv ) < tt_detail::fpt_abs( tolerance );
    }
};

namespace {
check_is_small_t const& check_is_small = unit_test::ut_detail::static_constant<check_is_small_t>::value;
}

//____________________________________________________________________________//

} // namespace test_tools

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_FLOATING_POINT_COMAPARISON_HPP_071894GER
