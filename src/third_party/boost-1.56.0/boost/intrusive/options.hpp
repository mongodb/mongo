/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_OPTIONS_HPP
#define BOOST_INTRUSIVE_OPTIONS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/pack_options.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/static_assert.hpp>

namespace boost {
namespace intrusive {

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

//typedef void default_tag;
struct default_tag;
struct member_tag;

namespace detail{

struct default_hook_tag{};

#define BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER_DEFINITION(BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER) \
struct BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER : public default_hook_tag\
{\
   template <class T>\
   struct apply\
   {  typedef typename T::BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER type;  };\
}\

BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER_DEFINITION(default_list_hook);
BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER_DEFINITION(default_slist_hook);
BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER_DEFINITION(default_rbtree_hook);
BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER_DEFINITION(default_hashtable_hook);
BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER_DEFINITION(default_avltree_hook);
BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER_DEFINITION(default_bstree_hook);

#undef BOOST_INTRUSIVE_DEFAULT_HOOK_MARKER_DEFINITION

template <class T, class BaseHook>
struct concrete_hook_base_value_traits
{
   typedef typename BaseHook::hooktags tags;
   typedef bhtraits
      < T
      , typename tags::node_traits
      , tags::link_mode
      , typename tags::tag
      , tags::type> type;
};

template <class BaseHook>
struct concrete_hook_base_node_traits
{  typedef typename BaseHook::hooktags::node_traits type;  };

template <class T, class AnyToSomeHook_ProtoValueTraits>
struct any_hook_base_value_traits
{
   //AnyToSomeHook value_traits derive from a generic_hook
   //The generic_hook is configured with any_node_traits
   //and AnyToSomeHook::value_traits with the correct
   //node traits for the container, so use node_traits
   //from AnyToSomeHook_ProtoValueTraits and the rest of
   //elements from the hooktags member of the generic_hook
   typedef AnyToSomeHook_ProtoValueTraits proto_value_traits;
   typedef bhtraits
      < T
      , typename proto_value_traits::node_traits
      , proto_value_traits::hooktags::link_mode
      , typename proto_value_traits::hooktags::tag
      , proto_value_traits::hooktags::type
      > type;
};

template <class BaseHook>
struct any_hook_base_node_traits
{  typedef typename BaseHook::node_traits type; };

template<class T, class BaseHook>
struct get_base_value_traits
{
   typedef typename detail::eval_if_c
      < internal_any_hook_bool_is_true<BaseHook>::value
      , any_hook_base_value_traits<T, BaseHook>
      , concrete_hook_base_value_traits<T, BaseHook>
      >::type type;
};

template<class BaseHook>
struct get_base_node_traits
{
   typedef typename detail::eval_if_c
      < internal_any_hook_bool_is_true<BaseHook>::value
      , any_hook_base_node_traits<BaseHook>
      , concrete_hook_base_node_traits<BaseHook>
      >::type type;
};

template<class T, class MemberHook>
struct get_member_value_traits
{
   typedef typename MemberHook::member_value_traits type;
};

template<class MemberHook>
struct get_member_node_traits
{
   typedef typename MemberHook::member_value_traits::node_traits type;
};

template<class T, class SupposedValueTraits>
struct get_value_traits
{
   typedef typename detail::eval_if_c
      <detail::is_convertible<SupposedValueTraits*, detail::default_hook_tag*>::value
      ,detail::apply<SupposedValueTraits, T>
      ,detail::identity<SupposedValueTraits>
   >::type supposed_value_traits;

   //...if it's a default hook
   typedef typename detail::eval_if_c
      < internal_base_hook_bool_is_true<supposed_value_traits>::value
      //...get it's internal value traits using
      //the provided T value type.
      , get_base_value_traits<T, supposed_value_traits>
      //...else use its internal value traits tag
      //(member hooks and custom value traits are in this group)
      , detail::eval_if_c
         < internal_member_value_traits<supposed_value_traits>::value
         , get_member_value_traits<T, supposed_value_traits>
         , detail::identity<supposed_value_traits>
         >
      >::type type;
};

template<class ValueTraits>
struct get_explicit_node_traits
{
   typedef typename ValueTraits::node_traits type;
};

template<class SupposedValueTraits>
struct get_node_traits
{
   typedef SupposedValueTraits supposed_value_traits;
   //...if it's a base hook
   typedef typename detail::eval_if_c
      < internal_base_hook_bool_is_true<supposed_value_traits>::value
      //...get it's internal value traits using
      //the provided T value type.
      , get_base_node_traits<supposed_value_traits>
      //...else use its internal value traits tag
      //(member hooks and custom value traits are in this group)
      , detail::eval_if_c
         < internal_member_value_traits<supposed_value_traits>::value
         , get_member_node_traits<supposed_value_traits>
         , get_explicit_node_traits<supposed_value_traits>
         >
      >::type type;
};

}  //namespace detail{

#endif   //BOOST_INTRUSIVE_DOXYGEN_INVOKED

//!This option setter specifies if the intrusive
//!container stores its size as a member to
//!obtain constant-time size() member.
BOOST_INTRUSIVE_OPTION_CONSTANT(constant_time_size, bool, Enabled, constant_time_size)

//!This option setter specifies a container header holder type
BOOST_INTRUSIVE_OPTION_TYPE(header_holder_type, HeaderHolder, HeaderHolder, header_holder_type)

//!This option setter specifies the type that
//!the container will use to store its size.
BOOST_INTRUSIVE_OPTION_TYPE(size_type, SizeType, SizeType, size_type)

//!This option setter specifies the strict weak ordering
//!comparison functor for the value type
BOOST_INTRUSIVE_OPTION_TYPE(compare, Compare, Compare, compare)

//!This option setter for scapegoat containers specifies if
//!the intrusive scapegoat container should use a non-variable
//!alpha value that does not need floating-point operations.
//!
//!If activated, the fixed alpha value is 1/sqrt(2). This
//!option also saves some space in the container since
//!the alpha value and some additional data does not need
//!to be stored in the container.
//!
//!If the user only needs an alpha value near 1/sqrt(2), this
//!option also improves performance since avoids logarithm
//!and division operations when rebalancing the tree.
BOOST_INTRUSIVE_OPTION_CONSTANT(floating_point, bool, Enabled, floating_point)

//!This option setter specifies the equality
//!functor for the value type
BOOST_INTRUSIVE_OPTION_TYPE(equal, Equal, Equal, equal)

//!This option setter specifies the equality
//!functor for the value type
BOOST_INTRUSIVE_OPTION_TYPE(priority, Priority, Priority, priority)

//!This option setter specifies the hash
//!functor for the value type
BOOST_INTRUSIVE_OPTION_TYPE(hash, Hash, Hash, hash)

//!This option setter specifies the relationship between the type
//!to be managed by the container (the value type) and the node to be
//!used in the node algorithms. It also specifies the linking policy.
BOOST_INTRUSIVE_OPTION_TYPE(value_traits, ValueTraits, ValueTraits, proto_value_traits)

//#define BOOST_INTRUSIVE_COMMA ,
//#define BOOST_INTRUSIVE_LESS <
//#define BOOST_INTRUSIVE_MORE >
//BOOST_INTRUSIVE_OPTION_TYPE (member_hook, Parent BOOST_INTRUSIVE_COMMA class MemberHook BOOST_INTRUSIVE_COMMA MemberHook Parent::* PtrToMember , mhtraits BOOST_INTRUSIVE_LESS Parent BOOST_INTRUSIVE_COMMA MemberHook BOOST_INTRUSIVE_COMMA PtrToMember BOOST_INTRUSIVE_MORE , proto_value_traits)
//template< class Parent , class MemberHook , MemberHook Parent::* PtrToMember>
//struct member_hook {
//   template<class Base> struct pack : Base {
//      typedef mhtraits < Parent , MemberHook , PtrToMember > proto_value_traits;
//   };
//};
//
//#undef BOOST_INTRUSIVE_COMMA
//#undef BOOST_INTRUSIVE_LESS
//#undef BOOST_INTRUSIVE_MORE

//!This option setter specifies the member hook the
//!container must use.
template< typename Parent
        , typename MemberHook
        , MemberHook Parent::* PtrToMember>
struct member_hook
{
// @cond
//   typedef typename MemberHook::hooktags::node_traits node_traits;
//   typedef typename node_traits::node node_type;
//   typedef node_type Parent::* Ptr2MemNode;
//   typedef mhtraits
//      < Parent
//      , node_traits
//      //This cast is really ugly but necessary to reduce template bloat.
//      //Since we control the layout between the hook and the node, and there is
//      //always single inheritance, the offset of the node is exactly the offset of
//      //the hook. Since the node type is shared between all member hooks, this saves
//      //quite a lot of symbol stuff.
//      , (Ptr2MemNode)PtrToMember 
//      , MemberHook::hooktags::link_mode> member_value_traits;
   typedef mhtraits <Parent, MemberHook, PtrToMember> member_value_traits;
   template<class Base>
   struct pack : Base
   {
      typedef member_value_traits proto_value_traits;
   };
/// @endcond
};

//!This option setter specifies the function object that will
//!be used to convert between values to be inserted in a container
//!and the hook to be used for that purpose.
BOOST_INTRUSIVE_OPTION_TYPE(function_hook, Functor, fhtraits<Functor>, proto_value_traits)

//!This option setter specifies that the container
//!must use the specified base hook
BOOST_INTRUSIVE_OPTION_TYPE(base_hook, BaseHook, BaseHook, proto_value_traits)

//!This option setter specifies the type of
//!a void pointer. This will instruct the hook
//!to use this type of pointer instead of the
//!default one
BOOST_INTRUSIVE_OPTION_TYPE(void_pointer, VoidPointer, VoidPointer, void_pointer)

//!This option setter specifies the type of
//!the tag of a base hook. A type cannot have two
//!base hooks of the same type, so a tag can be used
//!to differentiate two base hooks with otherwise same type
BOOST_INTRUSIVE_OPTION_TYPE(tag, Tag, Tag, tag)

//!This option setter specifies the link mode
//!(normal_link, safe_link or auto_unlink)
BOOST_INTRUSIVE_OPTION_CONSTANT(link_mode, link_mode_type, LinkType, link_mode)

//!This option setter specifies if the hook
//!should be optimized for size instead of for speed.
BOOST_INTRUSIVE_OPTION_CONSTANT(optimize_size, bool, Enabled, optimize_size)

//!This option setter specifies if the slist container should
//!use a linear implementation instead of a circular one.
BOOST_INTRUSIVE_OPTION_CONSTANT(linear, bool, Enabled, linear)

//!If true, slist also stores a pointer to the last element of the singly linked list.
//!This allows O(1) swap and splice_after(iterator, slist &) for circular slists and makes
//!possible new functions like push_back(reference) and back().
BOOST_INTRUSIVE_OPTION_CONSTANT(cache_last, bool, Enabled, cache_last)

//!This option setter specifies the bucket traits
//!class for unordered associative containers. When this option is specified,
//!instead of using the default bucket traits, a user defined holder will be defined
BOOST_INTRUSIVE_OPTION_TYPE(bucket_traits, BucketTraits, BucketTraits, bucket_traits)

//!This option setter specifies if the unordered hook
//!should offer room to store the hash value.
//!Storing the hash in the hook will speed up rehashing
//!processes in applications where rehashing is frequent,
//!rehashing might throw or the value is heavy to hash.
BOOST_INTRUSIVE_OPTION_CONSTANT(store_hash, bool, Enabled, store_hash)

//!This option setter specifies if the unordered hook
//!should offer room to store another link to another node
//!with the same key.
//!Storing this link will speed up lookups and insertions on
//!unordered_multiset containers with a great number of elements
//!with the same key.
BOOST_INTRUSIVE_OPTION_CONSTANT(optimize_multikey, bool, Enabled, optimize_multikey)

//!This option setter specifies if the bucket array will be always power of two.
//!This allows using masks instead of the default modulo operation to determine
//!the bucket number from the hash value, leading to better performance.
//!In debug mode, if power of two buckets mode is activated, the bucket length
//!will be checked to through assertions to assure the bucket length is power of two.
BOOST_INTRUSIVE_OPTION_CONSTANT(power_2_buckets, bool, Enabled, power_2_buckets)

//!This option setter specifies if the container will cache a pointer to the first
//!non-empty bucket so that begin() is always constant-time.
//!This is specially helpful when we can have containers with a few elements
//!but with big bucket arrays (that is, hashtables with low load factors).
BOOST_INTRUSIVE_OPTION_CONSTANT(cache_begin, bool, Enabled, cache_begin)

//!This option setter specifies if the container will compare the hash value
//!before comparing objects. This option can't be specified if store_hash<>
//!is not true.
//!This is specially helpful when we have containers with a high load factor.
//!and the comparison function is much more expensive that comparing already
//!stored hash values.
BOOST_INTRUSIVE_OPTION_CONSTANT(compare_hash, bool, Enabled, compare_hash)

//!This option setter specifies if the hash container will use incremental
//!hashing. With incremental hashing the cost of hash table expansion is spread
//!out across each hash table insertion operation, as opposed to be incurred all at once.
//!Therefore linear hashing is well suited for interactive applications or real-time
//!appplications where the worst-case insertion time of non-incremental hash containers
//!(rehashing the whole bucket array) is not admisible.
BOOST_INTRUSIVE_OPTION_CONSTANT(incremental, bool, Enabled, incremental)

/// @cond

struct none
{
   template<class Base>
   struct pack : Base
   {};
};

struct hook_defaults
{
   typedef void* void_pointer;
   static const link_mode_type link_mode = safe_link;
   typedef default_tag tag;
   static const bool optimize_size = false;
   static const bool store_hash = false;
   static const bool linear = false;
   static const bool optimize_multikey = false;
};

/// @endcond

}  //namespace intrusive {
}  //namespace boost {

#include <boost/intrusive/detail/config_end.hpp>

#endif   //#ifndef BOOST_INTRUSIVE_OPTIONS_HPP
