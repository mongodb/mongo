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

#include <boost/config.hpp>

namespace boost {
namespace interprocess {

#if !defined(BOOST_NO_CXX11_SCOPED_ENUMS)

enum class cv_status
{
   no_timeout,
   timeout
};

#else //BOOST_NO_CXX11_SCOPED_ENUMS

// enum class cv_status emulation
struct cv_status
{
   typedef void is_boost_scoped_enum_tag;
   typedef int underlying_type;

   cv_status()  {}

   explicit  cv_status(underlying_type v)
      : v_(v)
   {}

   underlying_type get_underlying_value_() const  { return v_; }

   private:
   underlying_type v_;
   typedef cv_status self_type;
   public:
   enum enum_type
   {
      no_timeout,
      timeout
   };

   cv_status(enum_type v)
      : v_(v)
   {}

   enum_type get_native_value_() const
   { return enum_type(v_); }

   friend bool operator ==(self_type lhs, self_type rhs)
   { return enum_type(lhs.v_)==enum_type(rhs.v_); }

   friend bool operator ==(self_type lhs, enum_type rhs)
   { return enum_type(lhs.v_)==rhs; }

   friend bool operator ==(enum_type lhs, self_type rhs)
   { return lhs==enum_type(rhs.v_); }

   friend bool operator !=(self_type lhs, self_type rhs)
   { return enum_type(lhs.v_)!=enum_type(rhs.v_); }

   friend  bool operator !=(self_type lhs, enum_type rhs)
   { return enum_type(lhs.v_)!=rhs; }

   friend bool operator !=(enum_type lhs, self_type rhs)
   { return lhs!=enum_type(rhs.v_); }

   friend bool operator <(self_type lhs, self_type rhs)
   { return enum_type(lhs.v_)<enum_type(rhs.v_); }

   friend bool operator <(self_type lhs, enum_type rhs)
   { return enum_type(lhs.v_)<rhs; }

   friend bool operator <(enum_type lhs, self_type rhs)
   { return lhs<enum_type(rhs.v_); }

   friend bool operator <=(self_type lhs, self_type rhs)
   { return enum_type(lhs.v_)<=enum_type(rhs.v_); }

   friend bool operator <=(self_type lhs, enum_type rhs)
   { return enum_type(lhs.v_)<=rhs; }

   friend bool operator <=(enum_type lhs, self_type rhs)
   { return lhs<=enum_type(rhs.v_); }

   friend bool operator >(self_type lhs, self_type rhs)
   { return enum_type(lhs.v_)>enum_type(rhs.v_); }

   friend bool operator >(self_type lhs, enum_type rhs)
   { return enum_type(lhs.v_)>rhs; }

   friend bool operator >(enum_type lhs, self_type rhs)
   { return lhs>enum_type(rhs.v_); }

   friend bool operator >=(self_type lhs, self_type rhs)
   { return enum_type(lhs.v_)>=enum_type(rhs.v_); }

   friend bool operator >=(self_type lhs, enum_type rhs)
   { return enum_type(lhs.v_)>=rhs; }

   friend bool operator >=(enum_type lhs, self_type rhs)
   { return lhs>=enum_type(rhs.v_); }
};

#endif

} //namespace interprocess
} //namespace boost

#endif // header
