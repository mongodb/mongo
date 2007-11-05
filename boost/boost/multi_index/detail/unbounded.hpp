/* Copyright 2003-2006 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_UNBOUNDED_HPP
#define BOOST_MULTI_INDEX_DETAIL_UNBOUNDED_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <boost/detail/workaround.hpp>

namespace boost{

namespace multi_index{

/* dummy type and variable for use in ordered_index::range() */

namespace detail{

struct unbounded_type{};

} /* namespace multi_index::detail */

namespace{

#if BOOST_WORKAROUND(BOOST_MSVC,<1300)
/* The default branch actually works for MSVC 6.0, but seems like
 * the const qualifier reduces the performance of ordered indices! This
 * behavior is hard to explain and probably a test artifact, but it
 * does not hurt to have the workaround anyway.
 */

static detail::unbounded_type  unbounded_obj=detail::unbounded_type();
static detail::unbounded_type& unbounded=unbounded_obj;
#else
const detail::unbounded_type unbounded=detail::unbounded_type();
#endif

} /* unnamed */

} /* namespace multi_index */

} /* namespace boost */

#endif
