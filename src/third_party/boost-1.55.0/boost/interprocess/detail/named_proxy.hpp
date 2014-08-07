//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_NAMED_PROXY_HPP
#define BOOST_INTERPROCESS_NAMED_PROXY_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>

#include <new>
#include <iterator>
#include <boost/interprocess/detail/in_place_interface.hpp>
#include <boost/interprocess/detail/mpl.hpp>

#ifndef BOOST_INTERPROCESS_PERFECT_FORWARDING
#include <boost/interprocess/detail/preprocessor.hpp>
#else
#include <boost/move/move.hpp>
#include <boost/interprocess/detail/variadic_templates_tools.hpp>
#endif   //#ifdef BOOST_INTERPROCESS_PERFECT_FORWARDING

//!\file
//!Describes a proxy class that implements named allocation syntax.

namespace boost {
namespace interprocess {
namespace ipcdetail {

#ifdef BOOST_INTERPROCESS_PERFECT_FORWARDING

template<class T, bool is_iterator, class ...Args>
struct CtorNArg : public placement_destroy<T>
{
   typedef bool_<is_iterator> IsIterator;
   typedef CtorNArg<T, is_iterator, Args...> self_t;
   typedef typename build_number_seq<sizeof...(Args)>::type index_tuple_t;

   self_t& operator++()
   {
      this->do_increment(IsIterator(), index_tuple_t());
      return *this;
   }

   self_t  operator++(int) {  return ++*this;   *this;  }

   CtorNArg(Args && ...args)
      :  args_(args...)
   {}

   virtual void construct_n(void *mem
                     , std::size_t num
                     , std::size_t &constructed)
   {
      T* memory      = static_cast<T*>(mem);
      for(constructed = 0; constructed < num; ++constructed){
         this->construct(memory++, IsIterator(), index_tuple_t());
         this->do_increment(IsIterator(), index_tuple_t());
      }
   }

   private:
   template<int ...IdxPack>
   void construct(void *mem, true_, const index_tuple<IdxPack...>&)
   {  new((void*)mem)T(*boost::forward<Args>(get<IdxPack>(args_))...); }

   template<int ...IdxPack>
   void construct(void *mem, false_, const index_tuple<IdxPack...>&)
   {  new((void*)mem)T(boost::forward<Args>(get<IdxPack>(args_))...); }

   template<int ...IdxPack>
   void do_increment(true_, const index_tuple<IdxPack...>&)
   {
      this->expansion_helper(++get<IdxPack>(args_)...);
   }

   template<class ...ExpansionArgs>
   void expansion_helper(ExpansionArgs &&...)
   {}

   template<int ...IdxPack>
   void do_increment(false_, const index_tuple<IdxPack...>&)
   {}

   tuple<Args&...> args_;
};

//!Describes a proxy class that implements named
//!allocation syntax.
template
   < class SegmentManager  //segment manager to construct the object
   , class T               //type of object to build
   , bool is_iterator      //passing parameters are normal object or iterators?
   >
class named_proxy
{
   typedef typename SegmentManager::char_type char_type;
   const char_type *    mp_name;
   SegmentManager *     mp_mngr;
   mutable std::size_t  m_num;
   const bool           m_find;
   const bool           m_dothrow;

   public:
   named_proxy(SegmentManager *mngr, const char_type *name, bool find, bool dothrow)
      :  mp_name(name), mp_mngr(mngr), m_num(1)
      ,  m_find(find),  m_dothrow(dothrow)
   {}

   template<class ...Args>
   T *operator()(Args &&...args) const
   {
      CtorNArg<T, is_iterator, Args...> &&ctor_obj = CtorNArg<T, is_iterator, Args...>
         (boost::forward<Args>(args)...);
      return mp_mngr->template
         generic_construct<T>(mp_name, m_num, m_find, m_dothrow, ctor_obj);
   }

   //This operator allows --> named_new("Name")[3]; <-- syntax
   const named_proxy &operator[](std::size_t num) const
   {  m_num *= num; return *this;  }
};

#else //#ifdef BOOST_INTERPROCESS_PERFECT_FORWARDING

//!Function object that makes placement new
//!without arguments
template<class T>
struct Ctor0Arg   :  public placement_destroy<T>
{
   typedef Ctor0Arg self_t;

   Ctor0Arg(){}

   self_t& operator++()       {  return *this;  }
   self_t  operator++(int)    {  return *this;  }

   void construct(void *mem)
   {  new((void*)mem)T;  }

   virtual void construct_n(void *mem, std::size_t num, std::size_t &constructed)
   {
      T* memory = static_cast<T*>(mem);
      for(constructed = 0; constructed < num; ++constructed)
         new((void*)memory++)T;
   }
};

////////////////////////////////////////////////////////////////
//    What the macro should generate (n == 2):
//
//    template<class T, bool is_iterator, class P1, class P2>
//    struct Ctor2Arg
//      :  public placement_destroy<T>
//    {
//       typedef bool_<is_iterator> IsIterator;
//       typedef Ctor2Arg self_t;
//
//       void do_increment(false_)
//       { ++m_p1; ++m_p2;  }
//
//       void do_increment(true_){}
//
//       self_t& operator++()
//       {
//          this->do_increment(IsIterator());
//          return *this;
//       }
//
//       self_t  operator++(int) {  return ++*this;   *this;  }
//
//       Ctor2Arg(const P1 &p1, const P2 &p2)
//          : p1((P1 &)p_1), p2((P2 &)p_2) {}
//
//       void construct(void *mem)
//       {  new((void*)object)T(m_p1, m_p2); }
//
//       virtual void construct_n(void *mem
//                                , std::size_t num
//                                , std::size_t &constructed)
//       {
//          T* memory      = static_cast<T*>(mem);
//          for(constructed = 0; constructed < num; ++constructed){
//             this->construct(memory++, IsIterator());
//             this->do_increment(IsIterator());
//          }
//       }
//
//       private:
//       void construct(void *mem, true_)
//       {  new((void*)mem)T(*m_p1, *m_p2); }
//
//       void construct(void *mem, false_)
//       {  new((void*)mem)T(m_p1, m_p2); }
//
//       P1 &m_p1; P2 &m_p2;
//    };
////////////////////////////////////////////////////////////////

//Note:
//We define template parameters as const references to
//be able to bind temporaries. After that we will un-const them.
//This cast is ugly but it is necessary until "perfect forwarding"
//is achieved in C++0x. Meanwhile, if we want to be able to
//bind lvalues with non-const references, we have to be ugly
#define BOOST_PP_LOCAL_MACRO(n)                                            \
   template<class T, bool is_iterator, BOOST_PP_ENUM_PARAMS(n, class P) >  \
   struct BOOST_PP_CAT(BOOST_PP_CAT(Ctor, n), Arg)                         \
      :  public placement_destroy<T>                                       \
   {                                                                       \
      typedef bool_<is_iterator> IsIterator;                               \
      typedef BOOST_PP_CAT(BOOST_PP_CAT(Ctor, n), Arg) self_t;             \
                                                                           \
      void do_increment(true_)                                             \
         { BOOST_PP_ENUM(n, BOOST_INTERPROCESS_PP_PARAM_INC, _); }         \
                                                                           \
      void do_increment(false_){}                                          \
                                                                           \
      self_t& operator++()                                                 \
      {                                                                    \
         this->do_increment(IsIterator());                                 \
         return *this;                                                     \
      }                                                                    \
                                                                           \
      self_t  operator++(int) {  return ++*this;   *this;  }               \
                                                                           \
      BOOST_PP_CAT(BOOST_PP_CAT(Ctor, n), Arg)                             \
         ( BOOST_PP_ENUM(n, BOOST_INTERPROCESS_PP_PARAM_LIST, _) )        \
         : BOOST_PP_ENUM(n, BOOST_INTERPROCESS_PP_PARAM_INIT, _) {}       \
                                                                           \
      virtual void construct_n(void *mem                                   \
                        , std::size_t num                                  \
                        , std::size_t &constructed)                        \
      {                                                                    \
         T* memory      = static_cast<T*>(mem);                            \
         for(constructed = 0; constructed < num; ++constructed){           \
            this->construct(memory++, IsIterator());                       \
            this->do_increment(IsIterator());                              \
         }                                                                 \
      }                                                                    \
                                                                           \
      private:                                                             \
      void construct(void *mem, true_)                                     \
      {                                                                    \
         new((void*)mem) T                                                 \
         (BOOST_PP_ENUM(n, BOOST_INTERPROCESS_PP_MEMBER_IT_FORWARD, _));   \
      }                                                                    \
                                                                           \
      void construct(void *mem, false_)                                    \
      {                                                                    \
         new((void*)mem) T                                                 \
            (BOOST_PP_ENUM(n, BOOST_INTERPROCESS_PP_MEMBER_FORWARD, _));   \
      }                                                                    \
                                                                           \
      BOOST_PP_REPEAT(n, BOOST_INTERPROCESS_PP_PARAM_DEFINE, _)            \
   };                                                                      \
//!
#define BOOST_PP_LOCAL_LIMITS (1, BOOST_INTERPROCESS_MAX_CONSTRUCTOR_PARAMETERS)
#include BOOST_PP_LOCAL_ITERATE()

//!Describes a proxy class that implements named
//!allocation syntax.
template
   < class SegmentManager  //segment manager to construct the object
   , class T               //type of object to build
   , bool is_iterator      //passing parameters are normal object or iterators?
   >
class named_proxy
{
   typedef typename SegmentManager::char_type char_type;
   const char_type *    mp_name;
   SegmentManager *     mp_mngr;
   mutable std::size_t  m_num;
   const bool           m_find;
   const bool           m_dothrow;

   public:
   named_proxy(SegmentManager *mngr, const char_type *name, bool find, bool dothrow)
      :  mp_name(name), mp_mngr(mngr), m_num(1)
      ,  m_find(find),  m_dothrow(dothrow)
   {}

   //!makes a named allocation and calls the
   //!default constructor
   T *operator()() const
   {
      Ctor0Arg<T> ctor_obj;
      return mp_mngr->template
         generic_construct<T>(mp_name, m_num, m_find, m_dothrow, ctor_obj);
   }
   //!

   #define BOOST_PP_LOCAL_MACRO(n)                                               \
      template<BOOST_PP_ENUM_PARAMS(n, class P)>                                 \
      T *operator()(BOOST_PP_ENUM (n, BOOST_INTERPROCESS_PP_PARAM_LIST, _)) const\
      {                                                                          \
         typedef BOOST_PP_CAT(BOOST_PP_CAT(Ctor, n), Arg)                        \
            <T, is_iterator, BOOST_PP_ENUM_PARAMS(n, P)>                         \
            ctor_obj_t;                                                          \
         ctor_obj_t ctor_obj                                                     \
            (BOOST_PP_ENUM(n, BOOST_INTERPROCESS_PP_PARAM_FORWARD, _));          \
         return mp_mngr->template generic_construct<T>                           \
            (mp_name, m_num, m_find, m_dothrow, ctor_obj);                       \
      }                                                                          \
   //!

   #define BOOST_PP_LOCAL_LIMITS ( 1, BOOST_INTERPROCESS_MAX_CONSTRUCTOR_PARAMETERS )
   #include BOOST_PP_LOCAL_ITERATE()

   ////////////////////////////////////////////////////////////////////////
   //             What the macro should generate (n == 2)
   ////////////////////////////////////////////////////////////////////////
   //
   // template <class P1, class P2>
   // T *operator()(P1 &p1, P2 &p2) const
   // {
   //    typedef Ctor2Arg
   //       <T, is_iterator, P1, P2>
   //       ctor_obj_t;
   //    ctor_obj_t ctor_obj(p1, p2);
   //
   //    return mp_mngr->template generic_construct<T>
   //       (mp_name, m_num, m_find, m_dothrow, ctor_obj);
   // }
   //
   //////////////////////////////////////////////////////////////////////////

   //This operator allows --> named_new("Name")[3]; <-- syntax
   const named_proxy &operator[](std::size_t num) const
      {  m_num *= num; return *this;  }
};

#endif   //#ifdef BOOST_INTERPROCESS_PERFECT_FORWARDING

}}}   //namespace boost { namespace interprocess { namespace ipcdetail {

#include <boost/interprocess/detail/config_end.hpp>

#endif //#ifndef BOOST_INTERPROCESS_NAMED_PROXY_HPP
