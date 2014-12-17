/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2013
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

/// @cond

enum base_hook_type
{  NoBaseHookId
,  ListBaseHookId
,  SlistBaseHookId
,  RbTreeBaseHookId
,  HashBaseHookId
,  AvlTreeBaseHookId
,  BsTreeBaseHookId
,  TreapTreeBaseHookId
,  AnyBaseHookId
};


template <class HookTags, unsigned int>
struct hook_tags_definer{};

template <class HookTags>
struct hook_tags_definer<HookTags, ListBaseHookId>
{  typedef HookTags default_list_hook;  };

template <class HookTags>
struct hook_tags_definer<HookTags, SlistBaseHookId>
{  typedef HookTags default_slist_hook;  };

template <class HookTags>
struct hook_tags_definer<HookTags, RbTreeBaseHookId>
{  typedef HookTags default_rbtree_hook;  };

template <class HookTags>
struct hook_tags_definer<HookTags, HashBaseHookId>
{  typedef HookTags default_hashtable_hook;  };

template <class HookTags>
struct hook_tags_definer<HookTags, AvlTreeBaseHookId>
{  typedef HookTags default_avltree_hook;  };

template <class HookTags>
struct hook_tags_definer<HookTags, BsTreeBaseHookId>
{  typedef HookTags default_bstree_hook;  };

template <class HookTags>
struct hook_tags_definer<HookTags, AnyBaseHookId>
{  typedef HookTags default_any_hook;  };

template
   < class NodeTraits
   , class Tag
   , link_mode_type LinkMode
   , base_hook_type BaseHookType
   >
struct hooktags_impl
{
   static const link_mode_type link_mode = LinkMode;
   typedef Tag tag;
   typedef NodeTraits node_traits;
   static const bool is_base_hook = !detail::is_same<Tag, member_tag>::value;
   static const bool safemode_or_autounlink = is_safe_autounlink<link_mode>::value;
   static const unsigned int type = BaseHookType;
};

/// @endcond

template
   < class GetNodeAlgorithms
   , class Tag
   , link_mode_type LinkMode
   , base_hook_type BaseHookType
   >
class generic_hook
   /// @cond
   //If the hook is a base hook, derive generic hook from node_holder
   //so that a unique base class is created to convert from the node
   //to the type. This mechanism will be used by bhtraits.
   //
   //If the hook is a member hook, generic hook will directly derive
   //from the hook.
   : public detail::if_c
      < detail::is_same<Tag, member_tag>::value
      , typename GetNodeAlgorithms::type::node
      , node_holder<typename GetNodeAlgorithms::type::node, Tag, BaseHookType>
      >::type
   //If this is the a default-tagged base hook derive from a class that 
   //will define an special internal typedef. Containers will be able to detect this
   //special typedef and obtain generic_hook's internal types in order to deduce
   //value_traits for this hook.
   , public hook_tags_definer
      < generic_hook<GetNodeAlgorithms, Tag, LinkMode, BaseHookType>
      , detail::is_same<Tag, default_tag>::value*BaseHookType>
   /// @endcond
{
   /// @cond
   typedef typename GetNodeAlgorithms::type           node_algorithms;
   typedef typename node_algorithms::node             node;
   typedef typename node_algorithms::node_ptr         node_ptr;
   typedef typename node_algorithms::const_node_ptr   const_node_ptr;

   public:

   typedef hooktags_impl
      < typename GetNodeAlgorithms::type::node_traits
      , Tag, LinkMode, BaseHookType>                  hooktags;

   node_ptr this_ptr()
   {  return pointer_traits<node_ptr>::pointer_to(static_cast<node&>(*this)); }

   const_node_ptr this_ptr() const
   {  return pointer_traits<const_node_ptr>::pointer_to(static_cast<const node&>(*this)); }

   public:
   /// @endcond

   generic_hook()
   {
      if(hooktags::safemode_or_autounlink){
         node_algorithms::init(this->this_ptr());
      }
   }

   generic_hook(const generic_hook& )
   {
      if(hooktags::safemode_or_autounlink){
         node_algorithms::init(this->this_ptr());
      }
   }

   generic_hook& operator=(const generic_hook& )
   {  return *this;  }

   ~generic_hook()
   {
      destructor_impl
         (*this, detail::link_dispatch<hooktags::link_mode>());
   }

   void swap_nodes(generic_hook &other)
   {
      node_algorithms::swap_nodes
         (this->this_ptr(), other.this_ptr());
   }

   bool is_linked() const
   {
      //is_linked() can be only used in safe-mode or auto-unlink
      BOOST_STATIC_ASSERT(( hooktags::safemode_or_autounlink ));
      return !node_algorithms::unique(this->this_ptr());
   }

   void unlink()
   {
      BOOST_STATIC_ASSERT(( (int)hooktags::link_mode == (int)auto_unlink ));
      node_algorithms::unlink(this->this_ptr());
      node_algorithms::init(this->this_ptr());
   }
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_GENERIC_HOOK_HPP
