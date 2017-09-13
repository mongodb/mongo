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
// other
#include <boost/core/no_exceptions_support.hpp>



#include <boost/container/detail/std_fwd.hpp>

namespace boost {
namespace container {
namespace container_detail {

using boost::intrusive::tree_value_compare;

template<class VoidPointer, boost::container::tree_type_enum tree_type_value, bool OptimizeSize>
struct intrusive_tree_hook;

template<class VoidPointer, bool OptimizeSize>
struct intrusive_tree_hook<VoidPointer, boost::container::red_black_tree, OptimizeSize>
{
   typedef typename container_detail::bi::make_set_base_hook
      < container_detail::bi::void_pointer<VoidPointer>
      , container_detail::bi::link_mode<container_detail::bi::normal_link>
      , container_detail::bi::optimize_size<OptimizeSize>
      >::type  type;
};

template<class VoidPointer, bool OptimizeSize>
struct intrusive_tree_hook<VoidPointer, boost::container::avl_tree, OptimizeSize>
{
   typedef typename container_detail::bi::make_avl_set_base_hook
      < container_detail::bi::void_pointer<VoidPointer>
      , container_detail::bi::link_mode<container_detail::bi::normal_link>
      , container_detail::bi::optimize_size<OptimizeSize>
      >::type  type;
};

template<class VoidPointer, bool OptimizeSize>
struct intrusive_tree_hook<VoidPointer, boost::container::scapegoat_tree, OptimizeSize>
{
   typedef typename container_detail::bi::make_bs_set_base_hook
      < container_detail::bi::void_pointer<VoidPointer>
      , container_detail::bi::link_mode<container_detail::bi::normal_link>
      >::type  type;
};

template<class VoidPointer, bool OptimizeSize>
struct intrusive_tree_hook<VoidPointer, boost::container::splay_tree, OptimizeSize>
{
   typedef typename container_detail::bi::make_bs_set_base_hook
      < container_detail::bi::void_pointer<VoidPointer>
      , container_detail::bi::link_mode<container_detail::bi::normal_link>
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

//The node to be store in the tree
template <class T, class VoidPointer, boost::container::tree_type_enum tree_type_value, bool OptimizeSize>
struct tree_node
   :  public intrusive_tree_hook<VoidPointer, tree_type_value, OptimizeSize>::type
{
   private:
   //BOOST_COPYABLE_AND_MOVABLE(tree_node)
   tree_node();

   public:
   typedef typename intrusive_tree_hook
      <VoidPointer, tree_type_value, OptimizeSize>::type hook_type;
   typedef T value_type;
   typedef typename tree_internal_data_type<T>::type     internal_type;

   typedef tree_node< T, VoidPointer
                    , tree_type_value, OptimizeSize>     node_type;

   T &get_data()
   {
      T* ptr = reinterpret_cast<T*>(&this->m_data);
      return *ptr;
   }

   const T &get_data() const
   {
      const T* ptr = reinterpret_cast<const T*>(&this->m_data);
      return *ptr;
   }

   internal_type m_data;

   template<class T1, class T2>
   void do_assign(const std::pair<const T1, T2> &p)
   {
      const_cast<T1&>(m_data.first) = p.first;
      m_data.second  = p.second;
   }

   template<class T1, class T2>
   void do_assign(const pair<const T1, T2> &p)
   {
      const_cast<T1&>(m_data.first) = p.first;
      m_data.second  = p.second;
   }

   template<class V>
   void do_assign(const V &v)
   {  m_data = v; }

   template<class T1, class T2>
   void do_move_assign(std::pair<const T1, T2> &p)
   {
      const_cast<T1&>(m_data.first) = ::boost::move(p.first);
      m_data.second = ::boost::move(p.second);
   }

   template<class T1, class T2>
   void do_move_assign(pair<const T1, T2> &p)
   {
      const_cast<T1&>(m_data.first) = ::boost::move(p.first);
      m_data.second  = ::boost::move(p.second);
   }

   template<class V>
   void do_move_assign(V &v)
   {  m_data = ::boost::move(v); }
};

template <class T, class VoidPointer, boost::container::tree_type_enum tree_type_value, bool OptimizeSize>
struct iiterator_node_value_type< tree_node<T, VoidPointer, tree_type_value, OptimizeSize> > {
  typedef T type;
};

template<class Node, class Icont>
class insert_equal_end_hint_functor
{
   Icont &icont_;

   public:
   insert_equal_end_hint_functor(Icont &icont)
      :  icont_(icont)
   {}

   void operator()(Node &n)
   {  this->icont_.insert_equal(this->icont_.cend(), n); }
};

template<class Node, class Icont>
class push_back_functor
{
   Icont &icont_;

   public:
   push_back_functor(Icont &icont)
      :  icont_(icont)
   {}

   void operator()(Node &n)
   {  this->icont_.push_back(n); }
};

}//namespace container_detail {

namespace container_detail {

template< class NodeType, class NodeCompareType
        , class SizeType,  class HookType
        , boost::container::tree_type_enum tree_type_value>
struct intrusive_tree_dispatch;

template<class NodeType, class NodeCompareType, class SizeType, class HookType>
struct intrusive_tree_dispatch
   <NodeType, NodeCompareType, SizeType, HookType, boost::container::red_black_tree>
{
   typedef typename container_detail::bi::make_rbtree
      <NodeType
      ,container_detail::bi::compare<NodeCompareType>
      ,container_detail::bi::base_hook<HookType>
      ,container_detail::bi::constant_time_size<true>
      ,container_detail::bi::size_type<SizeType>
      >::type  type;
};

template<class NodeType, class NodeCompareType, class SizeType, class HookType>
struct intrusive_tree_dispatch
   <NodeType, NodeCompareType, SizeType, HookType, boost::container::avl_tree>
{
   typedef typename container_detail::bi::make_avltree
      <NodeType
      ,container_detail::bi::compare<NodeCompareType>
      ,container_detail::bi::base_hook<HookType>
      ,container_detail::bi::constant_time_size<true>
      ,container_detail::bi::size_type<SizeType>
      >::type  type;
};

template<class NodeType, class NodeCompareType, class SizeType, class HookType>
struct intrusive_tree_dispatch
   <NodeType, NodeCompareType, SizeType, HookType, boost::container::scapegoat_tree>
{
   typedef typename container_detail::bi::make_sgtree
      <NodeType
      ,container_detail::bi::compare<NodeCompareType>
      ,container_detail::bi::base_hook<HookType>
      ,container_detail::bi::floating_point<true>
      ,container_detail::bi::size_type<SizeType>
      >::type  type;
};

template<class NodeType, class NodeCompareType, class SizeType, class HookType>
struct intrusive_tree_dispatch
   <NodeType, NodeCompareType, SizeType, HookType, boost::container::splay_tree>
{
   typedef typename container_detail::bi::make_splaytree
      <NodeType
      ,container_detail::bi::compare<NodeCompareType>
      ,container_detail::bi::base_hook<HookType>
      ,container_detail::bi::constant_time_size<true>
      ,container_detail::bi::size_type<SizeType>
      >::type  type;
};

template<class Allocator, class ValueCompare, boost::container::tree_type_enum tree_type_value, bool OptimizeSize>
struct intrusive_tree_type
{
   private:
   typedef typename boost::container::
      allocator_traits<Allocator>::value_type              value_type;
   typedef typename boost::container::
      allocator_traits<Allocator>::void_pointer            void_pointer;
   typedef typename boost::container::
      allocator_traits<Allocator>::size_type               size_type;
   typedef typename container_detail::tree_node
         < value_type, void_pointer
         , tree_type_value, OptimizeSize>          node_type;
   typedef value_to_node_compare
      <node_type, ValueCompare>                    node_compare_type;
   //Deducing the hook type from node_type (e.g. node_type::hook_type) would
   //provoke an early instantiation of node_type that could ruin recursive
   //tree definitions, so retype the complete type to avoid any problem.
   typedef typename intrusive_tree_hook
      <void_pointer, tree_type_value
      , OptimizeSize>::type                        hook_type;
   public:
   typedef typename intrusive_tree_dispatch
      < node_type, node_compare_type
      , size_type, hook_type
      , tree_type_value>::type                     type;
};

//Trait to detect manually rebalanceable tree types
template<boost::container::tree_type_enum tree_type_value>
struct is_manually_balanceable
{  static const bool value = true;  };

template<>  struct is_manually_balanceable<red_black_tree>
{  static const bool value = false; };

template<>  struct is_manually_balanceable<avl_tree>
{  static const bool value = false; };

//Proxy traits to implement different operations depending on the
//is_manually_balanceable<>::value
template< boost::container::tree_type_enum tree_type_value
        , bool IsManuallyRebalanceable = is_manually_balanceable<tree_type_value>::value>
struct intrusive_tree_proxy
{
   template<class Icont>
   static void rebalance(Icont &)   {}
};

template<boost::container::tree_type_enum tree_type_value>
struct intrusive_tree_proxy<tree_type_value, true>
{
   template<class Icont>
   static void rebalance(Icont &c)
   {  c.rebalance(); }
};

}  //namespace container_detail {

namespace container_detail {

//This functor will be used with Intrusive clone functions to obtain
//already allocated nodes from a intrusive container instead of
//allocating new ones. When the intrusive container runs out of nodes
//the node holder is used instead.
template<class AllocHolder, bool DoMove>
class RecyclingCloner
{
   typedef typename AllocHolder::intrusive_container  intrusive_container;
   typedef typename AllocHolder::Node                 node_type;
   typedef typename AllocHolder::NodePtr              node_ptr_type;

   public:
   RecyclingCloner(AllocHolder &holder, intrusive_container &itree)
      :  m_holder(holder), m_icont(itree)
   {}

   static void do_assign(node_ptr_type &p, const node_type &other, bool_<true>)
   {  p->do_move_assign(const_cast<node_type &>(other).m_data);   }

   static void do_assign(node_ptr_type &p, const node_type &other, bool_<false>)
   {  p->do_assign(other.m_data);   }

   node_ptr_type operator()(const node_type &other) const
   {
      if(node_ptr_type p = m_icont.unlink_leftmost_without_rebalance()){
         //First recycle a node (this can't throw)
         BOOST_TRY{
            //This can throw
            this->do_assign(p, other, bool_<DoMove>());
            return p;
         }
         BOOST_CATCH(...){
            //If there is an exception destroy the whole source
            m_holder.destroy_node(p);
            while((p = m_icont.unlink_leftmost_without_rebalance())){
               m_holder.destroy_node(p);
            }
            BOOST_RETHROW
         }
         BOOST_CATCH_END
      }
      else{
         return m_holder.create_node(other.m_data);
      }
   }

   AllocHolder &m_holder;
   intrusive_container &m_icont;
};

template<class KeyValueCompare, class Node>
//where KeyValueCompare is tree_value_compare<Key, T, Compare, KeyOfValue>
struct key_node_compare
   :  private KeyValueCompare
{
   explicit key_node_compare(const KeyValueCompare &comp)
      :  KeyValueCompare(comp)
   {}

   template<class T>
   struct is_node
   {
      static const bool value = is_same<T, Node>::value;
   };

   template<class T>
   typename enable_if_c<is_node<T>::value, const typename KeyValueCompare::value_type &>::type
      key_forward(const T &node) const
   {  return node.get_data();  }

   template<class T>
   #if defined(BOOST_MOVE_HELPERS_RETURN_SFINAE_BROKEN)
   const T &key_forward(const T &key, typename enable_if_c<!is_node<T>::value>::type* =0) const
   #else
   typename enable_if_c<!is_node<T>::value, const T &>::type key_forward(const T &key) const
   #endif
   {  return key; }

   template<class KeyType, class KeyType2>
   bool operator()(const KeyType &key1, const KeyType2 &key2) const
   {  return KeyValueCompare::operator()(this->key_forward(key1), this->key_forward(key2));  }
};

template <class Key, class T, class KeyOfValue,
          class Compare, class Allocator,
          class Options = tree_assoc_defaults>
class tree
   : protected container_detail::node_alloc_holder
      < Allocator
      , typename container_detail::intrusive_tree_type
         < Allocator, tree_value_compare<Key, T, Compare, KeyOfValue> //ValComp
         , Options::tree_type, Options::optimize_size>::type
      >
{
   typedef tree_value_compare
            <Key, T, Compare, KeyOfValue>                   ValComp;
   typedef typename container_detail::intrusive_tree_type
         < Allocator, ValComp, Options::tree_type
         , Options::optimize_size>::type                    Icont;
   typedef container_detail::node_alloc_holder
      <Allocator, Icont>                                    AllocHolder;
   typedef typename AllocHolder::NodePtr                    NodePtr;
   typedef tree < Key, T, KeyOfValue
                , Compare, Allocator, Options>              ThisType;
   typedef typename AllocHolder::NodeAlloc                  NodeAlloc;
   typedef boost::container::
      allocator_traits<NodeAlloc>                           allocator_traits_type;
   typedef typename AllocHolder::ValAlloc                   ValAlloc;
   typedef typename AllocHolder::Node                       Node;
   typedef typename Icont::iterator                         iiterator;
   typedef typename Icont::const_iterator                   iconst_iterator;
   typedef container_detail::allocator_destroyer<NodeAlloc> Destroyer;
   typedef typename AllocHolder::alloc_version              alloc_version;
   typedef intrusive_tree_proxy<Options::tree_type>         intrusive_tree_proxy_t;

   BOOST_COPYABLE_AND_MOVABLE(tree)

   public:

   typedef Key                                        key_type;
   typedef T                                          value_type;
   typedef Allocator                                  allocator_type;
   typedef Compare                                    key_compare;
   typedef ValComp                                    value_compare;
   typedef typename boost::container::
      allocator_traits<Allocator>::pointer            pointer;
   typedef typename boost::container::
      allocator_traits<Allocator>::const_pointer      const_pointer;
   typedef typename boost::container::
      allocator_traits<Allocator>::reference          reference;
   typedef typename boost::container::
      allocator_traits<Allocator>::const_reference    const_reference;
   typedef typename boost::container::
      allocator_traits<Allocator>::size_type          size_type;
   typedef typename boost::container::
      allocator_traits<Allocator>::difference_type    difference_type;
   typedef difference_type                            tree_difference_type;
   typedef pointer                                    tree_pointer;
   typedef const_pointer                              tree_const_pointer;
   typedef reference                                  tree_reference;
   typedef const_reference                            tree_const_reference;
   typedef NodeAlloc                                  stored_allocator_type;

   private:

   typedef key_node_compare<value_compare, Node>  KeyNodeCompare;

   public:
   typedef container_detail::iterator_from_iiterator<iiterator, false>  iterator;
   typedef container_detail::iterator_from_iiterator<iiterator, true >  const_iterator;
   typedef boost::container::reverse_iterator<iterator>                 reverse_iterator;
   typedef boost::container::reverse_iterator<const_iterator>           const_reverse_iterator;

   tree()
      : AllocHolder()
   {}

   explicit tree(const key_compare& comp, const allocator_type& a = allocator_type())
      : AllocHolder(ValComp(comp), a)
   {}

   explicit tree(const allocator_type& a)
      : AllocHolder(a)
   {}

   template <class InputIterator>
   tree(bool unique_insertion, InputIterator first, InputIterator last, const key_compare& comp,
          const allocator_type& a
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_or
         < void
         , container_detail::is_same<alloc_version, version_1>
         , container_detail::is_input_iterator<InputIterator>
         >::type * = 0
      #endif
         )
      : AllocHolder(value_compare(comp), a)
   {
      //Use cend() as hint to achieve linear time for
      //ordered ranges as required by the standard
      //for the constructor
      const const_iterator end_it(this->cend());
      if(unique_insertion){
         for ( ; first != last; ++first){
            this->insert_unique(end_it, *first);
         }
      }
      else{
         for ( ; first != last; ++first){
            this->insert_equal(end_it, *first);
         }
      }
   }

   template <class InputIterator>
   tree(bool unique_insertion, InputIterator first, InputIterator last, const key_compare& comp,
          const allocator_type& a
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::disable_if_or
         < void
         , container_detail::is_same<alloc_version, version_1>
         , container_detail::is_input_iterator<InputIterator>
         >::type * = 0
      #endif
         )
      : AllocHolder(value_compare(comp), a)
   {
      if(unique_insertion){
         //Use cend() as hint to achieve linear time for
         //ordered ranges as required by the standard
         //for the constructor
         const const_iterator end_it(this->cend());
         for ( ; first != last; ++first){
            this->insert_unique(end_it, *first);
         }
      }
      else{
         //Optimized allocation and construction
         this->allocate_many_and_construct
            ( first, boost::container::iterator_distance(first, last)
            , insert_equal_end_hint_functor<Node, Icont>(this->icont()));
      }
   }

   template <class InputIterator>
   tree( ordered_range_t, InputIterator first, InputIterator last
         , const key_compare& comp = key_compare(), const allocator_type& a = allocator_type()
         #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_or
         < void
         , container_detail::is_same<alloc_version, version_1>
         , container_detail::is_input_iterator<InputIterator>
         >::type * = 0
         #endif
         )
      : AllocHolder(value_compare(comp), a)
   {
      for ( ; first != last; ++first){
         this->push_back_impl(*first);
      }
   }

   template <class InputIterator>
   tree( ordered_range_t, InputIterator first, InputIterator last
         , const key_compare& comp = key_compare(), const allocator_type& a = allocator_type()
         #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::disable_if_or
         < void
         , container_detail::is_same<alloc_version, version_1>
         , container_detail::is_input_iterator<InputIterator>
         >::type * = 0
         #endif
         )
      : AllocHolder(value_compare(comp), a)
   {
      //Optimized allocation and construction
      this->allocate_many_and_construct
         ( first, boost::container::iterator_distance(first, last)
         , container_detail::push_back_functor<Node, Icont>(this->icont()));
   }

   tree(const tree& x)
      :  AllocHolder(x.value_comp(), x)
   {
      this->icont().clone_from
         (x.icont(), typename AllocHolder::cloner(*this), Destroyer(this->node_alloc()));
   }

   tree(BOOST_RV_REF(tree) x)
      :  AllocHolder(BOOST_MOVE_BASE(AllocHolder, x), x.value_comp())
   {}

   tree(const tree& x, const allocator_type &a)
      :  AllocHolder(x.value_comp(), a)
   {
      this->icont().clone_from
         (x.icont(), typename AllocHolder::cloner(*this), Destroyer(this->node_alloc()));
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
   }

   ~tree()
   {} //AllocHolder clears the tree

   tree& operator=(BOOST_COPY_ASSIGN_REF(tree) x)
   {
      if (&x != this){
         NodeAlloc &this_alloc     = this->get_stored_allocator();
         const NodeAlloc &x_alloc  = x.get_stored_allocator();
         container_detail::bool_<allocator_traits<NodeAlloc>::
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
      BOOST_NOEXCEPT_IF(  allocator_traits_type::is_always_equal::value
                                 && boost::container::container_detail::is_nothrow_move_assignable<Compare>::value )
   {
      BOOST_ASSERT(this != &x);
      NodeAlloc &this_alloc = this->node_alloc();
      NodeAlloc &x_alloc    = x.node_alloc();
      const bool propagate_alloc = allocator_traits<NodeAlloc>::
            propagate_on_container_move_assignment::value;
      const bool allocators_equal = this_alloc == x_alloc; (void)allocators_equal;
      //Resources can be transferred if both allocators are
      //going to be equal after this function (either propagated or already equal)
      if(propagate_alloc || allocators_equal){
         //Destroy
         this->clear();
         //Move allocator if needed
         this->AllocHolder::move_assign_alloc(x);
         //Obtain resources
         this->icont() = boost::move(x.icont());
      }
      //Else do a one by one move
      else{
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
         while((p = other_tree.unlink_leftmost_without_rebalance())){
            AllocHolder::destroy_node(p);
         }
      }
      return *this;
   }

   public:
   // accessors:
   value_compare value_comp() const
   {  return this->icont().value_comp().predicate(); }

   key_compare key_comp() const
   {  return this->icont().value_comp().predicate().key_comp(); }

   allocator_type get_allocator() const
   {  return allocator_type(this->node_alloc()); }

   const stored_allocator_type &get_stored_allocator() const
   {  return this->node_alloc(); }

   stored_allocator_type &get_stored_allocator()
   {  return this->node_alloc(); }

   iterator begin()
   { return iterator(this->icont().begin()); }

   const_iterator begin() const
   {  return this->cbegin();  }

   iterator end()
   {  return iterator(this->icont().end());  }

   const_iterator end() const
   {  return this->cend();  }

   reverse_iterator rbegin()
   {  return reverse_iterator(end());  }

   const_reverse_iterator rbegin() const
   {  return this->crbegin();  }

   reverse_iterator rend()
   {  return reverse_iterator(begin());   }

   const_reverse_iterator rend() const
   {  return this->crend();   }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cbegin() const
   { return const_iterator(this->non_const_icont().begin()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cend() const
   { return const_iterator(this->non_const_icont().end()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin() const
   { return const_reverse_iterator(cend()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend() const
   { return const_reverse_iterator(cbegin()); }

   bool empty() const
   {  return !this->size();  }

   size_type size() const
   {  return this->icont().size();   }

   size_type max_size() const
   {  return AllocHolder::max_size();  }

   void swap(ThisType& x)
      BOOST_NOEXCEPT_IF(  allocator_traits_type::is_always_equal::value
                                 && boost::container::container_detail::is_nothrow_swappable<Compare>::value )
   {  AllocHolder::swap(x);   }

   public:

   typedef typename Icont::insert_commit_data insert_commit_data;

   // insert/erase
   std::pair<iterator,bool> insert_unique_check
      (const key_type& key, insert_commit_data &data)
   {
      std::pair<iiterator, bool> ret =
         this->icont().insert_unique_check(key, KeyNodeCompare(value_comp()), data);
      return std::pair<iterator, bool>(iterator(ret.first), ret.second);
   }

   std::pair<iterator,bool> insert_unique_check
      (const_iterator hint, const key_type& key, insert_commit_data &data)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      std::pair<iiterator, bool> ret =
         this->icont().insert_unique_check(hint.get(), key, KeyNodeCompare(value_comp()), data);
      return std::pair<iterator, bool>(iterator(ret.first), ret.second);
   }

   iterator insert_unique_commit(const value_type& v, insert_commit_data &data)
   {
      NodePtr tmp = AllocHolder::create_node(v);
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_unique_commit(*tmp, data));
      destroy_deallocator.release();
      return ret;
   }

   template<class MovableConvertible>
   iterator insert_unique_commit
      (BOOST_FWD_REF(MovableConvertible) v, insert_commit_data &data)
   {
      NodePtr tmp = AllocHolder::create_node(boost::forward<MovableConvertible>(v));
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_unique_commit(*tmp, data));
      destroy_deallocator.release();
      return ret;
   }

   std::pair<iterator,bool> insert_unique(const value_type& v)
   {
      insert_commit_data data;
      std::pair<iterator,bool> ret =
         this->insert_unique_check(KeyOfValue()(v), data);
      if(ret.second){
         ret.first = this->insert_unique_commit(v, data);
      }
      return ret;
   }

   template<class MovableConvertible>
   std::pair<iterator,bool> insert_unique(BOOST_FWD_REF(MovableConvertible) v)
   {
      insert_commit_data data;
      std::pair<iterator,bool> ret =
         this->insert_unique_check(KeyOfValue()(v), data);
      if(ret.second){
         ret.first = this->insert_unique_commit(boost::forward<MovableConvertible>(v), data);
      }
      return ret;
   }

   private:

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

   std::pair<iterator, bool> emplace_unique_impl(NodePtr p)
   {
      value_type &v = p->get_data();
      insert_commit_data data;
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(p, this->node_alloc());
      std::pair<iterator,bool> ret =
         this->insert_unique_check(KeyOfValue()(v), data);
      if(!ret.second){
         return ret;
      }
      //No throw insertion part, release rollback
      destroy_deallocator.release();
      return std::pair<iterator,bool>
         ( iterator(iiterator(this->icont().insert_unique_commit(*p, data)))
         , true );
   }

   iterator emplace_unique_hint_impl(const_iterator hint, NodePtr p)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      value_type &v = p->get_data();
      insert_commit_data data;
      std::pair<iterator,bool> ret =
         this->insert_unique_check(hint, KeyOfValue()(v), data);
      if(!ret.second){
         Destroyer(this->node_alloc())(p);
         return ret.first;
      }
      return iterator(iiterator(this->icont().insert_unique_commit(*p, data)));
   }

   public:

   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   template <class... Args>
   std::pair<iterator, bool> emplace_unique(BOOST_FWD_REF(Args)... args)
   {  return this->emplace_unique_impl(AllocHolder::create_node(boost::forward<Args>(args)...));   }

   template <class... Args>
   iterator emplace_hint_unique(const_iterator hint, BOOST_FWD_REF(Args)... args)
   {  return this->emplace_unique_hint_impl(hint, AllocHolder::create_node(boost::forward<Args>(args)...));   }

   template <class... Args>
   iterator emplace_equal(BOOST_FWD_REF(Args)... args)
   {
      NodePtr tmp(AllocHolder::create_node(boost::forward<Args>(args)...));
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(this->icont().end(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   template <class... Args>
   iterator emplace_hint_equal(const_iterator hint, BOOST_FWD_REF(Args)... args)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      NodePtr tmp(AllocHolder::create_node(boost::forward<Args>(args)...));
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(hint.get(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   #else // !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   #define BOOST_CONTAINER_TREE_EMPLACE_CODE(N) \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   std::pair<iterator, bool> emplace_unique(BOOST_MOVE_UREF##N)\
   {  return this->emplace_unique_impl(AllocHolder::create_node(BOOST_MOVE_FWD##N));  }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   iterator emplace_hint_unique(const_iterator hint BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {  return this->emplace_unique_hint_impl(hint, AllocHolder::create_node(BOOST_MOVE_FWD##N)); }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   iterator emplace_equal(BOOST_MOVE_UREF##N)\
   {\
      NodePtr tmp(AllocHolder::create_node(BOOST_MOVE_FWD##N));\
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());\
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
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());\
      iterator ret(this->icont().insert_equal(hint.get(), *tmp));\
      destroy_deallocator.release();\
      return ret;\
   }\
   //
   BOOST_MOVE_ITERATE_0TO9(BOOST_CONTAINER_TREE_EMPLACE_CODE)
   #undef BOOST_CONTAINER_TREE_EMPLACE_CODE

   #endif   // !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   iterator insert_unique(const_iterator hint, const value_type& v)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      insert_commit_data data;
      std::pair<iterator,bool> ret =
         this->insert_unique_check(hint, KeyOfValue()(v), data);
      if(!ret.second)
         return ret.first;
      return this->insert_unique_commit(v, data);
   }

   template<class MovableConvertible>
   iterator insert_unique(const_iterator hint, BOOST_FWD_REF(MovableConvertible) v)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      insert_commit_data data;
      std::pair<iterator,bool> ret =
         this->insert_unique_check(hint, KeyOfValue()(v), data);
      if(!ret.second)
         return ret.first;
      return this->insert_unique_commit(boost::forward<MovableConvertible>(v), data);
   }

   template <class InputIterator>
   void insert_unique(InputIterator first, InputIterator last)
   {
      for( ; first != last; ++first)
         this->insert_unique(*first);
   }

   iterator insert_equal(const value_type& v)
   {
      NodePtr tmp(AllocHolder::create_node(v));
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(this->icont().end(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   template<class MovableConvertible>
   iterator insert_equal(BOOST_FWD_REF(MovableConvertible) v)
   {
      NodePtr tmp(AllocHolder::create_node(boost::forward<MovableConvertible>(v)));
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(this->icont().end(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   iterator insert_equal(const_iterator hint, const value_type& v)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      NodePtr tmp(AllocHolder::create_node(v));
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(hint.get(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   template<class MovableConvertible>
   iterator insert_equal(const_iterator hint, BOOST_FWD_REF(MovableConvertible) v)
   {
      BOOST_ASSERT((priv_is_linked)(hint));
      NodePtr tmp(AllocHolder::create_node(boost::forward<MovableConvertible>(v)));
      scoped_destroy_deallocator<NodeAlloc> destroy_deallocator(tmp, this->node_alloc());
      iterator ret(this->icont().insert_equal(hint.get(), *tmp));
      destroy_deallocator.release();
      return ret;
   }

   template <class InputIterator>
   void insert_equal(InputIterator first, InputIterator last)
   {
      for( ; first != last; ++first)
         this->insert_equal(*first);
   }

   iterator erase(const_iterator position)
   {
      BOOST_ASSERT(position != this->cend() && (priv_is_linked)(position));
      return iterator(this->icont().erase_and_dispose(position.get(), Destroyer(this->node_alloc())));
   }

   size_type erase(const key_type& k)
   {  return AllocHolder::erase_key(k, KeyNodeCompare(value_comp()), alloc_version()); }

   iterator erase(const_iterator first, const_iterator last)
   {
      BOOST_ASSERT(first == last || (first != this->cend() && (priv_is_linked)(first)));
      BOOST_ASSERT(first == last || (priv_is_linked)(last));
      return iterator(AllocHolder::erase_range(first.get(), last.get(), alloc_version()));
   }

   void clear()
   {  AllocHolder::clear(alloc_version());  }

   // search operations. Const and non-const overloads even if no iterator is returned
   // so splay implementations can to their rebalancing when searching in non-const versions
   iterator find(const key_type& k)
   {  return iterator(this->icont().find(k, KeyNodeCompare(value_comp())));  }

   const_iterator find(const key_type& k) const
   {  return const_iterator(this->non_const_icont().find(k, KeyNodeCompare(value_comp())));  }

   size_type count(const key_type& k) const
   {  return size_type(this->icont().count(k, KeyNodeCompare(value_comp()))); }

   iterator lower_bound(const key_type& k)
   {  return iterator(this->icont().lower_bound(k, KeyNodeCompare(value_comp())));  }

   const_iterator lower_bound(const key_type& k) const
   {  return const_iterator(this->non_const_icont().lower_bound(k, KeyNodeCompare(value_comp())));  }

   iterator upper_bound(const key_type& k)
   {  return iterator(this->icont().upper_bound(k, KeyNodeCompare(value_comp())));   }

   const_iterator upper_bound(const key_type& k) const
   {  return const_iterator(this->non_const_icont().upper_bound(k, KeyNodeCompare(value_comp())));  }

   std::pair<iterator,iterator> equal_range(const key_type& k)
   {
      std::pair<iiterator, iiterator> ret =
         this->icont().equal_range(k, KeyNodeCompare(value_comp()));
      return std::pair<iterator,iterator>(iterator(ret.first), iterator(ret.second));
   }

   std::pair<const_iterator, const_iterator> equal_range(const key_type& k) const
   {
      std::pair<iiterator, iiterator> ret =
         this->non_const_icont().equal_range(k, KeyNodeCompare(value_comp()));
      return std::pair<const_iterator,const_iterator>
         (const_iterator(ret.first), const_iterator(ret.second));
   }

   std::pair<iterator,iterator> lower_bound_range(const key_type& k)
   {
      std::pair<iiterator, iiterator> ret =
         this->icont().lower_bound_range(k, KeyNodeCompare(value_comp()));
      return std::pair<iterator,iterator>(iterator(ret.first), iterator(ret.second));
   }

   std::pair<const_iterator, const_iterator> lower_bound_range(const key_type& k) const
   {
      std::pair<iiterator, iiterator> ret =
         this->non_const_icont().lower_bound_range(k, KeyNodeCompare(value_comp()));
      return std::pair<const_iterator,const_iterator>
         (const_iterator(ret.first), const_iterator(ret.second));
   }

   void rebalance()
   {  intrusive_tree_proxy_t::rebalance(this->icont());   }

   friend bool operator==(const tree& x, const tree& y)
   {  return x.size() == y.size() && ::boost::container::algo_equal(x.begin(), x.end(), y.begin());  }

   friend bool operator<(const tree& x, const tree& y)
   {  return ::boost::container::algo_lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   friend bool operator!=(const tree& x, const tree& y)
   {  return !(x == y);  }

   friend bool operator>(const tree& x, const tree& y)
   {  return y < x;  }

   friend bool operator<=(const tree& x, const tree& y)
   {  return !(y < x);  }

   friend bool operator>=(const tree& x, const tree& y)
   {  return !(x < y);  }

   friend void swap(tree& x, tree& y)
   {  x.swap(y);  }
};

} //namespace container_detail {
} //namespace container {

template <class T>
struct has_trivial_destructor_after_move;

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class Key, class T, class KeyOfValue, class Compare, class Allocator, class Options>
struct has_trivial_destructor_after_move
   < 
      ::boost::container::container_detail::tree
         <Key, T, KeyOfValue, Compare, Allocator, Options>
   >
{
   typedef typename ::boost::container::allocator_traits<Allocator>::pointer pointer;
   static const bool value = ::boost::has_trivial_destructor_after_move<Allocator>::value &&
                             ::boost::has_trivial_destructor_after_move<pointer>::value &&
                             ::boost::has_trivial_destructor_after_move<Compare>::value;
};

} //namespace boost  {

#include <boost/container/detail/config_end.hpp>

#endif //BOOST_CONTAINER_TREE_HPP
