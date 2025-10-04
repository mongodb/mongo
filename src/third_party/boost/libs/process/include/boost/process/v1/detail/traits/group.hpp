// Copyright (c) 2016 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


#ifndef BOOST_PROCESS_DETAIL_TRAITS_GROUP_HPP_
#define BOOST_PROCESS_DETAIL_TRAITS_GROUP_HPP_

#include <boost/process/v1/detail/config.hpp>
#include <boost/process/v1/detail/traits/decl.hpp>



namespace boost { namespace process { BOOST_PROCESS_V1_INLINE namespace v1 {

struct group;

namespace detail {


struct group_tag {};

template<>
struct make_initializer_t<group_tag>;


template<> struct initializer_tag_t<::boost::process::v1::group> { typedef group_tag type;};




}}}}



#endif /* BOOST_PROCESS_DETAIL_HANDLER_HPP_ */
