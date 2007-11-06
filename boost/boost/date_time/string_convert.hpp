#ifndef _STRING_CONVERT_HPP___
#define _STRING_CONVERT_HPP___

/* Copyright (c) 2005 CrystalClear Software, Inc.
 * Subject to the Boost Software License, Version 1.0. (See accompanying
 * file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * Author: Jeff Garland, Bart Garst
 * $Date: 2005/04/23 05:39:52 $
 */

#include "boost/date_time/compiler_config.hpp"
#include <string>

namespace boost {
namespace date_time {

  //! Converts a string from one value_type to another
  /*! Converts a wstring to a string (or a string to wstring). If both template parameters 
   * are of same type, a copy of the input string is returned. */
  template<class InputT, class OutputT>
  inline
  std::basic_string<OutputT> convert_string_type(const std::basic_string<InputT>& inp_str)
  {
    typedef std::basic_string<InputT> input_type;
    typedef std::basic_string<OutputT> output_type;
    output_type result;
    result.insert(result.begin(), inp_str.begin(), inp_str.end());
    return result;
  }
  
}} // namespace boost::date_time

#endif // _STRING_CONVERT_HPP___
