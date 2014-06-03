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

#ifndef BOOST_INTRUSIVE_ANY_HOOK_HPP
#define BOOST_INTRUSIVE_ANY_HOOK_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/detail/any_node_and_algorithms.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/detail/generic_hook.hpp>
#include <boost/intrusive/pointer_traits.hpp>

namespace boost {
namespace intrusive {

/// @cond
template<class VoidPointer>
struct get_any_node_algo
{
   typedef any_algorithms<VoidPointer> type;
};
/// @endcond

//! Helper metafunction to define a \c \c any_base_hook that yields to the same
//! type when the same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1 = none, class O2 = none, class O3 = none>
#endif
struct make_any_base_hook
{
   /// @cond
   typedef typename pack_options
      < hook_defaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3
      #else
      Options...
      #endif
      >::type packed_options;
   
   typedef detail::generic_hook
   < get_any_node_algo<typename packed_options::void_pointer>
   , typename packed_options::tag
   , packed_options::link_mode
   , detail::AnyBaseHook
   > implementation_defined;
   /// @endcond
   typedef implementation_defined type;
};

//! Derive a class from this hook in order to store objects of that class
//! in an intrusive container.
//! 
//! The hook admits the following options: \c tag<>, \c void_pointer<> and
//! \c link_mode<>.
//!
//! \c tag<> defines a tag to identify the node. 
//! The same tag value can be used in different classes, but if a class is 
//! derived from more than one \c any_base_hook, then each \c any_base_hook needs its 
//! unique tag.
//!
//! \c link_mode<> will specify the linking mode of the hook (\c normal_link, \c safe_link).
//!
//! \c void_pointer<> is the pointer type that will be used internally in the hook
//! and the the container configured to use this hook.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1, class O2, class O3>
#endif
class any_base_hook
   :  public make_any_base_hook
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      <O1, O2, O3>
      #else
      <Options...>
      #endif
      ::type
{
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   public:
   //! <b>Effects</b>: If link_mode is or \c safe_link
   //!   initializes the node to an unlinked state.
   //! 
   //! <b>Throws</b>: Nothing. 
   any_base_hook();

   //! <b>Effects</b>: If link_mode is or \c safe_link
   //!   initializes the node to an unlinked state. The argument is ignored.
   //! 
   //! <b>Throws</b>: Nothing. 
   //! 
   //! <b>Rationale</b>: Providing a copy-constructor
   //!   makes classes using the hook STL-compliant without forcing the 
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   any_base_hook(const any_base_hook& );

   //! <b>Effects</b>: Empty function. The argument is ignored.
   //! 
   //! <b>Throws</b>: Nothing. 
   //! 
   //! <b>Rationale</b>: Providing an assignment operator 
   //!   makes classes using the hook STL-compliant without forcing the 
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   any_base_hook& operator=(const any_base_hook& );

   //! <b>Effects</b>: If link_mode is \c normal_link, the destructor does
   //!   nothing (ie. no code is generated). If link_mode is \c safe_link and the
   //!   object is stored in a container an assertion is raised.
   //! 
   //! <b>Throws</b>: Nothing. 
   ~any_base_hook();

   //! <b>Precondition</b>: link_mode must be \c safe_link.
   //!
   //! <b>Returns</b>: true, if the node belongs to a container, false
   //!   otherwise. This function can be used to test whether \c container::iterator_to 
   //!   will return a valid iterator. 
   //!
   //! <b>Complexity</b>: Constant 
   bool is_linked() const;
   #endif
};

//! Helper metafunction to define a \c \c any_member_hook that yields to the same
//! type when the same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1 = none, class O2 = none, class O3 = none>
#endif
struct make_any_member_hook
{
   /// @cond
   typedef typename pack_options
      < hook_defaults, 
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3
      #else
      Options...
      #endif
      >::type packed_options;

   typedef detail::generic_hook
   < get_any_node_algo<typename packed_options::void_pointer>
   , member_tag
   , packed_options::link_mode
   , detail::NoBaseHook
   > implementation_defined;
   /// @endcond
   typedef implementation_defined type;
};

//! Store this hook in a class to be inserted
//! in an intrusive container.
//! 
//! The hook admits the following options: \c void_pointer<> and
//! \c link_mode<>.
//! 
//! \c link_mode<> will specify the linking mode of the hook (\c normal_link or \c safe_link).
//!
//! \c void_pointer<> is the pointer type that will be used internally in the hook
//! and the the container configured to use this hook.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1, class O2, class O3>
#endif
class any_member_hook
   :  public make_any_member_hook
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      <O1, O2, O3>
      #else
      <Options...>
      #endif
      ::type
{
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   public:
   //! <b>Effects</b>: If link_mode is or \c safe_link
   //!   initializes the node to an unlinked state.
   //! 
   //! <b>Throws</b>: Nothing. 
   any_member_hook();

   //! <b>Effects</b>: If link_mode is or \c safe_link
   //!   initializes the node to an unlinked state. The argument is ignored.
   //! 
   //! <b>Throws</b>: Nothing. 
   //! 
   //! <b>Rationale</b>: Providing a copy-constructor
   //!   makes classes using the hook STL-compliant without forcing the 
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   any_member_hook(const any_member_hook& );

   //! <b>Effects</b>: Empty function. The argument is ignored.
   //! 
   //! <b>Throws</b>: Nothing. 
   //! 
   //! <b>Rationale</b>: Providing an assignment operator 
   //!   makes classes using the hook STL-compliant without forcing the 
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   any_member_hook& operator=(const any_member_hook& );

   //! <b>Effects</b>: If link_mode is \c normal_link, the destructor does
   //!   nothing (ie. no code is generated). If link_mode is \c safe_link and the
   //!   object is stored in a container an assertion is raised.
   //! 
   //! <b>Throws</b>: Nothing. 
   ~any_member_hook();

   //! <b>Precondition</b>: link_mode must be \c safe_link.
   //!
   //! <b>Returns</b>: true, if the node belongs to a container, false
   //!   otherwise. This function can be used to test whether \c container::iterator_to 
   //!   will return a valid iterator. 
   //!
   //! <b>Complexity</b>: Constant 
   bool is_linked() const;
   #endif
};

/// @cond

namespace detail{

template<class ValueTraits>
struct any_to_get_base_pointer_type
{
   typedef typename pointer_traits<typename ValueTraits::boost_intrusive_tags::node_traits::node_ptr>::template
      rebind_pointer<void>::type type;
};

template<class ValueTraits>
struct any_to_get_member_pointer_type
{
   typedef typename pointer_traits
      <typename ValueTraits::node_ptr>::template rebind_pointer<void>::type type;
};

//!This option setter specifies that the container
//!must use the specified base hook
template<class BaseHook, template <class> class NodeTraits>
struct any_to_some_hook
{
   typedef typename BaseHook::template pack<none>::value_traits old_value_traits;
   template<class Base>
   struct pack : public Base
   {
      struct value_traits : public old_value_traits
      {
         static const bool is_any_hook = true;
         typedef typename detail::eval_if_c
            < detail::internal_base_hook_bool_is_true<old_value_traits>::value
            , any_to_get_base_pointer_type<old_value_traits>
            , any_to_get_member_pointer_type<old_value_traits>
            >::type void_pointer;
         typedef NodeTraits<void_pointer> node_traits;
      };
   };
};

}  //namespace detail{

/// @endcond

//!This option setter specifies that
//!any hook should behave as an slist hook
template<class BaseHook>
struct any_to_slist_hook
/// @cond
   :  public detail::any_to_some_hook<BaseHook, any_slist_node_traits>
/// @endcond
{};

//!This option setter specifies that
//!any hook should behave as an list hook
template<class BaseHook>
struct any_to_list_hook
/// @cond
   :  public detail::any_to_some_hook<BaseHook, any_list_node_traits>
/// @endcond
{};

//!This option setter specifies that
//!any hook should behave as a set hook
template<class BaseHook>
struct any_to_set_hook
/// @cond
   :  public detail::any_to_some_hook<BaseHook, any_rbtree_node_traits>
/// @endcond
{};

//!This option setter specifies that
//!any hook should behave as an avl_set hook
template<class BaseHook>
struct any_to_avl_set_hook
/// @cond
   :  public detail::any_to_some_hook<BaseHook, any_avltree_node_traits>
/// @endcond
{};

//!This option setter specifies that any
//!hook should behave as a bs_set hook
template<class BaseHook>
struct any_to_bs_set_hook
/// @cond
   :  public detail::any_to_some_hook<BaseHook, any_tree_node_traits>
/// @endcond
{};

//!This option setter specifies that any hook
//!should behave as an unordered set hook
template<class BaseHook>
struct any_to_unordered_set_hook
/// @cond
   :  public detail::any_to_some_hook<BaseHook, any_unordered_node_traits>
/// @endcond
{};


} //namespace intrusive 
} //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_ANY_HOOK_HPP
