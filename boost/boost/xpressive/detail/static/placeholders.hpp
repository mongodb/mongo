///////////////////////////////////////////////////////////////////////////////
// placeholders.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PLACEHOLDERS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PLACEHOLDERS_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <string>
#include <boost/shared_ptr.hpp>
#include <boost/xpressive/detail/core/quant_style.hpp>
#include <boost/xpressive/detail/core/regex_impl.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// literal_placeholder
//
template<typename Char, bool Not>
struct literal_placeholder
  : quant_style_fixed_width<1>
{
    typedef mpl::bool_<Not> not_type;
    Char ch_;

    literal_placeholder(Char ch)
      : ch_(ch)
    {
    }
};

///////////////////////////////////////////////////////////////////////////////
// string_placeholder
//
template<typename Char>
struct string_placeholder
  : quant_style_fixed_unknown_width
{
    std::basic_string<Char> str_;

    string_placeholder(std::basic_string<Char> const &str)
      : str_(str)
    {
    }
};

///////////////////////////////////////////////////////////////////////////////
// mark_placeholder
//
struct mark_placeholder
  : quant_style_fixed_unknown_width
{
    int mark_number_;

    mark_placeholder(int mark_number)
      : mark_number_(mark_number)
    {
    }
};

///////////////////////////////////////////////////////////////////////////////
// regex_placeholder
//
template<typename BidiIter, bool ByRef>
struct regex_placeholder
  : quant_style<quant_variable_width, unknown_width, mpl::false_>
{
    shared_ptr<regex_impl<BidiIter> > impl_;

    regex_placeholder(shared_ptr<regex_impl<BidiIter> > const &impl)
      : impl_(impl)
    {
    }
};

///////////////////////////////////////////////////////////////////////////////
// posix_charset_placeholder
//
struct posix_charset_placeholder
  : quant_style_fixed_width<1>
{
    char const *name_;
    bool not_;

    posix_charset_placeholder(char const *name)
      : name_(name)
      , not_(false)
    {
    }
};

///////////////////////////////////////////////////////////////////////////////
// assert_word_placeholder
//
template<typename Cond>
struct assert_word_placeholder
  : quant_style_assertion
{
};

///////////////////////////////////////////////////////////////////////////////
// range_placeholder
//
template<typename Char>
struct range_placeholder
  : quant_style_fixed_width<1>
{
    Char ch_min_;
    Char ch_max_;
    bool not_;

    range_placeholder(Char ch_min, Char ch_max)
      : ch_min_(ch_min)
      , ch_max_(ch_max)
      , not_(false)
    {
    }
};

///////////////////////////////////////////////////////////////////////////////
// assert_bol_placeholder
//
struct assert_bol_placeholder
  : quant_style_assertion
{
};

///////////////////////////////////////////////////////////////////////////////
// assert_eol_placeholder
//
struct assert_eol_placeholder
  : quant_style_assertion
{
};

///////////////////////////////////////////////////////////////////////////////
// logical_newline_placeholder
//
struct logical_newline_placeholder
  : quant_style_variable_width
{
};

///////////////////////////////////////////////////////////////////////////////
// self_placeholder
//
struct self_placeholder
  : quant_style<quant_variable_width, unknown_width, mpl::false_>
{
};

}}} // namespace boost::xpressive::detail

#endif
