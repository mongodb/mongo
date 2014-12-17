//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2008-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_ADVANCED_INSERT_INT_HPP
#define BOOST_CONTAINER_ADVANCED_INSERT_INT_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include "config_begin.hpp"
#include <boost/container/detail/workaround.hpp>
#include <boost/container/allocator/allocator_traits.hpp>
#include <boost/move/move.hpp>
#include <iterator>  //std::iterator_traits
#include <boost/assert.hpp>

namespace boost { namespace container { namespace container_detail {

//This class will be interface for operations dependent on FwdIt types used advanced_insert_aux_impl
template<class Iterator>
struct advanced_insert_aux_int
{
   typedef typename std::iterator_traits<Iterator>::difference_type difference_type;
   virtual void copy_remaining_to(Iterator p) = 0;
   virtual void uninitialized_copy_remaining_to(Iterator p) = 0;
   virtual void uninitialized_copy_some_and_update(Iterator pos, difference_type division_count, bool first) = 0;
   virtual void copy_some_and_update(Iterator pos, difference_type division_count, bool first) = 0;
   virtual ~advanced_insert_aux_int() {}
};

//This class template will adapt each FwIt types to advanced_insert_aux_int
template<class A, class FwdIt, class Iterator>
struct advanced_insert_aux_proxy
   :  public advanced_insert_aux_int<Iterator>
{
   typedef boost::container::allocator_traits<A> alloc_traits;
   typedef typename allocator_traits<A>::size_type size_type;
   typedef typename allocator_traits<A>::value_type value_type;
   typedef typename advanced_insert_aux_int<Iterator>::difference_type difference_type;

   advanced_insert_aux_proxy(A& a, FwdIt first, FwdIt last)
      :  a_(a), first_(first), last_(last)
   {}

   virtual ~advanced_insert_aux_proxy()
   {}

   virtual void copy_remaining_to(Iterator p)
   {  ::boost::copy_or_move(first_, last_, p);  }

   virtual void uninitialized_copy_remaining_to(Iterator p)
   {  ::boost::container::uninitialized_copy_or_move_alloc(a_, first_, last_, p);  }

   virtual void uninitialized_copy_some_and_update(Iterator pos, difference_type division_count, bool first_n)
   {
      FwdIt mid = first_;
      std::advance(mid, division_count);
      if(first_n){
         ::boost::container::uninitialized_copy_or_move_alloc(a_, first_, mid, pos);
         first_ = mid;
      }
      else{
         ::boost::container::uninitialized_copy_or_move_alloc(a_, mid, last_, pos);
         last_ = mid;
      }
   }

   virtual void copy_some_and_update(Iterator pos, difference_type division_count, bool first_n)
   {
      FwdIt mid = first_;
      std::advance(mid, division_count);
      if(first_n){
         ::boost::copy_or_move(first_, mid, pos);
         first_ = mid;
      }
      else{
         ::boost::copy_or_move(mid, last_, pos);
         last_ = mid;
      }
   }
   A &a_;
   FwdIt first_, last_;
};

//This class template will adapt default construction insertions to advanced_insert_aux_int
template<class A, class Iterator>
struct default_construct_aux_proxy
   :  public advanced_insert_aux_int<Iterator>
{
   typedef boost::container::allocator_traits<A> alloc_traits;
   typedef typename allocator_traits<A>::size_type size_type;
   typedef typename allocator_traits<A>::value_type value_type;
   typedef typename advanced_insert_aux_int<Iterator>::difference_type difference_type;

   default_construct_aux_proxy(A &a, size_type count)
      :  a_(a), count_(count)
   {}

   virtual ~default_construct_aux_proxy()
   {}

   virtual void copy_remaining_to(Iterator)
   {  //This should never be called with any count
      BOOST_ASSERT(count_ == 0);
   }

   virtual void uninitialized_copy_remaining_to(Iterator p)
   {  this->priv_uninitialized_copy(p, count_); }

   virtual void uninitialized_copy_some_and_update(Iterator pos, difference_type division_count, bool first_n)
   {
      size_type new_count;
      if(first_n){
         new_count = division_count;
      }
      else{
         BOOST_ASSERT(difference_type(count_)>= division_count);
         new_count = count_ - division_count;
      }
      this->priv_uninitialized_copy(pos, new_count);
   }

   virtual void copy_some_and_update(Iterator , difference_type division_count, bool first_n)
   {
      BOOST_ASSERT(count_ == 0);
      size_type new_count;
      if(first_n){
         new_count = division_count;
      }
      else{
         BOOST_ASSERT(difference_type(count_)>= division_count);
         new_count = count_ - division_count;
      }
      //This function should never called with a count different to zero
      BOOST_ASSERT(new_count == 0);
      (void)new_count;
   }

   private:
   void priv_uninitialized_copy(Iterator p, const size_type n)
   {
      BOOST_ASSERT(n <= count_);
      Iterator orig_p = p;
      size_type i = 0;
      try{
         for(; i < n; ++i, ++p){
            alloc_traits::construct(a_, container_detail::to_raw_pointer(&*p));
         }
      }
      catch(...){
         while(i--){
            alloc_traits::destroy(a_, container_detail::to_raw_pointer(&*orig_p++));
         }
         throw;
      }
      count_ -= n;
   }
   A &a_;
   size_type count_;
};

}}}   //namespace boost { namespace container { namespace container_detail {

#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

#include <boost/container/detail/variadic_templates_tools.hpp>
#include <boost/container/detail/stored_ref.hpp>
#include <boost/move/move.hpp>
#include <typeinfo>
//#include <iostream> //For debugging purposes

namespace boost {
namespace container {
namespace container_detail {


//This class template will adapt emplace construction insertions of movable types 
//to advanced_insert_aux_int
template<class A, class Iterator, class ...Args>
struct advanced_insert_aux_non_movable_emplace
   :  public advanced_insert_aux_int<Iterator>
{
   typedef boost::container::allocator_traits<A> alloc_traits;
   typedef typename allocator_traits<A>::size_type size_type;
   typedef typename allocator_traits<A>::value_type value_type;
   typedef typename advanced_insert_aux_int<Iterator>::difference_type difference_type;
   typedef typename build_number_seq<sizeof...(Args)>::type             index_tuple_t;

   explicit advanced_insert_aux_non_movable_emplace(A &a, Args&&... args)
      : a_(a)
      , args_(args...)
      , used_(false)
   {}

   ~advanced_insert_aux_non_movable_emplace()
   {}

   virtual void copy_remaining_to(Iterator)
   //This code can't be called since value_type is not movable or copyable
   {  BOOST_ASSERT(false);   }

   virtual void uninitialized_copy_remaining_to(Iterator p)
   {  this->priv_uninitialized_copy_remaining_to(index_tuple_t(), p);   }

   virtual void uninitialized_copy_some_and_update(Iterator p, difference_type division_count, bool first_n)
   {  this->priv_uninitialized_copy_some_and_update(index_tuple_t(), p, division_count, first_n);  }

   virtual void copy_some_and_update(Iterator, difference_type, bool )
   //This code can't be called since value_type is not movable or copyable
   {  BOOST_ASSERT(false);   }

   private:
   template<int ...IdxPack>
   void priv_uninitialized_copy_some_and_update(const index_tuple<IdxPack...>&, Iterator p, difference_type division_count, bool first_n)
   {
      BOOST_ASSERT(division_count <=1);
      if((first_n && division_count == 1) || (!first_n && division_count == 0)){
         if(!used_){
            alloc_traits::construct( a_
                                   , container_detail::to_raw_pointer(&*p)
                                   , ::boost::container::container_detail::
                                       stored_ref<Args>::forward(get<IdxPack>(args_))...
                                   );
            used_ = true;
         }
      }
   }

   template<int ...IdxPack>
   void priv_uninitialized_copy_remaining_to(const index_tuple<IdxPack...>&, Iterator p)
   {
      if(!used_){
         alloc_traits::construct( a_
                                , container_detail::to_raw_pointer(&*p)
                                , ::boost::container::container_detail::
                                    stored_ref<Args>::forward(get<IdxPack>(args_))...
                                );
         used_ = true;
      }
   }

   protected:
   A &a_;
   tuple<Args&...> args_;
   bool used_;
};

//This class template will adapt emplace construction insertions of movable types 
//to advanced_insert_aux_int
template<class A, class Iterator, class ...Args>
struct advanced_insert_aux_emplace
   :  public advanced_insert_aux_non_movable_emplace<A, Iterator, Args...>
{
   typedef advanced_insert_aux_non_movable_emplace<A, Iterator, Args...> base_t;
   typedef typename base_t::value_type       value_type;
   typedef typename base_t::difference_type  difference_type;
   typedef typename base_t::index_tuple_t    index_tuple_t;

   explicit advanced_insert_aux_emplace(A &a, Args&&... args)
      : base_t(a, boost::forward<Args>(args)...)
   {}

   ~advanced_insert_aux_emplace()
   {}

   //Override only needed functions
   virtual void copy_remaining_to(Iterator p)
   {  this->priv_copy_remaining_to(index_tuple_t(), p);   }

   virtual void copy_some_and_update(Iterator p, difference_type division_count, bool first_n)
   {  this->priv_copy_some_and_update(index_tuple_t(), p, division_count, first_n);  }

   private:
   template<int ...IdxPack>
   void priv_copy_remaining_to(const index_tuple<IdxPack...>&, Iterator p)
   {
      if(!this->used_){
         *p = boost::move(value_type (
            ::boost::container::container_detail::stored_ref<Args>::forward(get<IdxPack>(this->args_))...));
         this->used_ = true;
      }
   }

   template<int ...IdxPack>
   void priv_copy_some_and_update(const index_tuple<IdxPack...>&, Iterator p, difference_type division_count, bool first_n)
   {
      BOOST_ASSERT(division_count <=1);
      if((first_n && division_count == 1) || (!first_n && division_count == 0)){
         if(!this->used_){
            *p = boost::move(value_type(
               ::boost::container::container_detail::stored_ref<Args>::forward(get<IdxPack>(this->args_))...));
            this->used_ = true;
         }
      }
   }
};

}}}   //namespace boost { namespace container { namespace container_detail {

#else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

#include <boost/container/detail/preprocessor.hpp> 
#include <boost/container/detail/value_init.hpp>

namespace boost {
namespace container { 
namespace container_detail {

#define BOOST_PP_LOCAL_MACRO(n)                                                     \
template<class A, class Iterator BOOST_PP_ENUM_TRAILING_PARAMS(n, class P) >        \
struct BOOST_PP_CAT(BOOST_PP_CAT(advanced_insert_aux_non_movable_emplace, n), arg)  \
   :  public advanced_insert_aux_int<Iterator>                                      \
{                                                                                   \
   typedef boost::container::allocator_traits<A> alloc_traits;                      \
   typedef typename allocator_traits<A>::size_type size_type;                       \
   typedef typename allocator_traits<A>::value_type value_type;                     \
   typedef typename advanced_insert_aux_int<Iterator>::difference_type              \
      difference_type;                                                              \
                                                                                    \
   BOOST_PP_CAT(BOOST_PP_CAT(advanced_insert_aux_non_movable_emplace, n), arg)      \
      ( A &a BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _) )          \
      : a_(a)                                                                       \
      , used_(false)                                                                \
      BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_INIT, _)                   \
    {}                                                                              \
                                                                                    \
   virtual void copy_remaining_to(Iterator)                                         \
   {  BOOST_ASSERT(false);   }                                                      \
                                                                                    \
   virtual void uninitialized_copy_remaining_to(Iterator p)                         \
   {                                                                                \
      if(!used_){                                                                   \
         alloc_traits::construct                                                    \
            ( a_                                                                    \
            , container_detail::to_raw_pointer(&*p)                                 \
            BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_MEMBER_FORWARD, _)         \
            );                                                                      \
         used_ = true;                                                              \
      }                                                                             \
   }                                                                                \
                                                                                    \
   virtual void uninitialized_copy_some_and_update                                  \
      (Iterator p, difference_type division_count, bool first_n)                    \
   {                                                                                \
      BOOST_ASSERT(division_count <=1);                                             \
      if((first_n && division_count == 1) || (!first_n && division_count == 0)){    \
         if(!used_){                                                                \
            alloc_traits::construct                                                 \
               ( a_                                                                 \
               , container_detail::to_raw_pointer(&*p)                              \
               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_MEMBER_FORWARD, _)      \
               );                                                                   \
            used_ = true;                                                           \
         }                                                                          \
      }                                                                             \
   }                                                                                \
                                                                                    \
   virtual void copy_some_and_update(Iterator, difference_type, bool)               \
   {  BOOST_ASSERT(false);   }                                                      \
                                                                                    \
   A &a_;                                                                           \
   bool used_;                                                                      \
   BOOST_PP_REPEAT(n, BOOST_CONTAINER_PP_PARAM_DEFINE, _)                           \
};                                                                                  \
                                                                                    \
template<class A, class Iterator BOOST_PP_ENUM_TRAILING_PARAMS(n, class P) >        \
struct BOOST_PP_CAT(BOOST_PP_CAT(advanced_insert_aux_emplace, n), arg)              \
   : BOOST_PP_CAT(BOOST_PP_CAT(                                                     \
      advanced_insert_aux_non_movable_emplace, n), arg)                             \
         < A, Iterator BOOST_PP_ENUM_TRAILING_PARAMS(n, P) >                        \
{                                                                                   \
   typedef BOOST_PP_CAT(BOOST_PP_CAT(                                               \
      advanced_insert_aux_non_movable_emplace, n), arg)                             \
         <A, Iterator BOOST_PP_ENUM_TRAILING_PARAMS(n, P) > base_t;                 \
   typedef typename base_t::value_type       value_type;                            \
   typedef typename base_t::difference_type  difference_type;                       \
                                                                                    \
   BOOST_PP_CAT(BOOST_PP_CAT(advanced_insert_aux_emplace, n), arg)                  \
      ( A &a BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _) )          \
      : base_t(a BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _) )   \
    {}                                                                              \
                                                                                    \
   virtual void copy_remaining_to(Iterator p)                                       \
   {                                                                                \
      if(!this->used_){                                                             \
         value_type v BOOST_PP_LPAREN_IF(n)                                         \
            BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_MEMBER_FORWARD, _)                  \
            BOOST_PP_RPAREN_IF(n);                                                  \
         *p = boost::move(v);                                                       \
         this->used_ = true;                                                        \
      }                                                                             \
   }                                                                                \
                                                                                    \
   virtual void copy_some_and_update                                                \
      (Iterator p, difference_type division_count, bool first_n)                    \
   {                                                                                \
      BOOST_ASSERT(division_count <=1);                                             \
      if((first_n && division_count == 1) || (!first_n && division_count == 0)){    \
         if(!this->used_){                                                          \
            value_type v BOOST_PP_LPAREN_IF(n)                                      \
                  BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_MEMBER_FORWARD, _)            \
                BOOST_PP_RPAREN_IF(n);                                              \
            *p = boost::move(v);                                                    \
            this->used_ = true;                                                     \
         }                                                                          \
      }                                                                             \
   }                                                                                \
};                                                                                  \
//!

#define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
#include BOOST_PP_LOCAL_ITERATE()

}}}   //namespace boost { namespace container { namespace container_detail {

#endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

#include <boost/container/detail/config_end.hpp>

#endif //#ifndef BOOST_CONTAINER_ADVANCED_INSERT_INT_HPP
