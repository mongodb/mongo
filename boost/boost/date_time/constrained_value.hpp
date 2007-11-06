#ifndef CONSTRAINED_VALUE_HPP___
#define CONSTRAINED_VALUE_HPP___

/* Copyright (c) 2002,2003 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * Author: Jeff Garland 
 * $Date: 2004/02/26 18:26:47 $
 */

#include <boost/config.hpp>

namespace boost {

//! Namespace containing constrained_value template and types
namespace CV {
  //! Represent a min or max violation type
  enum violation_enum {min_violation, max_violation};
  
  //! A template to specify a constrained basic value type
  /*! This template provides a quick way to generate
   *  an integer type with a constrained range.  The type
   *  provides for the ability to specify the min, max, and
   *  and error handling policy.
   *  
   *  <b>value policies</b>
   *  A class that provides the range limits via the min and
   *  max functions as well as a function on_error that 
   *  determines how errors are handled.  A common strategy
   *  would be to assert or throw and exception.  The on_error
   *  is passed both the current value and the new value that
   *  is in error.
   *
   */
  template<class value_policies>
  class constrained_value {
  public:
    typedef typename value_policies::value_type value_type;
    //    typedef except_type exception_type;
    constrained_value(value_type value) 
    {
      assign(value);
    };
    constrained_value& operator=(value_type v) 
    { 
      assign(v); 
      return *this;
    }   
    //! Return the max allowed value (traits method)
    static value_type max BOOST_PREVENT_MACRO_SUBSTITUTION () {return (value_policies::max)();};
    //! Return the min allowed value (traits method)
    static value_type min BOOST_PREVENT_MACRO_SUBSTITUTION () {return (value_policies::min)();};
    //! Coerce into the representation type
    operator value_type() const {return value_;};
  protected:
    value_type value_;
  private:
    void assign(value_type value)
    {
      //adding 1 below gets rid of a compiler warning which occurs when the 
      //min_value is 0 and the type is unsigned....
      if (value+1 < (min)()+1) {
        value_policies::on_error(value_, value, min_violation);
        return;
      }
      if (value > (max)()) {
        value_policies::on_error(value_, value, max_violation);
        return;
      }
      value_ = value;

    }
};

  //! Template to shortcut the constrained_value policy creation process
  template<typename rep_type, rep_type min_value, 
           rep_type max_value, class exception_type>
  class simple_exception_policy
  {
  public:
    typedef rep_type value_type;
    static rep_type min BOOST_PREVENT_MACRO_SUBSTITUTION () { return min_value; };
    static rep_type max BOOST_PREVENT_MACRO_SUBSTITUTION () { return max_value;};
    static void on_error(rep_type, rep_type, violation_enum)
    {
      throw exception_type();
    }
  };



} } //namespace CV




#endif
