//  cv_status.hpp
//
// Copyright (C) 2011 Vicente J. Botet Escriba
// Copyright (C) 2021 Ion Gaztanaga
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_INTERPROCESS_CV_STATUS_HPP
#define BOOST_INTERPROCESS_CV_STATUS_HPP

#include <boost/core/scoped_enum.hpp>

namespace boost {
namespace interprocess {

  // enum class cv_status;
  BOOST_SCOPED_ENUM_DECLARE_BEGIN(cv_status)
  {
    no_timeout,
    timeout
  }
  BOOST_SCOPED_ENUM_DECLARE_END(cv_status)

} //namespace interprocess
} //namespace boost

#endif // header
