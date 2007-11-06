///////////////////////////////////////////////////////////////////////////////
/// \file xpressive_typeof.hpp
/// Type registrations so that xpressive can be used with the Boost.Typeof library.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_XPRESSIVE_TYPEOF_H
#define BOOST_XPRESSIVE_XPRESSIVE_TYPEOF_H

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/config.hpp>
#include <boost/typeof/typeof.hpp>
#ifndef BOOST_NO_STL_LOCALE
# include <boost/typeof/std/locale.hpp>
#endif
#include <boost/xpressive/proto/proto_typeof.hpp>
#include <boost/xpressive/xpressive_fwd.hpp>

#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()

///////////////////////////////////////////////////////////////////////////////
// Misc.
//
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::set_initializer)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::keeper_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::modifier_tag)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::generic_quant_tag, (unsigned int)(unsigned int))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::lookahead_tag, (bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::lookbehind_tag, (bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::basic_regex, (typename))

///////////////////////////////////////////////////////////////////////////////
// Placeholders
//
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::mark_placeholder)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::posix_charset_placeholder)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::assert_bol_placeholder)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::assert_eol_placeholder)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::logical_newline_placeholder)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::self_placeholder)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::string_placeholder, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::assert_word_placeholder, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::range_placeholder, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::regex_placeholder, (typename)(bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::literal_placeholder, (typename)(bool))

///////////////////////////////////////////////////////////////////////////////
// Matchers
//
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::epsilon_matcher)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::true_matcher)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::end_matcher)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::any_matcher)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::assert_bos_matcher)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::assert_eos_matcher)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::mark_begin_matcher)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::mark_end_matcher)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::repeat_begin_matcher)
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::alternate_end_matcher)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::assert_bol_matcher, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::assert_eol_matcher, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::literal_matcher, (typename)(bool)(bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::string_matcher, (typename)(bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::charset_matcher, (typename)(bool)(typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::logical_newline_matcher, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::mark_matcher, (typename)(bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::repeat_end_matcher, (bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::alternate_matcher, (typename)(typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::simple_repeat_matcher, (typename)(bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::regex_byref_matcher, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::regex_matcher, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::posix_charset_matcher, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::assert_word_matcher, (typename)(typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::range_matcher, (typename)(bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::keeper_matcher, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::lookahead_matcher, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::lookbehind_matcher, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::set_matcher, (typename)(int))

///////////////////////////////////////////////////////////////////////////////
// Modifiers
//
BOOST_TYPEOF_REGISTER_TYPE(boost::xpressive::detail::icase_modifier)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::detail::locale_modifier, (typename))

///////////////////////////////////////////////////////////////////////////////
// Traits
//
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::null_regex_traits, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::cpp_regex_traits, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::c_regex_traits, (typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::xpressive::regex_traits, (typename)(typename))

#endif
