//
// is_contiguous_iterator.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IS_CONTIGUOUS_ITERATOR_HPP
#define BOOST_ASIO_IS_CONTIGUOUS_ITERATOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <iterator>
#include <boost/asio/detail/type_traits.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

/// The is_contiguous_iterator class is a traits class that may be used to
/// determine whether a type is a contiguous iterator.
template <typename T>
struct is_contiguous_iterator :
#if defined(BOOST_ASIO_HAS_STD_CONCEPTS) \
  || defined(GENERATING_DOCUMENTATION)
  integral_constant<bool, std::contiguous_iterator<T>>
#else // defined(BOOST_ASIO_HAS_STD_CONCEPTS)
      //   || defined(GENERATING_DOCUMENTATION)
  is_pointer<T>
#endif // defined(BOOST_ASIO_HAS_STD_CONCEPTS)
       //   || defined(GENERATING_DOCUMENTATION)
{
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IS_CONTIGUOUS_ITERATOR_HPP
