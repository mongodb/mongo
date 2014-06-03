/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2006-2009
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_VALUE_LINK_TYPE_HPP
#define BOOST_INTRUSIVE_VALUE_LINK_TYPE_HPP

namespace boost {
namespace intrusive {

//!This enumeration defines the type of value_traits that can be defined
//!for Boost.Intrusive containers
enum link_mode_type{
   //!If this linking policy is specified in a value_traits class
   //!as the link_mode, containers
   //!configured with such value_traits won't set the hooks
   //!of the erased values to a default state. Containers also won't
   //!check that the hooks of the new values are default initialized.
   normal_link,

   //!If this linking policy is specified in a value_traits class
   //!as the link_mode, containers
   //!configured with such value_traits will set the hooks
   //!of the erased values to a default state. Containers also will
   //!check that the hooks of the new values are default initialized.
   safe_link,

   //!Same as "safe_link" but the user type is an auto-unlink
   //!type, so the containers with constant-time size features won't be
   //!compatible with value_traits configured with this policy.
   //!Containers also know that the a value can be silently erased from
   //!the container without using any function provided by the containers.
   auto_unlink
};
} //namespace intrusive 
} //namespace boost 

#endif //BOOST_INTRUSIVE_VALUE_LINK_TYPE_HPP
