//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_NODE_ALLOC_HPP_
#define BOOST_CONTAINER_DETAIL_NODE_ALLOC_HPP_

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

#include <utility>
#include <functional>

#include <boost/move/utility.hpp>
#include <boost/intrusive/options.hpp>

#include <boost/container/detail/version_type.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/utilities.hpp>
#include <boost/container/allocator_traits.hpp>
#include <boost/container/detail/allocator_version_traits.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/destroyers.hpp>
#include <boost/container/detail/memory_util.hpp>
#include <boost/container/detail/allocator_version_traits.hpp>
#include <boost/detail/no_exceptions_support.hpp>

#ifndef BOOST_CONTAINER_PERFECT_FORWARDING
#include <boost/container/detail/preprocessor.hpp>
#endif

#include <boost/container/detail/algorithms.hpp>
#include <new>


namespace boost {
namespace container {
namespace container_detail {

template<class ValueCompare, class Node>
struct node_compare
   :  private ValueCompare
{
   typedef ValueCompare                                 wrapped_value_compare;
   typedef typename wrapped_value_compare::key_type     key_type;
   typedef typename wrapped_value_compare::value_type   value_type;
   typedef typename wrapped_value_compare::key_of_value key_of_value;

   explicit node_compare(const wrapped_value_compare &pred)
      :  wrapped_value_compare(pred)
   {}

   node_compare()
      :  wrapped_value_compare()
   {}

   wrapped_value_compare &value_comp()
   {  return static_cast<wrapped_value_compare &>(*this);  }

   wrapped_value_compare &value_comp() const
   {  return static_cast<const wrapped_value_compare &>(*this);  }

   bool operator()(const Node &a, const Node &b) const
   {  return wrapped_value_compare::operator()(a.get_data(), b.get_data());  }
};

template<class A, class ICont>
struct node_alloc_holder
{
   //If the intrusive container is an associative container, obtain the predicate, which will
   //be of type node_compare<>. If not an associative container value_compare will be a "nat" type.
   typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, ICont,
      value_compare, container_detail::nat)                       intrusive_value_compare;
   //In that case obtain the value predicate from the node predicate via wrapped_value_compare
   //if intrusive_value_compare is node_compare<>, nat otherwise
   typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, ICont,
      wrapped_value_compare, container_detail::nat)               value_compare;

   typedef allocator_traits<A>                                    allocator_traits_type;
   typedef typename allocator_traits_type::value_type             value_type;
   typedef ICont                                                  intrusive_container;
   typedef typename ICont::value_type                             Node;
   typedef typename allocator_traits_type::template
      portable_rebind_alloc<Node>::type                           NodeAlloc;
   typedef allocator_traits<NodeAlloc>                            node_allocator_traits_type;
   typedef container_detail::allocator_version_traits<NodeAlloc>  node_allocator_version_traits_type;
   typedef A                                                      ValAlloc;
   typedef typename node_allocator_traits_type::pointer           NodePtr;
   typedef container_detail::scoped_deallocator<NodeAlloc>        Deallocator;
   typedef typename node_allocator_traits_type::size_type         size_type;
   typedef typename node_allocator_traits_type::difference_type   difference_type;
   typedef container_detail::integral_constant<unsigned, 1>       allocator_v1;
   typedef container_detail::integral_constant<unsigned, 2>       allocator_v2;
   typedef container_detail::integral_constant<unsigned,
      boost::container::container_detail::
         version<NodeAlloc>::value>                   alloc_version;
   typedef typename ICont::iterator                   icont_iterator;
   typedef typename ICont::const_iterator             icont_citerator;
   typedef allocator_destroyer<NodeAlloc>             Destroyer;
   typedef allocator_traits<NodeAlloc>                NodeAllocTraits;
   typedef allocator_version_traits<NodeAlloc>        AllocVersionTraits;

   private:
   BOOST_COPYABLE_AND_MOVABLE(node_alloc_holder)

   public:

   //Constructors for sequence containers
   node_alloc_holder()
      : members_()
   {}

   explicit node_alloc_holder(const ValAlloc &a)
      : members_(a)
   {}

   explicit node_alloc_holder(const node_alloc_holder &x)
      : members_(NodeAllocTraits::select_on_container_copy_construction(x.node_alloc()))
   {}

   explicit node_alloc_holder(BOOST_RV_REF(node_alloc_holder) x)
      : members_(boost::move(x.node_alloc()))
   {  this->icont().swap(x.icont());  }

   //Constructors for associative containers
   explicit node_alloc_holder(const ValAlloc &a, const value_compare &c)
      : members_(a, c)
   {}

   explicit node_alloc_holder(const node_alloc_holder &x, const value_compare &c)
      : members_(NodeAllocTraits::select_on_container_copy_construction(x.node_alloc()), c)
   {}

   explicit node_alloc_holder(const value_compare &c)
      : members_(c)
   {}

   //helpers for move assignments
   explicit node_alloc_holder(BOOST_RV_REF(node_alloc_holder) x, const value_compare &c)
      : members_(boost::move(x.node_alloc()), c)
   {  this->icont().swap(x.icont());  }

   void copy_assign_alloc(const node_alloc_holder &x)
   {
      container_detail::bool_<allocator_traits_type::propagate_on_container_copy_assignment::value> flag;
      container_detail::assign_alloc( static_cast<NodeAlloc &>(this->members_)
                                    , static_cast<const NodeAlloc &>(x.members_), flag);
   }

   void move_assign_alloc( node_alloc_holder &x)
   {
      container_detail::bool_<allocator_traits_type::propagate_on_container_move_assignment::value> flag;
      container_detail::move_alloc( static_cast<NodeAlloc &>(this->members_)
                                  , static_cast<NodeAlloc &>(x.members_), flag);
   }

   ~node_alloc_holder()
   {  this->clear(alloc_version()); }

   size_type max_size() const
   {  return allocator_traits_type::max_size(this->node_alloc());  }

   NodePtr allocate_one()
   {  return AllocVersionTraits::allocate_one(this->node_alloc());   }

   void deallocate_one(const NodePtr &p)
   {  AllocVersionTraits::deallocate_one(this->node_alloc(), p);  }

   #ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   template<class ...Args>
   NodePtr create_node(Args &&...args)
   {
      NodePtr p = this->allocate_one();
      Deallocator node_deallocator(p, this->node_alloc());
      allocator_traits<NodeAlloc>::construct
         ( this->node_alloc()
         , container_detail::addressof(p->m_data), boost::forward<Args>(args)...);
      node_deallocator.release();
      //This does not throw
      typedef typename Node::hook_type hook_type;
      ::new(static_cast<hook_type*>(container_detail::to_raw_pointer(p))) hook_type;
      return (p);
   }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                        \
                                                                                          \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >) \
   NodePtr create_node(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                \
   {                                                                                      \
      NodePtr p = this->allocate_one();                                                   \
      Deallocator node_deallocator(p, this->node_alloc());                                \
      allocator_traits<NodeAlloc>::construct                                              \
         (this->node_alloc(), container_detail::addressof(p->m_data)                      \
            BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));              \
      node_deallocator.release();                                                         \
      typedef typename Node::hook_type hook_type;                                         \
      ::new(static_cast<hook_type*>(container_detail::to_raw_pointer(p))) hook_type;      \
      return (p);                                                                         \
   }                                                                                      \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   template<class It>
   NodePtr create_node_from_it(const It &it)
   {
      NodePtr p = this->allocate_one();
      Deallocator node_deallocator(p, this->node_alloc());
      ::boost::container::construct_in_place(this->node_alloc(), container_detail::addressof(p->m_data), it);
      node_deallocator.release();
      //This does not throw
      typedef typename Node::hook_type hook_type;
      ::new(static_cast<hook_type*>(container_detail::to_raw_pointer(p))) hook_type;
      return (p);
   }

   void destroy_node(const NodePtr &nodep)
   {
      allocator_traits<NodeAlloc>::destroy(this->node_alloc(), container_detail::to_raw_pointer(nodep));
      this->deallocate_one(nodep);
   }

   void swap(node_alloc_holder &x)
   {
      this->icont().swap(x.icont());
      container_detail::bool_<allocator_traits_type::propagate_on_container_swap::value> flag;
      container_detail::swap_alloc(this->node_alloc(), x.node_alloc(), flag);
   }

   template<class FwdIterator, class Inserter>
   void allocate_many_and_construct
      (FwdIterator beg, difference_type n, Inserter inserter)
   {
      if(n){
         typedef typename node_allocator_version_traits_type::multiallocation_chain multiallocation_chain;

         //Try to allocate memory in a single block
         typedef typename multiallocation_chain::iterator multialloc_iterator;
         multiallocation_chain mem;
         NodeAlloc &nalloc = this->node_alloc();
         node_allocator_version_traits_type::allocate_individual(nalloc, n, mem);
         multialloc_iterator itbeg(mem.begin()), itlast(mem.last());
         mem.clear();
         Node *p = 0;
         BOOST_TRY{
            Deallocator node_deallocator(NodePtr(), nalloc);
            container_detail::scoped_destructor<NodeAlloc> sdestructor(nalloc, 0);
            while(n--){
               p = container_detail::to_raw_pointer(iterator_to_pointer(itbeg));
               node_deallocator.set(p);
               ++itbeg;
               //This can throw
               boost::container::construct_in_place(nalloc, container_detail::addressof(p->m_data), beg);
               sdestructor.set(p);
               ++beg;
               //This does not throw
               typedef typename Node::hook_type hook_type;
               ::new(static_cast<hook_type*>(p)) hook_type;
               //This can throw in some containers (predicate might throw).
               //(sdestructor will destruct the node and node_deallocator will deallocate it in case of exception)
               inserter(*p);
               sdestructor.set(0);
            }
            sdestructor.release();
            node_deallocator.release();
         }
         BOOST_CATCH(...){
            mem.incorporate_after(mem.last(), &*itbeg, &*itlast, n);
            node_allocator_version_traits_type::deallocate_individual(this->node_alloc(), mem);
            BOOST_RETHROW
         }
         BOOST_CATCH_END
      }
   }

   void clear(allocator_v1)
   {  this->icont().clear_and_dispose(Destroyer(this->node_alloc()));   }

   void clear(allocator_v2)
   {
      typename NodeAlloc::multiallocation_chain chain;
      allocator_destroyer_and_chain_builder<NodeAlloc> builder(this->node_alloc(), chain);
      this->icont().clear_and_dispose(builder);
      //BOOST_STATIC_ASSERT((::boost::has_move_emulation_enabled<typename NodeAlloc::multiallocation_chain>::value == true));
      if(!chain.empty())
         this->node_alloc().deallocate_individual(chain);
   }

   icont_iterator erase_range(const icont_iterator &first, const icont_iterator &last, allocator_v1)
   {  return this->icont().erase_and_dispose(first, last, Destroyer(this->node_alloc())); }

   icont_iterator erase_range(const icont_iterator &first, const icont_iterator &last, allocator_v2)
   {
      typedef typename NodeAlloc::multiallocation_chain multiallocation_chain;
      NodeAlloc & nalloc = this->node_alloc();
      multiallocation_chain chain;
      allocator_destroyer_and_chain_builder<NodeAlloc> chain_builder(nalloc, chain);
      icont_iterator ret_it = this->icont().erase_and_dispose(first, last, chain_builder);
      nalloc.deallocate_individual(chain);
      return ret_it;
   }

   template<class Key, class Comparator>
   size_type erase_key(const Key& k, const Comparator &comp, allocator_v1)
   {  return this->icont().erase_and_dispose(k, comp, Destroyer(this->node_alloc())); }

   template<class Key, class Comparator>
   size_type erase_key(const Key& k, const Comparator &comp, allocator_v2)
   {
      allocator_multialloc_chain_node_deallocator<NodeAlloc> chain_holder(this->node_alloc());
      return this->icont().erase_and_dispose(k, comp, chain_holder.get_chain_builder());
   }

   protected:
   struct cloner
   {
      cloner(node_alloc_holder &holder)
         :  m_holder(holder)
      {}

      NodePtr operator()(const Node &other) const
      {  return m_holder.create_node(other.get_data());  }

      node_alloc_holder &m_holder;
   };

   struct members_holder
      :  public NodeAlloc
   {
      private:
      members_holder(const members_holder&);
      members_holder & operator=(const members_holder&);

      public:
      members_holder()
         : NodeAlloc(), m_icont()
      {}

      template<class ConvertibleToAlloc>
      explicit members_holder(BOOST_FWD_REF(ConvertibleToAlloc) c2alloc)
         :  NodeAlloc(boost::forward<ConvertibleToAlloc>(c2alloc))
         , m_icont()
      {}

      template<class ConvertibleToAlloc>
      members_holder(BOOST_FWD_REF(ConvertibleToAlloc) c2alloc, const value_compare &c)
         :  NodeAlloc(boost::forward<ConvertibleToAlloc>(c2alloc))
         , m_icont(typename ICont::value_compare(c))
      {}

      explicit members_holder(const value_compare &c)
         : NodeAlloc()
         , m_icont(typename ICont::value_compare(c))
      {}

      //The intrusive container
      ICont m_icont;
   };

   ICont &non_const_icont() const
   {  return const_cast<ICont&>(this->members_.m_icont);   }

   ICont &icont()
   {  return this->members_.m_icont;   }

   const ICont &icont() const
   {  return this->members_.m_icont;   }

   NodeAlloc &node_alloc()
   {  return static_cast<NodeAlloc &>(this->members_);   }

   const NodeAlloc &node_alloc() const
   {  return static_cast<const NodeAlloc &>(this->members_);   }

   members_holder members_;
};

}  //namespace container_detail {
}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif // BOOST_CONTAINER_DETAIL_NODE_ALLOC_HPP_
