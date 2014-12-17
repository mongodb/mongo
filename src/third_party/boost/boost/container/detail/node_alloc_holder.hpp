//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_NODE_ALLOC_HPP_
#define BOOST_CONTAINER_DETAIL_NODE_ALLOC_HPP_

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include "config_begin.hpp"
#include <boost/container/detail/workaround.hpp>

#include <utility>
#include <functional>

#include <boost/move/move.hpp>
#include <boost/intrusive/options.hpp>

#include <boost/container/detail/version_type.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/utilities.hpp>
#include <boost/container/allocator/allocator_traits.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/destroyers.hpp>

#ifndef BOOST_CONTAINER_PERFECT_FORWARDING
#include <boost/container/detail/preprocessor.hpp>
#endif

#include <boost/container/detail/algorithms.hpp>


namespace boost {
namespace container {
namespace container_detail {

//!A deleter for scoped_ptr that deallocates the memory
//!allocated for an object using a STL allocator.
template <class A>
struct scoped_deallocator
{
   typedef allocator_traits<A> allocator_traits_type;
   typedef typename allocator_traits_type::pointer pointer;
   typedef container_detail::integral_constant<unsigned,
      boost::container::container_detail::
         version<A>::value>                   alloc_version;
   typedef container_detail::integral_constant<unsigned, 1>     allocator_v1;
   typedef container_detail::integral_constant<unsigned, 2>     allocator_v2;

   private:
   void priv_deallocate(allocator_v1)
   {  m_alloc.deallocate(m_ptr, 1); }

   void priv_deallocate(allocator_v2)
   {  m_alloc.deallocate_one(m_ptr); }

   BOOST_MOVABLE_BUT_NOT_COPYABLE(scoped_deallocator)

   public:

   pointer     m_ptr;
   A&  m_alloc;

   scoped_deallocator(pointer p, A& a)
      : m_ptr(p), m_alloc(a)
   {}

   ~scoped_deallocator()
   {  if (m_ptr)priv_deallocate(alloc_version());  }

   scoped_deallocator(BOOST_RV_REF(scoped_deallocator) o)
      :  m_ptr(o.m_ptr), m_alloc(o.m_alloc)
   {  o.release();  }

   pointer get() const
   {  return m_ptr;  }

   void release()
   {  m_ptr = 0; }
};

template <class A>
class allocator_destroyer_and_chain_builder
{
   typedef allocator_traits<A> allocator_traits_type;
   typedef typename allocator_traits_type::value_type value_type;
   typedef typename A::multiallocation_chain    multiallocation_chain;

   A & a_;
   multiallocation_chain &c_;

   public:
   allocator_destroyer_and_chain_builder(A &a, multiallocation_chain &c)
      :  a_(a), c_(c)
   {}

   void operator()(const typename A::pointer &p)
   {
      allocator_traits<A>::destroy(a_, container_detail::to_raw_pointer(p));
      c_.push_front(p);
   }
};

template <class A>
class allocator_multialloc_chain_node_deallocator
{
   typedef allocator_traits<A> allocator_traits_type;
   typedef typename allocator_traits_type::value_type value_type;
   typedef typename A::multiallocation_chain    multiallocation_chain;
   typedef allocator_destroyer_and_chain_builder<A> chain_builder;

   A & a_;
   multiallocation_chain c_;

   public:
   allocator_multialloc_chain_node_deallocator(A &a)
      :  a_(a), c_()
   {}

   chain_builder get_chain_builder()
   {  return chain_builder(a_, c_);  }

   ~allocator_multialloc_chain_node_deallocator()
   {
      if(!c_.empty())
         a_.deallocate_individual(boost::move(c_));
   }
};

template<class ValueCompare, class Node>
struct node_compare
   :  private ValueCompare
{
   typedef typename ValueCompare::key_type     key_type;
   typedef typename ValueCompare::value_type   value_type;
   typedef typename ValueCompare::key_of_value key_of_value;

   node_compare(const ValueCompare &pred)
      :  ValueCompare(pred)
   {}

   ValueCompare &value_comp()
   {  return static_cast<ValueCompare &>(*this);  }

   ValueCompare &value_comp() const
   {  return static_cast<const ValueCompare &>(*this);  }

   bool operator()(const Node &a, const Node &b) const
   {  return ValueCompare::operator()(a.get_data(), b.get_data());  }
};

template<class A, class ICont, class Pred = container_detail::nat>
struct node_alloc_holder
{
   typedef allocator_traits<A>                                    allocator_traits_type;
   typedef node_alloc_holder<A, ICont>                            self_t;
   typedef typename allocator_traits_type::value_type             value_type;
   typedef typename ICont::value_type                             Node;
   typedef typename allocator_traits_type::template
      portable_rebind_alloc<Node>::type                           NodeAlloc;
   typedef allocator_traits<NodeAlloc>                            node_allocator_traits_type;
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
   explicit node_alloc_holder(const ValAlloc &a, const Pred &c) 
      : members_(a, c)
   {}

   explicit node_alloc_holder(const node_alloc_holder &x, const Pred &c)
      : members_(NodeAllocTraits::select_on_container_copy_construction(x.node_alloc()), c)
   {}

   explicit node_alloc_holder(const Pred &c)
      : members_(c)
   {}

   //helpers for move assignments
   explicit node_alloc_holder(BOOST_RV_REF(node_alloc_holder) x, const Pred &c)
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
   {  return this->allocate_one(alloc_version());   }

   NodePtr allocate_one(allocator_v1)
   {  return this->node_alloc().allocate(1);   }

   NodePtr allocate_one(allocator_v2)
   {  return this->node_alloc().allocate_one();   }

   void deallocate_one(const NodePtr &p)
   {  return this->deallocate_one(p, alloc_version());   }

   void deallocate_one(const NodePtr &p, allocator_v1)
   {  this->node_alloc().deallocate(p, 1);   }

   void deallocate_one(const NodePtr &p, allocator_v2)
   {  this->node_alloc().deallocate_one(p);   }
/*
   template<class A, class Convertible1, class Convertible2>
   static void construct(A &a, const NodePtr &ptr,
      BOOST_RV_REF_2_TEMPL_ARGS(std::pair, Convertible1, Convertible2) value)
   {
      typedef typename Node::hook_type                hook_type;
      typedef typename Node::value_type::first_type   first_type;
      typedef typename Node::value_type::second_type  second_type;
      Node *nodeptr = container_detail::to_raw_pointer(ptr);

      //Hook constructor does not throw
      allocator_traits<A>::construct(a, static_cast<hook_type*>(nodeptr));

      //Now construct pair members_holder
      value_type *valueptr = &nodeptr->get_data();
      allocator_traits<A>::construct(a, &valueptr->first, boost::move(value.first));
      BOOST_TRY{
         allocator_traits<A>::construct(a, &valueptr->second, boost::move(value.second));
      }
      BOOST_CATCH(...){
         allocator_traits<A>::destroy(a, &valueptr->first);
         BOOST_RETHROW
      }
      BOOST_CATCH_END
   }
*/
   #ifdef BOOST_CONTAINER_PERFECT_FORWARDING
/*
   template<class A, class ...Args>
   static void construct(A &a, const NodePtr &ptr, Args &&...args)
   {  
   }
*/
   template<class ...Args>
   NodePtr create_node(Args &&...args)
   {
      NodePtr p = this->allocate_one();
      Deallocator node_deallocator(p, this->node_alloc());
      allocator_traits<NodeAlloc>::construct
         (this->node_alloc(), container_detail::to_raw_pointer(p), boost::forward<Args>(args)...);
      node_deallocator.release();
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
         (this->node_alloc(), container_detail::to_raw_pointer(p)                         \
            BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));              \
      node_deallocator.release();                                                         \
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
      ::boost::container::construct_in_place(this->node_alloc(), container_detail::to_raw_pointer(p), it);
      node_deallocator.release();
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
   FwdIterator allocate_many_and_construct
      (FwdIterator beg, difference_type n, Inserter inserter)
   {
      if(n){
         typedef typename NodeAlloc::multiallocation_chain multiallocation_chain;

         //Try to allocate memory in a single block
         multiallocation_chain mem(this->node_alloc().allocate_individual(n));
         int constructed = 0;
         Node *p = 0;
         BOOST_TRY{
               for(difference_type i = 0; i < n; ++i, ++beg, --constructed){
               p = container_detail::to_raw_pointer(mem.front());
               mem.pop_front();
               //This can throw
               constructed = 0;
               boost::container::construct_in_place(this->node_alloc(), p, beg);
               ++constructed;
               //This can throw in some containers (predicate might throw)
               inserter(*p);
            }
         }
         BOOST_CATCH(...){
            if(constructed){
               allocator_traits<NodeAlloc>::destroy(this->node_alloc(), container_detail::to_raw_pointer(p));
            }
            this->node_alloc().deallocate_individual(boost::move(mem));
            BOOST_RETHROW
         }
         BOOST_CATCH_END
      }
      return beg;
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
         this->node_alloc().deallocate_individual(boost::move(chain));
   }

   icont_iterator erase_range(const icont_iterator &first, const icont_iterator &last, allocator_v1)
   {  return this->icont().erase_and_dispose(first, last, Destroyer(this->node_alloc())); }

   icont_iterator erase_range(const icont_iterator &first, const icont_iterator &last, allocator_v2)
   {
      allocator_multialloc_chain_node_deallocator<NodeAlloc> chain_holder(this->node_alloc());
      return this->icont().erase_and_dispose(first, last, chain_holder.get_chain_builder());
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
      members_holder(BOOST_FWD_REF(ConvertibleToAlloc) c2alloc, const Pred &c)
         :  NodeAlloc(boost::forward<ConvertibleToAlloc>(c2alloc))
         , m_icont(typename ICont::value_compare(c))
      {}

      explicit members_holder(const Pred &c)
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
