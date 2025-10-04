// Copyright (C) 2022 Andrzej Krzemienski.
//
// Use, modification, and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/optional for documentation.
//
// You are welcome to contact the author at:
//  akrzemi1@gmail.com

#ifndef BOOST_OPTIONAL_DETAIL_OPTIONAL_HASH_AJK_20MAY2022_HPP
#define BOOST_OPTIONAL_DETAIL_OPTIONAL_HASH_AJK_20MAY2022_HPP

#include <boost/optional/optional_fwd.hpp>
#include <boost/config.hpp>

#if !defined(BOOST_OPTIONAL_CONFIG_DO_NOT_SPECIALIZE_STD_HASH) && !defined(BOOST_NO_CXX11_HDR_FUNCTIONAL)

#include <functional>

namespace std
{
  template <typename T>
  struct hash<boost::optional<T> >
  {
    typedef std::size_t result_type;
    typedef boost::optional<T> argument_type;

    BOOST_CONSTEXPR result_type operator()(const argument_type& arg) const {
      return arg ? std::hash<T>()(*arg) : result_type();
    }
  };

  template <typename T>
  struct hash<boost::optional<T&> >
  {
    typedef std::size_t result_type;
    typedef boost::optional<T&> argument_type;

    BOOST_CONSTEXPR result_type operator()(const argument_type& arg) const {
      return arg ? std::hash<T>()(*arg) : result_type();
    }
  };
}

#endif // !defined(BOOST_OPTIONAL_CONFIG_DO_NOT_SPECIALIZE_STD_HASH) && !defined(BOOST_NO_CXX11_HDR_FUNCTIONAL)

#endif // header guard
