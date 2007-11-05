///////////////////////////////////////////////////////////////////////////////
// quant_style.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_QUANT_STYLE_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_QUANT_STYLE_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/type_traits/is_base_and_derived.hpp>
#include <boost/mpl/assert.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/and.hpp>
#include <boost/mpl/not_equal_to.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>

#if defined(NDEBUG) & defined(BOOST_XPR_DEBUG_STACK)
# undef BOOST_XPR_DEBUG_STACK
#endif

#ifdef BOOST_XPR_DEBUG_STACK
# define BOOST_XPR_DEBUG_STACK_ASSERT BOOST_ASSERT
#else
# define BOOST_XPR_DEBUG_STACK_ASSERT(x) static_cast<void>(0)
#endif

namespace boost { namespace xpressive { namespace detail
{

//////////////////////////////////////////////////////////////////////////
// xpression_base
//
struct xpression_base
{
#ifdef BOOST_XPR_DEBUG_STACK
    virtual ~xpression_base()
    {
    }
#endif
};

///////////////////////////////////////////////////////////////////////////////
// is_xpr
//
template<typename T>
struct is_xpr
  : is_base_and_derived<xpression_base, T>
{
};

///////////////////////////////////////////////////////////////////////////////
// quant_enum
//
enum quant_enum
{
    quant_none,
    quant_auto,
    quant_fixed_width,
    quant_variable_width
};

///////////////////////////////////////////////////////////////////////////////
// quant_style
//
template<quant_enum QuantStyle, typename Width = unknown_width, typename Pure = mpl::true_>
struct quant_style
  : xpression_base
{
    typedef mpl::int_<QuantStyle> quant;   // Which quantification strategy to use?
    typedef Width width;                   // how many characters this matcher consumes
    typedef Pure pure;                     // whether this matcher has observable side-effects

    template<typename BidiIter>
    static std::size_t get_width(state_type<BidiIter> *)
    {
        return Width::value;
    }
};

///////////////////////////////////////////////////////////////////////////////
// quant_style_none
//  this sub-expression cannot be quantified
typedef quant_style<quant_none> quant_style_none;

///////////////////////////////////////////////////////////////////////////////
// quant_style_fixed_unknown_width
//  this sub-expression is fixed width for the purpose of quantification, but
//  the width cannot be determined at compile time. An example would be the
//  string_matcher or the mark_matcher.
typedef quant_style<quant_fixed_width> quant_style_fixed_unknown_width;

///////////////////////////////////////////////////////////////////////////////
// quant_style_variable_width
//  this sub-expression can match a variable number of characters
typedef quant_style<quant_variable_width> quant_style_variable_width;

///////////////////////////////////////////////////////////////////////////////
// quant_style_fixed_width
//  for when the sub-expression has a fixed width that is known at compile time
template<std::size_t Width>
struct quant_style_fixed_width
  : quant_style<quant_fixed_width, mpl::size_t<Width> >
{
};

///////////////////////////////////////////////////////////////////////////////
// quant_style_assertion
//  a zero-width assertion.
struct quant_style_assertion
  : quant_style<quant_none, mpl::size_t<0> >
{
};

///////////////////////////////////////////////////////////////////////////////
// quant_style_auto
//  automatically pick the quantification style based on width and purity
template<typename Width, typename Pure>
struct quant_style_auto
  : quant_style<quant_auto, Width, Pure>
{
};

///////////////////////////////////////////////////////////////////////////////
// quant_type
//
template<typename Matcher, typename QuantStyle = typename Matcher::quant>
struct quant_type
  : QuantStyle
{
};

///////////////////////////////////////////////////////////////////////////////
// when the quant_type is auto, determine the quant type from the width and purity
template<typename Matcher>
struct quant_type<Matcher, mpl::int_<quant_auto> >
  : mpl::if_
    <
        mpl::and_
        <
            mpl::not_equal_to<typename Matcher::width, unknown_width>
          , typename Matcher::pure
        >
      , mpl::int_<quant_fixed_width>
      , mpl::int_<quant_variable_width>
    >::type
{
};

}}} // namespace boost::xpressive::detail

#endif
