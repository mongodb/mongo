//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2015. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_TREE_HPP
#define BOOST_CONTAINER_TREE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
// container
#include <boost/container/allocator_traits.hpp>
#include <boost/container/container_fwd.hpp>
#include <boost/container/options.hpp>
#include <boost/container/node_handle.hpp>

// container/detail
#include <boost/container/detail/algorithm.hpp> //algo_equal(), algo_lexicographical_compare
#include <boost/container/detail/compare_functors.hpp>
#include <boost/container/detail/destroyers.hpp>
#include <boost/container/detail/iterator.hpp>
#include <boost/container/detail/iterators.hpp>
#include <boost/container/detail/node_alloc_holder.hpp>
#include <boost/container/detail/pair.hpp>
#include <boost/container/detail/type_traits.hpp>
// intrusive
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <boost/intrusive/avltree.hpp>
#include <boost/intrusive/splaytree.hpp>
#include <boost/intrusive/sgtree.hpp>
// intrusive/detail
#include <boost/intrusive/detail/minimal_pair_header.hpp>   //pair
#include <boost/intrusive/detail/tree_value_compare.hpp>    //tree_value_compare
// move
#include <boost/move/utility_core.hpp>
// move/detail
#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
#include <boost/move/detail/fwd_macros.hpp>
#endif
#include <boost/move/detail/move_helpers.hpp>



#include <boost/container/detail/std_fwd.hpp>

namespace boost {
namespace container {
namespace dtl {

using boost::intrusive::tree_value_compare;

template<class VoidPointer, boost::container::tree_type_enum tree_type_value, bool OptimizeSize>
struct intrusive_tree_hook;

template<class VoidPointer, bool OptimizeSize>
struct intrusive_tree_hook<VoidPointer, boost::container::red_black_tree, OptimizeSize>
{
   typedef typename dtl::bi::make_set_base_hook
      < dtl::bi::void_pointer<VoidPointer>
      , dtl::bi::link_mode<dtl::bi::normal_link>
      , dtl::bi::optimize_size<OptimizeSize>
      >::type  type;
};

template<class VoidPointer, bool OptimizeSize>
struct intrusive_tree_hook<VoidPointer, boost::container::avl_tree, OptimizeSize>
{
   typedef typename dtl::bi::make_avl_set_base_hook
      < dtl::bi::void_pointer<VoidPointer>
      , dtl::bi::link_mode<dtl::bi::normal_link>
      , dtl::bi::optimize_size<OptimizeSize>
      >::type  type;
};

template<class VoidPointer, bool OptimizeSize>
struct intrusive_tree_hook<VoidPointer, boost::container::scapegoat_tree, OptimizeSize>
{
   typedef typename dtl::bi::make_bs_set_base_hook
      < dtl::bi::void_pointer<VoidPointer>
      , dtl::bi::link_mode<dtl::bi::normal_link>
      >::type  type;
};

template<class VoidPointer, bool OptimizeSize>
struct intrusive_tree_hook<VoidPointer, boost::container::splay_tree, OptimizeSize>
{
   typedef typename dtl::bi::make_bs_set_base_hook
      < dtl::bi::void_pointer<VoidPointer>
      , dtl::bi::link_mode<dtl::bi::normal_link>
      >::type  type;
};

//This trait is used to type-pun std::pair because in C++03
//compilers std::pair is useless for C++11 features
template<class T>
struct tree_internal_data_type
{
   typedef T type;
};

template<class T1, class T2>
struct tree_internal_data_type< std::pair<T1, T2> >
{
   typedef pair<typename boost::move_detail::remove_const<T1>::type, T2> type;
};

template <class T, class VoidPointer, boost::container::tree_type_enum tree_type_value, bool OptimizeSize>
struct iiterator_node_value_type< base_node<T, intrusive_tree_hook<VoidPointer, tree_type_value, OptimizeSize>, true > >
{
  typedef T type;
};

template<class Node, class Icont>
class insert_equal_end_hint_functor
{
   Icont &icont_;

   public:
   inline insert_equal_end_hint_functor(Icont &icont)
      :  icont_(icont)
   {}

   inline void operator()(Node &n)
   {  this->icont_.insert_equal(this->icont_.cend(), n); }
};

template<class Node, class Icont>
class push_back_functor
{
   Icont &icont_;

   public:
   inline push_back_functor(Icont &icont)
      :  icont_(icont)
   {}

   inline void operator()(Node &n)
   {  this->icont_.push_back(n); }
};

}//namespace dtl {

namespace dtl {

template< class NodeType
        , class KeyOfNode
        , class KeyCompare
        , class HookType
        , boost::container::tree_type_enum tree_type_value>
struct intrusive_tree_dispatch;

template<class NodeType, class KeyOfNode, class KeyCompare, class HookType>
struct intrusive_tree_dispatch
   <NodeType, KeyOfNode, KeyCompare, HookType, boost::container::red_black_tree>
{
   typedef typename dtl::bi::make_rbtree
      <NodeType
      ,dtl::bi::key_of_value<KeyOfNode>
      ,dtl::bi::compare<KeyCompare>
      ,dtl::bi::base_hook<HookType>
      ,dtl::bi::constant_time_size<true>
      >::type  type;
};

template<class NodeType, class KeyOfNode, class KeyCompare, class HookType>
struct intrusive_tree_dispatch
   <NodeType, KeyOfNode, KeyCompare, HookType, boost::container::avl_tree>
{
   typedef typename dtl::bi::make_avltree
      <NodeType
      ,dtl::bi::key_of_value<KeyOfNode>
      ,dtl::bi::compare<KeyCompare>
      ,dtl::bi::base_hook<HookType>
      ,dtl::bi::constant_time_size<true>
      >::type  type;
};

template<class NodeType, class KeyOfNode, class KeyCompare, class HookType>
struct intrusive_tree_dispatch
   <NodeType, KeyOfNode, KeyCompare, HookType, boost::container::scapegoat_tree>
{
   typedef typename dtl::bi::make_sgtree
      <NodeType
      ,dtl::bi::key_of_value<KeyOfNode>
      ,dtl::bi::compare<KeyCompare>
      ,dtl::bi::base_hook<HookType>
      ,dtl::bi::floating_point<true>
      >::type  type;
};

template<class NodeType, class KeyOfNode, class KeyCompare, class HookType>
struct intrusive_tree_dispatch
   <NodeType, KeyOfNode, KeyCompare, HookType, boost::container::splay_tree>
{
   typedef typename dtl::bi::make_splaytree
      <NodeType
      ,dtl::bi::key_of_value<KeyOfNode>
      ,dtl::bi::compare<KeyCompare>
      ,dtl::bi::base_hook<HookType>
      ,dtl::bi::constant_time_size<true>
      >::type  type;
};

template < class Allocator
         , class KeyOfValue
         , class KeyCompare
         , boost::container::tree_type_enum tree_type_value
         , bool OptimizeSize>
struct intrusive_tree_type
{
   private:
   typedef typename boost::container::
      allocator_traits<Allocator>::value_type               value_type;
   typedef typename boost::container::
      allocator_traits<Allocator>::void_pointer             void_pointer;
   typedef base_node<value_type, intrusive_tree_hook
      <void_pointer, tree_type_value, OptimizeSize>, true > node_t;
   //Deducing the hook type from node_t (e.g. node_t::hook_type) would
   //provoke an early instantiation of node_t that could ruin recursive
   //tree definitions, so retype the complete type to avoid any problem.
   typedef typename intrusive_tree_hook
      <void_pointer, tree_type_value
      , OptimizeSize>::type                                 hook_type;

   typedef key_of_node
      <node_t, KeyOfValue>                                  key_of_node_t;

   public:
   typedef typename intrusive_tree_dispatch
      < node_t
      , key_of_node_t
      , KeyCompare
      , hook_type
      , tree_type_value>::type                     type;
};

//Trait to detect manually rebalanceable tree types
template<boost::container::tree_type_enum tree_type_value>
struct is_manually_balanceable
{  BOOST_STATIC_CONSTEXPR bool value = true;  };

template<>  struct is_manually_balanceable<red_black_tree>
{  BOOST_STATIC_CONSTEXPR bool value = false; };

template<>  struct is_manually_balanceable<avl_tree>
{  BOOST_STATIC_CONSTEXPR bool value = false; };

//Proxy traits to implement different operations depending on the
//is_manually_balanceable<>::value
template< boost::container::tree_type_enum tree_type_value
        , bool IsManuallyRebalanceable = is_manually_balanceable<tree_type_value>::value>
struct intrusive_tree_proxy
{
   template<class Icont>
   inline static void rebalance(Icont &)   {}
};

template<boost::container::tree_type_enum tree_type_value>
struct intrusive_tree_proxy<tree_type_value, true>
{
   template<class Icont>
   inline static void rebalance(Icont &c)
   {  c.rebalance(); }
};

}  //namespace dtl {

namespace dtl {

//This functor will be used with Intrusive clone functions to obtain
//already allocated nodes from a intrusive container instead of
//allocating new ones. When the intrusive container runs out of nodes
//the node holder is used instead.
template<class AllocHolder, bool DoMove>
class RecyclingCloner
{
   typedef typename AllocHolder::intrusive_container  intrusive_container;
   typedef typename AllocHolder::Node                 node_t;
   typedef typename AllocHolder::NodePtr              node_ptr_type;

   public:
   RecyclingCloner(AllocHolder &holder, intrusive_container &itree)
      :  m_holder(holder), m_icont(itree)
   {}

   inline static void do_assign(node_ptr_type p, node_t &other, bool_<true>)
   {  p->do_move_assign(other.get_real_data());   }

   inline static void do_assign(node_ptr_type p, const node_t &other, bool_<false>)
   {  p->do_assign(other.get_real_data());   }

   node_ptr_type operator()
      (typename dtl::if_c<DoMove, node_t &, const node_t&>::type other) const
   {
      if(node_ptr_type p = m_icont.unlink_leftmost_without_rebalance()){
         //First recycle a node (this can't throw)
         BOOST_CONTAINER_TRY{
            //This can throw
            this->do_assign(p, other, bool_<DoMove>());
            return p;
         }
         BOOST_CONTAINER_CATCH(...){
            //If there is an exception destroy the whole source
            m_holder.destroy_node(p);
            while((p = m_icont.unlink_leftmost_without_rebalance())){
               m_holder.destroy_node(p);
            }
            BOOST_CONTAINER_RETHROW
         }
         BOOST_CONTAINER_CATCH_END
      }
      else{
         return m_holder.create_node(boost::move(other.get_real_data()));
      }
   }

   AllocHolder &m_holder;
   intrusive_container &m_icont;
};

template<class Options>
struct get_tree_opt
{
   typedef Options type;
};

template<>
struct get_tree_opt<void>
{
   typedef tree_assoc_defaults type;
};

template<class, class KeyOfValue>
struct tree_key_of_value
{
   typedef KeyOfValue type;
};

template<class T>
struct tree_key_of_value<T, void>
{
   typedef dtl::identity<T> type;
};

template<class T1, class T2>
struct tree_key_of_value<std::pair<T1, T2>, int>
{
   typedef dtl::select1st<T1> type;
};

template<class T1, class T2>
struct tree_key_of_value<boost::container::dtl::pair<T1, T2>, int>
{
   typedef dtl::select1st<T1> type;
};


template <class T, class KeyOfValue, class Compare, class Allocator, class Options>
struct make_intrusive_tree_type
   : dtl::intrusive_tree_type
         < typename real_allocator<T, Allocator>::type
         , typename tree_key_of_value<T, KeyOfValue>::type
         , Compare
         , get_tree_opt<Options>::type::tree_type
         , get_tree_opt<Options>::type::optimize_size
         >
{};


template <class T, class KeyOfValue, class Compare, class Allocator, class Options>
class tree
   : public dtl::node_alloc_holder
      < typename real_allocator<T, Allocator>::type
      , typename make_intrusive_tree_type<T, KeyOfValue, Compare, Allocator, Options>::type
      >
{
   typedef tree < T, KeyOfValue
                , Compare, Allocator, Options>              ThisType;
   public:
   typedef typename real_allocator<T, Allocator>::type      allocator_type;

   private:
   typedef allocator_traits<allocator_type>                 allocator_traits_t;
   typedef typename tree_key_of_value<T, KeyOfValue>::type  key_of_value_t;
   typedef tree_value_compare
      < typename allocator_traits_t::pointer
      , Compare
      , key_of_value_t>                                     ValComp;
   typedef typename get_tree_opt<Options>::type             options_type;
   typedef typename make_intrusive_tree_type
      <T, KeyOfValue, Compare, Allocator, Options>::type    Icont;
   typedef dtl::node_alloc_holder
      <allocator_type, Icont>                               AllocHolder;
   typedef typename AllocHolder::NodePtr                    NodePtr;

   typedef typename AllocHolder::NodeAlloc                  NodeAlloc;
   typedef boost::container::
      allocator_traits<NodeAlloc>                           allocator_traits_type;
   typedef typename AllocHolder::ValAlloc                   ValAlloc;
   typedef typename AllocHolder::Node                       Node;
   typedef typename Icont::iterator                         iiterator;
   typedef typename Icont::const_iterator                   iconst_iterator;
   typedef dtl::allocator_node_destroyer<NodeAlloc> Destroyer;
   typedef typename AllocHolder::alloc_version              alloc_version;
   typedef intrusive_tree_proxy<options_type::tree_type>    intrusive_tree_proxy_t;

   BOOST_COPYABLE_AND_MOVABLE(tree)

   public:

   typedef typename dtl::remove_const
      <typename key_of_value_t::type>::type                 key_type;
   typedef T                                                value_type;
   typedef Compare                                          key_compare;
   typedef ValComp                                          value_compare;
   typedef typename boost::container::
      allocator_traits<allocator_type>::pointer             pointer;
   typedef typename boost::container::
      allocator_traits<allocator_type>::const_pointer       const_pointer;
   typedef typename boost::container::
      allocator_traits<allocator_type>::reference           reference;
   typedef typename boost::container::
      allocator_traits<allocator_type>::const_reference     const_reference;
   typedef typename boost::container::
      allocator_traits<allocator_type>::size_type           size_type;
   typedef typename boost::container::
      allocator_traits<allocator_type>::difference_type     difference_type;
   typedef dtl::iterator_from_iiterator
      <iiterator, false>                                    iterator;
   typedef dtl::iterator_from_iiterator
      <iiterator, true >                                    const_iterator;
   typedef boost::container::reverse_iterator
      <iterator>                                            reverse_iterator;
   typedef boost::container::reverse_iterator
      <const_iterator>                                      const_reverse_iterator;
   typedef node_handle
      < NodeAlloc, void>                                    node_type;
   typedef insert_return_type_base
      <iterator, node_type>                                 insert_return_type;

   typedef NodeAlloc                                        stored_allocator_type;

   private:

   //`allocator_type::value_type` must match container's `value type`. If this
   //assertion fails, please review your allocator definition. 
   BOOST_CONTAINER_STATIC_ASSERT((dtl::is_same<value_type, typename allocator_traits<allocator_type>::value_type>::value));

   typedef key_node_pred<key_compare, key_of_value_t, Node>  KeyNodeCompare;

   public:

   inline tree()
      : AllocHolder()
   {}

   inline explicit tree(const key_compare& comp)
      : AllocHolder(ValComp(comp))
   {}

   inline explicit tree(const key_compare& comp, const allocator_type& a)
      : AllocHolder(ValComp(comp), a)
   {}

   inline explicit tree(const allocator_type& a)
      : AllocHolder(a)
   {}

   template <class InputIterator>
   tree(bool unique_insertion, InputIterator first, InputIterator last)
      : AllocHolder(value_compare(key_compare()))
   {
      this->tree_construct(unique_insertion, first, last);
      //AllocHolder clears in case of exception
   }

   template <class InputIterator>
   tree(bool unique_insertion, InputIterator first, InputIterator last, const key_compare& comp)
      : AllocHolder(value_compare(comp))
   {
      this->tree_construct(unique_insertion, first, last);
      //AllocHolder clears in case of exception
   }

   template <class InputIterator>
   tree(bool unique_insertion, InputIterator first, InputIterator last, const key_compare& comp, const allocator_type& a)
      : AllocHolder(value_compare(comp), a)
   {
      this->tree_construct(unique_insertion, first, last);
      //AllocHolder clears in case of exception
   }

   //construct with ordered range
   template <class InputIterator>
   tree( ordered_range_t, InputIterator first, InputIterator last)
      : AllocHolder(value_compare(key_compare()))
   {
      this->tree_construct(ordered_range_t(), first, last);
   }

   template <class InputIterator>
   tree( ordered_range_t, InputIterator first, InputIterator last, const key_compare& comp)
      : AllocHolder(value_compare(comp))
   {
      this->tree_construct(ordered_range_t(), first, last);
   }

   template <class InputIterator>
   tree( ordered_range_t, InputIterator first, InputIterator last
         , const key_compare& comp, const allocator_type& a)
      : AllocHolder(value_compare(comp), a)
   {
      this->tree_construct(ordered_range_t(), first, last);
   }

   private:

   template <class InputIterator>
   void tree_construct(bool unique_insertion, InputIterator first, InputIterator last)
   {
      //Use cend() as hint to achieve linear time for
      //ordered ranges as required by the standard
      //for the constructor
      if(unique_insertion){
         const const_iterator end_it(this->cend());
         for ( ; first != last; ++first){
            this->insert_unique_hint_convertible(end_it, *first);
         }
      }
      else{
         this->tree_construct_non_unique(first, last);
      }
   }

   template <class InputIterator>
   void tree_construct_non_unique(InputIterator first, InputIterator last
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename dtl::enable_if_or
         < void
         , dtl::is_same<alloc_version, version_1>
         , dtl::is_input_iterator<InputIterator>
         >::type * = 0
      #endif
         )
   {
      //Use cend() as hint to achieve linear time for
      //ordered ranges as required by the standard
      //for the constructor
      const const_iterator end_it(this->cend());
      for ( ; first != last; ++first){
         this->insert_equal_hint_convertible(end_it, *first);
      }
   }

   template <class InputIterator>
   void tree_construct_non_unique(InputIterator first, InputIterator last
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename dtl::disable_if_or
         < void
         , dtl::is_same<alloc_version, version_1>
         , dtl::is_input_iterator<InputIterator>
         >::type * = 0
      #endif
         )
   {
      //Optimized allocation and construction
      this->allocate_many_and_construct
         ( first, boost::container::iterator_udistance(first, last)
         , insert_equal_end_hint_functor<Node, Icont>(this->icont()));
   }

   template <class InputIterator>
   void tree_construct( ordered_range_t, InputIterator first, InputIterator last
         #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename dtl::disable_if_or
         < void
         , dtl::is_same<alloc_version, version_1>
         , dtl::is_input_iterator<InputIterator>
         >::type * = 0
         #endif
         )
   {
      //Optimized allocation and construction
      this->allocate_many_and_construct
         ( first, boost::container::iterator_udistance(first, last)
         , dtl::push_back_functor<Node, Icont>(this->icont()));
      //AllocHolder clears in case of exception
   }

   template <class InputIterator>
   void tree_construct( ordered_range_t, InputIterator first, InputIterator last
         #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename dtl::enable_if_or
         < void
         , dtl::is_same<alloc_version, version_1>
         , dtl::is_input_iterator<InputIterator>
         >::type * = 0
         #endif
         )
   {
      for ( ; first != last; ++first){
         this->push_back_impl(*first);
      }
   }

   public:

   inline tree(const tree& x)
      :  AllocHolder(x, x.value_comp())
   {
      this->icont().clone_from
         (x.icont(), typename AllocHolder::cloner(*this), Destroyer(this->node_alloc()));
   }

   inline tree(BOOST_RV_REF(tree) x)
      BOOST_NOEXCEPT_IF(boost::container::dtl::is_nothrow_move_constructible<Compare>::value)
      :  AllocHolder(BOOST_MOVE_BASE(AllocHolder, x), x.value_comp())
   {}

   inline tree(const tree& x, const allocator_type &a)
      :  AllocHolder(x.value_comp(), a)
   {
      this->icont().clone_from
         (x.icont(), typename AllocHolder::cloner(*this), Destroyer(this->node_alloc()));
      //AllocHolder clears in case of exception
   }

   tree(BOOST_RV_REF(tree) x, const allocator_type &a)
      :  AllocHolder(x.value_comp(), a)
   {
      if(this->node_alloc() == x.node_alloc()){
         this->icont().swap(x.icont());
      }
      else{
         this->icont().clone_from
            (boost::move(x.icont()), typename AllocHolder::move_cloner(*this), Destroyer(this->node_alloc()));
      }
      //AllocHolder clears in case of exception
   }

   inline ~tree()
   {} //AllocHolder clears the tree

   tree& operator=(BOOST_COPY_ASSIGN_REF(tree) x)
   {
      if (BOOST_LIKELY(this != &x)) {
         NodeAlloc &this_alloc     = this->get_stored_allocator();
         const NodeAlloc &x_alloc  = x.get_stored_allocator();
         dtl::bool_<allocator_traits<NodeAlloc>::
            propagate_on_container_copy_assignment::value> flag;
         if(flag && this_alloc != x_alloc){
            this->clear();
         }
         this->AllocHolder::copy_assign_alloc(x);
         //Transfer all the nodes to a temporary tree
         //If anything goes wrong, all the nodes will be destroyed
         //automatically
         Icont other_tree(::boost::move(this->icont()));

         //Now recreate the source tree reusing nodes stored by other_tree
         this->icont().clone_from
            (x.icont()
            , RecyclingCloner<AllocHolder, false>(*this, other_tree)
            , Destroyer(this->node_alloc()));

         //If there are remaining nodes, destroy them
         NodePtr p;
         while((p = other_tree.unlink_leftmost_without_rebalance())){
            AllocHolder::destroy_node(p);
         }
      }
      return *this;
   }

   tree& operator=(BOOST_RV_REF(tree) x)
      BOOST_NOEXCEPT_IF( (allocator_traits_type::propagate_on_container_move_assignment::value ||
                          allocator_traits_type::is_always_equal::value) &&
                           boost::container::dtl::is_nothrow_move_assignable<Compare>::value)
   {
      if (BOOST_LIKELY(this != &x)) {
         //We know resources can be transferred at comiple time if both allocators are
         //always equal or the allocator is going to be propagated
         const bool can_steal_resources_alloc
            =  allocator_traits_type::propagate_on_container_move_assignment::value
            || allocator_traits_type::is_always_equal::value;
         dtl::bool_<can_steal_resources_alloc> flag;
         this->priv_move_assign(boost::move(x), flag);
      }
      return *this;
   }

   public:
   // accessors:
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      value_compare value_comp() const
   {  return value_compare(this->key_comp()); }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      key_compare key_comp() const
   {  return this->icont().key_comp(); }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      allocator_type get_allocator() const
   {  return allocator_type(this->node_alloc()); }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const stored_allocator_type &get_stored_allocator() const
   {  return this->node_alloc(); }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      stored_allocator_type &get_stored_allocator()
   {  return this->node_alloc(); }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      iterator begin()
   { return iterator(this->icont().begin()); }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_iterator begin() const
   {  return this->cbegin();  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      iterator end()
   {  return iterator(this->icont().end());  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_iterator end() const
   {  return this->cend();  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      reverse_iterator rbegin()
   {  return reverse_iterator(end());  }

   
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_reverse_iterator rbegin() const
   {  return this->crbegin();  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      reverse_iterator rend()
   {  return reverse_iterator(begin());   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_reverse_iterator rend() const
   {  return this->crend();   }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_iterator cbegin() const
   { return const_iterator(this->non_const_icont().begin()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_iterator cend() const
   { return const_iterator(this->non_const_icont().end()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_reverse_iterator crbegin() const
   { return const_reverse_iterator(cend()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_reverse_iterator crend() const
   { return const_reverse_iterator(cbegin()); }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      bool empty() const
   {  return !this->size();  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      size_type size() const
   {  return this->icont().size();   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      size_type max_size() const
   {  return AllocHolder::max_size();  }

   inline void swap(ThisType& x)
      BOOST_NOEXCEPT_IF(  allocator_traits_type::is_always_equal::value
                                 && boost::container::dtl::is_nothrow_swappable<Compare>::value )
   {  AllocHolder::swap(x);   }

   public:

   typedef typename Icont::insert_commit_data insert_commit_data;

   // insert/erase
   std::pair<iterator,bool> insert_unique_check
      (const key_type& key, insert_commit_data &data)
   {
      std::pair<iiterator, bool> ret =
         this->icont().insert_unique_check(key, data);
      return std::pair<iterator, bool>(iterator(ret.first), ret.second);
   }

   std::pair<iterator,bool> insert_unique_check
      (const_iterator hint, const key_type& key, insert_commit_data &data)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      std::pair<iiterator, bool> ret =
         this->icont().insert_unique_check(hint.get(), key, data);
      return std::pair<iterator, bool>(iterator(ret.first), ret.second);
   }

   template<class MovableConvertible>
   iterator insert_unique_commit
      (BOOST_FWD_REF(MovableConvertible) v, insert_commit_data &data)
   {
      NodePtr tmp = AllocHolder::create_node(boost::forward<MovableConvertible>(v));
      scoped_node_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_unique_commit(*tmp, data));
      destroy_deallocator.release();
      return ret;
   }

   template<class MovableConvertible>
   std::pair<iterator,bool> insert_unique_convertible(BOOST_FWD_REF(MovableConvertible) v)
   {
      insert_commit_data data;
      std::pair<iterator,bool> ret =
         this->insert_unique_check(key_of_value_t()(v), data);
      if(ret.second){
         ret.first = this->insert_unique_commit(boost::forward<MovableConvertible>(v), data);
      }
      return ret;
   }

   template<class MovableConvertible>
   iterator insert_unique_hint_convertible(const_iterator hint, BOOST_FWD_REF(MovableConvertible) v)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      insert_commit_data data;
      std::pair<iterator,bool> ret =
         this->insert_unique_check(hint, key_of_value_t()(v), data);
      if(!ret.second)
         return ret.first;
      return this->insert_unique_commit(boost::forward<MovableConvertible>(v), data);
   }


   private:
   void priv_move_assign(BOOST_RV_REF(tree) x, dtl::bool_<true> /*steal_resources*/)
   {
      //Destroy objects but retain memory in case x reuses it in the future
      this->clear();
      //Move allocator if needed
      this->AllocHolder::move_assign_alloc(x);
      //Obtain resources
      this->icont() = boost::move(x.icont());
   }

   void priv_move_assign(BOOST_RV_REF(tree) x, dtl::bool_<false> /*steal_resources*/)
   {
      //We can't guarantee a compile-time equal allocator or propagation so fallback to runtime
      //Resources can be transferred if both allocators are equal
      if (this->node_alloc() == x.node_alloc()) {
         this->priv_move_assign(boost::move(x), dtl::true_());
      }
      else {
         //Transfer all the nodes to a temporary tree
         //If anything goes wrong, all the nodes will be destroyed
         //automatically
         Icont other_tree(::boost::move(this->icont()));

         //Now recreate the source tree reusing nodes stored by other_tree
         this->icont().clone_from
         (::boost::move(x.icont())
            , RecyclingCloner<AllocHolder, true>(*this, other_tree)
            , Destroyer(this->node_alloc()));

         //If there are remaining nodes, destroy them
         NodePtr p;
         while ((p = other_tree.unlink_leftmost_without_rebalance())) {
            AllocHolder::destroy_node(p);
         }
      }
   }

   template<class KeyConvertible, class M>
   iiterator priv_insert_or_assign_commit
      (BOOST_FWD_REF(KeyConvertible) key, BOOST_FWD_REF(M) obj, insert_commit_data &data)
   {
      NodePtr tmp = AllocHolder::create_node(boost::forward<KeyConvertible>(key), boost::forward<M>(obj));
      scoped_node_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iiterator ret(this->icont().insert_unique_commit(*tmp, data));
      destroy_deallocator.release();
      return ret;
   }

   bool priv_is_linked(const_iterator const position) const
   {
      iiterator const cur(position.get());
      return   cur == this->icont().end() ||
               cur == this->icont().root() ||
               iiterator(cur).go_parent().go_left()  == cur ||
               iiterator(cur).go_parent().go_right() == cur;
   }

   template<class MovableConvertible>
   void push_back_impl(BOOST_FWD_REF(MovableConvertible) v)
   {
      NodePtr tmp(AllocHolder::create_node(boost::forward<MovableConvertible>(v)));
      //push_back has no-throw guarantee so avoid any deallocator/destroyer
      this->icont().push_back(*tmp);
   }

   std::pair<iterator, bool> emplace_unique_node(NodePtr p)
   {
      value_type &v = p->get_data();
      insert_commit_data data;
      scoped_node_destroy_deallocator<NodeAlloc> destroy_deallocator(p, this->node_alloc());
      std::pair<iterator,bool> ret =
         this->insert_unique_check(key_of_value_t()(v), data);
      if(!ret.second){
         return ret;
      }
      //No throw insertion part, release rollback
      destroy_deallocator.release();
      return std::pair<iterator,bool>
         ( iterator(this->icont().insert_unique_commit(*p, data))
         , true );
   }

   iterator emplace_hint_unique_node(const_iterator hint, NodePtr p)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      value_type &v = p->get_data();
      insert_commit_data data;
      std::pair<iterator,bool> ret =
         this->insert_unique_check(hint, key_of_value_t()(v), data);
      if(!ret.second){
         //Destroy unneeded node
         Destroyer(this->node_alloc())(p);
         return ret.first;
      }
      return iterator(this->icont().insert_unique_commit(*p, data));
   }

   public:

   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   template <class... Args>
   inline std::pair<iterator, bool> emplace_unique(BOOST_FWD_REF(Args)... args)
   {  return this->emplace_unique_node(AllocHolder::create_node(boost::forward<Args>(args)...));   }

   template <class... Args>
   inline iterator emplace_hint_unique(const_iterator hint, BOOST_FWD_REF(Args)... args)
   {  return this->emplace_hint_unique_node(hint, AllocHolder::create_node(boost::forward<Args>(args)...));   }

   template <class... Args>
   iterator emplace_equal(BOOST_FWD_REF(Args)... args)
   {
      NodePtr tmp(AllocHolder::create_node(boost::forward<Args>(args)...));
      scoped_node_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(this->icont().end(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   template <class... Args>
   iterator emplace_hint_equal(const_iterator hint, BOOST_FWD_REF(Args)... args)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      NodePtr tmp(AllocHolder::create_node(boost::forward<Args>(args)...));
      scoped_node_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(hint.get(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   template <class KeyType, class... Args>
   inline std::pair<iterator, bool> try_emplace
      (const_iterator hint, BOOST_FWD_REF(KeyType) key, BOOST_FWD_REF(Args)... args)
   {
      insert_commit_data data;
      const key_type & k = key;  //Support emulated rvalue references
      std::pair<iiterator, bool> ret =
         hint == const_iterator() ? this->icont().insert_unique_check(            k, data)
                                  : this->icont().insert_unique_check(hint.get(), k, data);
      if(ret.second){
         ret.first = this->icont().insert_unique_commit
            (*AllocHolder::create_node(try_emplace_t(), boost::forward<KeyType>(key), boost::forward<Args>(args)...), data);
      }
      return std::pair<iterator, bool>(iterator(ret.first), ret.second);
   }

   #else // !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   #define BOOST_CONTAINER_TREE_EMPLACE_CODE(N) \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   std::pair<iterator, bool> emplace_unique(BOOST_MOVE_UREF##N)\
   {  return this->emplace_unique_node(AllocHolder::create_node(BOOST_MOVE_FWD##N));  }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   iterator emplace_hint_unique(const_iterator hint BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {  return this->emplace_hint_unique_node(hint, AllocHolder::create_node(BOOST_MOVE_FWD##N)); }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   iterator emplace_equal(BOOST_MOVE_UREF##N)\
   {\
      NodePtr tmp(AllocHolder::create_node(BOOST_MOVE_FWD##N));\
      scoped_node_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());\
      iterator ret(this->icont().insert_equal(this->icont().end(), *tmp));\
      destroy_deallocator.release();\
      return ret;\
   }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   iterator emplace_hint_equal(const_iterator hint BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      BOOST_ASSERT((priv_is_linked)(hint));\
      NodePtr tmp(AllocHolder::create_node(BOOST_MOVE_FWD##N));\
      scoped_node_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());\
      iterator ret(this->icont().insert_equal(hint.get(), *tmp));\
      destroy_deallocator.release();\
      return ret;\
   }\
   \
   template <class KeyType BOOST_MOVE_I##N BOOST_MOVE_CLASS##N>\
   inline std::pair<iterator, bool>\
      try_emplace(const_iterator hint, BOOST_FWD_REF(KeyType) key BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      insert_commit_data data;\
      const key_type & k = key;\
      std::pair<iiterator, bool> ret =\
         hint == const_iterator() ? this->icont().insert_unique_check(            k, data)\
                                  : this->icont().insert_unique_check(hint.get(), k, data);\
      if(ret.second){\
         ret.first = this->icont().insert_unique_commit\
            (*AllocHolder::create_node(try_emplace_t(), boost::forward<KeyType>(key) BOOST_MOVE_I##N BOOST_MOVE_FWD##N), data);\
      }\
      return std::pair<iterator, bool>(iterator(ret.first), ret.second);\
   }\
   //
   BOOST_MOVE_ITERATE_0TO9(BOOST_CONTAINER_TREE_EMPLACE_CODE)
   #undef BOOST_CONTAINER_TREE_EMPLACE_CODE

   #endif   // !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   //BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(insert_unique, value_type, iterator, this->insert_unique_hint_convertible, const_iterator, const_iterator)

   template <class InputIterator>
   void insert_unique_range(InputIterator first, InputIterator last)
   {
      for( ; first != last; ++first)
         this->insert_unique_convertible(*first);
   }

   template<class MovableConvertible>
   iterator insert_equal_convertible(BOOST_FWD_REF(MovableConvertible) v)
   {
      NodePtr tmp(AllocHolder::create_node(boost::forward<MovableConvertible>(v)));
      scoped_node_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(this->icont().end(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   template<class MovableConvertible>
   iterator insert_equal_hint_convertible(const_iterator hint, BOOST_FWD_REF(MovableConvertible) v)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      NodePtr tmp(AllocHolder::create_node(boost::forward<MovableConvertible>(v)));
      scoped_node_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(hint.get(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG
      (insert_equal, value_type, iterator, this->insert_equal_hint_convertible, const_iterator, const_iterator)

   template <class InputIterator>
   void insert_equal_range(InputIterator first, InputIterator last)
   {
      for( ; first != last; ++first)
         this->insert_equal_convertible(*first);
   }

   template<class KeyType, class M>
   std::pair<iterator, bool> insert_or_assign(const_iterator hint, BOOST_FWD_REF(KeyType) key, BOOST_FWD_REF(M) obj)
   {
      insert_commit_data data;
      const key_type & k = key;  //Support emulated rvalue references
      std::pair<iiterator, bool> ret =
         hint == const_iterator() ? this->icont().insert_unique_check(k, data)
                                  : this->icont().insert_unique_check(hint.get(), k, data);
      if(ret.second){
         ret.first = this->priv_insert_or_assign_commit(boost::forward<KeyType>(key), boost::forward<M>(obj), data);
      }
      else{
         ret.first->get_data().second = boost::forward<M>(obj);
      }
      return std::pair<iterator, bool>(iterator(ret.first), ret.second);
   }

   iterator erase(const_iterator position)
   {
      BOOST_ASSERT(position != this->cend() && (priv_is_linked)(position));
      return iterator(this->icont().erase_and_dispose(position.get(), Destroyer(this->node_alloc())));
   }

   inline size_type erase(const key_type& k)
   {  return AllocHolder::erase_key(k, alloc_version()); }

   size_type erase_unique(const key_type& k)
   {
      iterator i = this->find(k);
      size_type ret = static_cast<size_type>(i != this->end());
      if (ret)
         this->erase(i);
      return ret;
   }

   template <class K>
   inline typename dtl::enable_if_c<
      dtl::is_transparent<key_compare>::value &&      //transparent
      !dtl::is_convertible<K, iterator>::value &&     //not convertible to iterator
      !dtl::is_convertible<K, const_iterator>::value  //not convertible to const_iterator
      , size_type>::type
      erase(const K& k)
   {  return AllocHolder::erase_key(k, KeyNodeCompare(key_comp()), alloc_version()); }

   template <class K>
   inline typename dtl::enable_if_c<
      dtl::is_transparent<key_compare>::value &&      //transparent
      !dtl::is_convertible<K, iterator>::value &&     //not convertible to iterator
      !dtl::is_convertible<K, const_iterator>::value  //not convertible to const_iterator
      , size_type>::type
      erase_unique(const K& k)
   {
      iterator i = this->find(k);
      size_type ret = static_cast<size_type>(i != this->end());

      if (ret)
         this->erase(i);
      return ret;
   }

   iterator erase(const_iterator first, const_iterator last)
   {
      BOOST_ASSERT(first == last || (first != this->cend() && (priv_is_linked)(first)));
      BOOST_ASSERT(first == last || (priv_is_linked)(last));
      return iterator(AllocHolder::erase_range(first.get(), last.get(), alloc_version()));
   }

   node_type extract(const key_type& k)
   {
      iterator const it = this->find(k);
      if(this->end() != it){
         return this->extract(it);
      }
      return node_type();
   }

   node_type extract(const_iterator position)
   {
      BOOST_ASSERT(position != this->cend() && (priv_is_linked)(position));
      iiterator const iit(position.get());
      this->icont().erase(iit);
      return node_type(iit.operator->(), this->node_alloc());
   }

   insert_return_type insert_unique_node(BOOST_RV_REF_BEG_IF_CXX11 node_type BOOST_RV_REF_END_IF_CXX11 nh)
   {
      return this->insert_unique_node(this->end(), boost::move(nh));
   }

   insert_return_type insert_unique_node(const_iterator hint, BOOST_RV_REF_BEG_IF_CXX11 node_type BOOST_RV_REF_END_IF_CXX11 nh)
   {
      insert_return_type irt; //inserted == false, node.empty()
      if(!nh.empty()){
         insert_commit_data data;
         std::pair<iterator,bool> ret =
            this->insert_unique_check(hint, key_of_value_t()(nh.value()), data);
         if(ret.second){
            irt.inserted = true;
            irt.position = iterator(this->icont().insert_unique_commit(*nh.get(), data));
            nh.release();
         }
         else{
            irt.position = ret.first;
            irt.node = boost::move(nh);
         }
      }
      else{
         irt.position = this->end();
      }
      return BOOST_MOVE_RET(insert_return_type, irt);
   }

   iterator insert_equal_node(BOOST_RV_REF_BEG_IF_CXX11 node_type BOOST_RV_REF_END_IF_CXX11 nh)
   {
      if(nh.empty()){
         return this->end();
      }
      else{
         NodePtr const p(nh.release());
         return iterator(this->icont().insert_equal(*p));
      }
   }

   iterator insert_equal_node(const_iterator hint, BOOST_RV_REF_BEG_IF_CXX11 node_type BOOST_RV_REF_END_IF_CXX11 nh)
   {
      if(nh.empty()){
         return this->end();
      }
      else{
         NodePtr const p(nh.release());
         return iterator(this->icont().insert_equal(hint.get(), *p));
      }
   }

   template<class C2>
   inline void merge_unique(tree<T, KeyOfValue, C2, Allocator, Options>& source)
   {  return this->icont().merge_unique(source.icont()); }

   template<class C2>
   inline void merge_equal(tree<T, KeyOfValue, C2, Allocator, Options>& source)
   {  return this->icont().merge_equal(source.icont());  }
   inline void clear()
   {  AllocHolder::clear(alloc_version());  }

   // search operations. Const and non-const overloads even if no iterator is returned
   // so splay implementations can to their rebalancing when searching in non-const versions
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      iterator find(const key_type& k)
   {  return iterator(this->icont().find(k));  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_iterator find(const key_type& k) const
   {  return const_iterator(this->non_const_icont().find(k));  }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, iterator>::type
         find(const K& k)
   {  return iterator(this->icont().find(k, KeyNodeCompare(key_comp())));  }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, const_iterator>::type
         find(const K& k) const
   {  return const_iterator(this->non_const_icont().find(k, KeyNodeCompare(key_comp())));  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      size_type count(const key_type& k) const
   {  return size_type(this->icont().count(k)); }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, size_type>::type
         count(const K& k) const
   {  return size_type(this->icont().count(k, KeyNodeCompare(key_comp()))); }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      bool contains(const key_type& x) const
   {  return this->find(x) != this->cend();  }

   template<typename K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, bool>::type
         contains(const K& x) const
   {  return this->find(x) != this->cend();  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      iterator lower_bound(const key_type& k)
   {  return iterator(this->icont().lower_bound(k));  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_iterator lower_bound(const key_type& k) const
   {  return const_iterator(this->non_const_icont().lower_bound(k));  }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, iterator>::type
         lower_bound(const K& k)
   {  return iterator(this->icont().lower_bound(k, KeyNodeCompare(key_comp())));  }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, const_iterator>::type
         lower_bound(const K& k) const
   {  return const_iterator(this->non_const_icont().lower_bound(k, KeyNodeCompare(key_comp())));  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      iterator upper_bound(const key_type& k)
   {  return iterator(this->icont().upper_bound(k));   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_iterator upper_bound(const key_type& k) const
   {  return const_iterator(this->non_const_icont().upper_bound(k));  }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, iterator>::type
         upper_bound(const K& k)
   {  return iterator(this->icont().upper_bound(k, KeyNodeCompare(key_comp())));   }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, const_iterator>::type
         upper_bound(const K& k) const
   {  return const_iterator(this->non_const_icont().upper_bound(k, KeyNodeCompare(key_comp())));  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      std::pair<iterator,iterator> equal_range(const key_type& k)
   {
      std::pair<iiterator, iiterator> ret = this->icont().equal_range(k);
      return std::pair<iterator,iterator>(iterator(ret.first), iterator(ret.second));
   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      std::pair<const_iterator, const_iterator> equal_range(const key_type& k) const
   {
      std::pair<iiterator, iiterator> ret =
         this->non_const_icont().equal_range(k);
      return std::pair<const_iterator,const_iterator>
         (const_iterator(ret.first), const_iterator(ret.second));
   }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, std::pair<iterator,iterator> >::type
         equal_range(const K& k)
   {
      std::pair<iiterator, iiterator> ret =
         this->icont().equal_range(k, KeyNodeCompare(key_comp()));
      return std::pair<iterator,iterator>(iterator(ret.first), iterator(ret.second));
   }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, std::pair<const_iterator, const_iterator> >::type
         equal_range(const K& k) const
   {
      std::pair<iiterator, iiterator> ret =
         this->non_const_icont().equal_range(k, KeyNodeCompare(key_comp()));
      return std::pair<const_iterator,const_iterator>
         (const_iterator(ret.first), const_iterator(ret.second));
   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      std::pair<iterator,iterator> lower_bound_range(const key_type& k)
   {
      std::pair<iiterator, iiterator> ret =
         this->icont().lower_bound_range(k);
      return std::pair<iterator,iterator>(iterator(ret.first), iterator(ret.second));
   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      std::pair<const_iterator, const_iterator> lower_bound_range(const key_type& k) const
   {
      std::pair<iiterator, iiterator> ret =
         this->non_const_icont().lower_bound_range(k);
      return std::pair<const_iterator,const_iterator>
         (const_iterator(ret.first), const_iterator(ret.second));
   }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, std::pair<iterator,iterator> >::type
         lower_bound_range(const K& k)
   {
      std::pair<iiterator, iiterator> ret =
         this->icont().lower_bound_range(k, KeyNodeCompare(key_comp()));
      return std::pair<iterator,iterator>(iterator(ret.first), iterator(ret.second));
   }

   template <class K>
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      typename dtl::enable_if_transparent<key_compare, K, std::pair<const_iterator, const_iterator> >::type
         lower_bound_range(const K& k) const
   {
      std::pair<iiterator, iiterator> ret =
         this->non_const_icont().lower_bound_range(k, KeyNodeCompare(key_comp()));
      return std::pair<const_iterator,const_iterator>
         (const_iterator(ret.first), const_iterator(ret.second));
   }

   inline void rebalance()
   {  intrusive_tree_proxy_t::rebalance(this->icont());   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator==(const tree& x, const tree& y)
   {  return x.size() == y.size() && ::boost::container::algo_equal(x.begin(), x.end(), y.begin());  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator<(const tree& x, const tree& y)
   {  return ::boost::container::algo_lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator!=(const tree& x, const tree& y)
   {  return !(x == y);  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator>(const tree& x, const tree& y)
   {  return y < x;  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator<=(const tree& x, const tree& y)
   {  return !(y < x);  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator>=(const tree& x, const tree& y)
   {  return !(x < y);  }

   inline friend void swap(tree& x, tree& y)
      BOOST_NOEXCEPT_IF(  allocator_traits_type::is_always_equal::value
                                 && boost::container::dtl::is_nothrow_swappable<Compare>::value )
   {  x.swap(y);  }
};

} //namespace dtl {
} //namespace container {

template <class T>
struct has_trivial_destructor_after_move;

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class T, class KeyOfValue, class Compare, class Allocator, class Options>
struct has_trivial_destructor_after_move
   < 
      ::boost::container::dtl::tree
         <T, KeyOfValue, Compare, Allocator, Options>
   >
{
   typedef typename ::boost::container::dtl::tree<T, KeyOfValue, Compare, Allocator, Options>::allocator_type allocator_type;
   typedef typename ::boost::container::allocator_traits<allocator_type>::pointer pointer;
   BOOST_STATIC_CONSTEXPR bool value =
      ::boost::has_trivial_destructor_after_move<allocator_type>::value &&
      ::boost::has_trivial_destructor_after_move<pointer>::value &&
      ::boost::has_trivial_destructor_after_move<Compare>::value;
};

} //namespace boost  {

#include <boost/container/detail/config_end.hpp>

#endif //BOOST_CONTAINER_TREE_HPP
