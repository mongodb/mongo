/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2009
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_GENERIC_HOOK_HPP
#define BOOST_INTRUSIVE_GENERIC_HOOK_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/static_assert.hpp>

namespace boost {
namespace intrusive {
namespace detail {

/// @cond

enum
{  NoBaseHook
,  ListBaseHook
,  SlistBaseHook
,  SetBaseHook
,  UsetBaseHook
,  SplaySetBaseHook
,  AvlSetBaseHook
,  BsSetBaseHook
,  AnyBaseHook
};

struct no_default_definer{};

template <class Hook, unsigned int>
struct default_definer;

template <class Hook>
struct default_definer<Hook, ListBaseHook>
{  typedef Hook default_list_hook;  };

template <class Hook>
struct default_definer<Hook, SlistBaseHook>
{  typedef Hook default_slist_hook;  };

template <class Hook>
struct default_definer<Hook, SetBaseHook>
{  typedef Hook default_set_hook;  };

template <class Hook>
struct default_definer<Hook, UsetBaseHook>
{  typedef Hook default_uset_hook;  };

template <class Hook>
struct default_definer<Hook, SplaySetBaseHook>
{  typedef Hook default_splay_set_hook;  };

template <class Hook>
struct default_definer<Hook, AvlSetBaseHook>
{  typedef Hook default_avl_set_hook;  };

template <class Hook>
struct default_definer<Hook, BsSetBaseHook>
{  typedef Hook default_bs_set_hook;  };

template <class Hook>
struct default_definer<Hook, AnyBaseHook>
{  typedef Hook default_any_hook;  };

template <class Hook, unsigned int BaseHookType>
struct make_default_definer
{
   typedef typename detail::if_c
      < BaseHookType != 0
      , default_definer<Hook, BaseHookType>
      , no_default_definer>::type type;
};

template
   < class GetNodeAlgorithms
   , class Tag
   , link_mode_type LinkMode
   , int HookType
   >
struct make_node_holder
{
   typedef typename detail::if_c
      <!detail::is_same<Tag, member_tag>::value
      , detail::node_holder
         < typename GetNodeAlgorithms::type::node
         , Tag
         , LinkMode
         , HookType>
      , typename GetNodeAlgorithms::type::node
      >::type type;
};

/// @endcond

template
   < class GetNodeAlgorithms
   , class Tag
   , link_mode_type LinkMode
   , int HookType
   >
class generic_hook
   /// @cond

   //If the hook is a base hook, derive generic hook from detail::node_holder
   //so that a unique base class is created to convert from the node
   //to the type. This mechanism will be used by base_hook_traits.
   //
   //If the hook is a member hook, generic hook will directly derive
   //from the hook.
   : public make_default_definer
      < generic_hook<GetNodeAlgorithms, Tag, LinkMode, HookType>
      , detail::is_same<Tag, default_tag>::value*HookType
      >::type
   , public make_node_holder<GetNodeAlgorithms, Tag, LinkMode, HookType>::type
   /// @endcond
{
   /// @cond
   typedef typename GetNodeAlgorithms::type           node_algorithms;
   typedef typename node_algorithms::node             node;
   typedef typename node_algorithms::node_ptr         node_ptr;
   typedef typename node_algorithms::const_node_ptr   const_node_ptr;

   public:
   struct boost_intrusive_tags
   {
      static const int hook_type = HookType;
      static const link_mode_type link_mode = LinkMode;
      typedef Tag                                           tag;
      typedef typename GetNodeAlgorithms::type::node_traits node_traits;
      static const bool is_base_hook = !detail::is_same<Tag, member_tag>::value;
      static const bool safemode_or_autounlink = 
         (int)link_mode == (int)auto_unlink || (int)link_mode == (int)safe_link;
   };

   node_ptr this_ptr()
   {  return pointer_traits<node_ptr>::pointer_to(static_cast<node&>(*this)); }

   const_node_ptr this_ptr() const
   {  return pointer_traits<const_node_ptr>::pointer_to(static_cast<const node&>(*this)); }

   public:
   /// @endcond

   generic_hook()
   {
      if(boost_intrusive_tags::safemode_or_autounlink){
         node_algorithms::init(this->this_ptr());
      }
   }

   generic_hook(const generic_hook& ) 
   {
      if(boost_intrusive_tags::safemode_or_autounlink){
         node_algorithms::init(this->this_ptr());
      }
   }

   generic_hook& operator=(const generic_hook& ) 
   {  return *this;  }

   ~generic_hook()
   {
      destructor_impl
         (*this, detail::link_dispatch<boost_intrusive_tags::link_mode>());
   }

   void swap_nodes(generic_hook &other) 
   {
      node_algorithms::swap_nodes
         (this->this_ptr(), other.this_ptr());
   }

   bool is_linked() const 
   {
      //is_linked() can be only used in safe-mode or auto-unlink
      BOOST_STATIC_ASSERT(( boost_intrusive_tags::safemode_or_autounlink ));
      return !node_algorithms::unique(this->this_ptr());
   }

   void unlink()
   {
      BOOST_STATIC_ASSERT(( (int)boost_intrusive_tags::link_mode == (int)auto_unlink ));
      node_algorithms::unlink(this->this_ptr());
      node_algorithms::init(this->this_ptr());
   }
};

} //namespace detail
} //namespace intrusive 
} //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_GENERIC_HOOK_HPP
