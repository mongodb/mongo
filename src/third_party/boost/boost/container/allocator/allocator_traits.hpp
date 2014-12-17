//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Pablo Halpern 2009. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2011-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_ALLOCATOR_ALLOCATOR_TRAITS_HPP
#define BOOST_CONTAINER_ALLOCATOR_ALLOCATOR_TRAITS_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/container/allocator/memory_util.hpp>
#include <boost/type_traits/integral_constant.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/move/move.hpp>
#include <limits> //numeric_limits<>::max()
#include <new>    //placement new
#include <memory> //std::allocator
#include <boost/container/detail/preprocessor.hpp>

///@cond

namespace boost {
namespace container {
namespace container_detail {

//workaround needed for C++03 compilers with no construct()
//supporting rvalue references
template<class A>
struct is_std_allocator
{  static const bool value = false; };

template<class T>
struct is_std_allocator< std::allocator<T> >
{  static const bool value = true; };

}  //namespace container_detail {

///@endcond

template <typename Alloc>
struct allocator_traits
{
   //allocator_type
   typedef Alloc allocator_type;
   //value_type
   typedef typename Alloc::value_type         value_type;

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      //!Alloc::pointer if such a type exists; otherwise, value_type*
      //!
      typedef unspecified pointer;
      //!Alloc::const_pointer if such a type exists ; otherwise, pointer_traits<pointer>::rebind<const
      //!
      typedef unspecified const_pointer;
      //!Non-standard extension
      //!Alloc::reference if such a type exists; otherwise, value_type&
      typedef unspecified pointer;
      //!Non-standard extension
      //!Alloc::const_reference if such a type exists ; otherwise, const value_type&
      typedef unspecified const_pointer;
      //!Alloc::void_pointer if such a type exists ; otherwise, pointer_traits<pointer>::rebind<void>.
      //!
      typedef unspecified void_pointer;
      //!Alloc::const_void_pointer if such a type exists ; otherwis e, pointer_traits<pointer>::rebind<const
      //!
      typedef unspecified const_void_pointer;
      //!Alloc::difference_type if such a type exists ; otherwise, pointer_traits<pointer>::difference_type.
      //!
      typedef unspecified difference_type;
      //!Alloc::size_type if such a type exists ; otherwise, make_unsigned<difference_type>::type
      //!
      typedef unspecified size_type;
      //!Alloc::propagate_on_container_copy_assignment if such a type exists, otherwise an integral_constant
      //!type with internal constant static member <pre>value</pre> == false.
      typedef unspecified propagate_on_container_copy_assignment;
      //!Alloc::propagate_on_container_move_assignment if such a type exists, otherwise an integral_constant
      //!type with internal constant static member <pre>value</pre> == false.
      typedef unspecified propagate_on_container_move_assignment;
      //!Alloc::propagate_on_container_swap if such a type exists, otherwise an integral_constant
      //!type with internal constant static member <pre>value</pre> == false.
      typedef unspecified propagate_on_container_swap;
      //!Defines an allocator: Alloc::rebind<T>::other if such a type exists; otherwise, Alloc<T, Args>
      //!if Alloc is a class template instantiation of the form Alloc<U, Args>, where Args is zero or 
      //!more type arguments ; otherwise, the instantiation of rebind_alloc is ill-formed.
      //!
      //!In C++03 compilers <pre>rebind_alloc</pre> is a struct derived from an allocator
      //!deduced by previously detailed rules.
      template <class T> using rebind_alloc = unspecified;

      //!In C++03 compilers <pre>rebind_traits</pre> is a struct derived from
      //!<pre>allocator_traits<OtherAlloc><pre>, where `OtherAlloc` is
      //!the allocator deduced by rules explained in `rebind_alloc`.
      template <class T> using rebind_traits = allocator_traits<rebind_alloc<T> >;

      //!Non-standard extension: Portable allocator rebind for C++03 and C++11 compilers.
      //!`type` is an allocator related to Alloc deduced deduced by rules explained in `rebind_alloc`.
      template <class T>
      struct portable_rebind_alloc
      {  typedef unspecified_type type;  };
   #else
      //pointer
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, Alloc,
         pointer, value_type*)
            pointer;
      //const_pointer
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_EVAL_DEFAULT(boost::container::container_detail::, Alloc, 
         const_pointer, typename boost::intrusive::pointer_traits<pointer>::template
            rebind_pointer<const value_type>)
               const_pointer;
      //reference
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, Alloc,
         reference, typename container_detail::unvoid<value_type>::type&)
            reference;
      //const_reference
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, Alloc, 
         const_reference, const typename container_detail::unvoid<value_type>::type&)
               const_reference;
      //void_pointer
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_EVAL_DEFAULT(boost::container::container_detail::, Alloc, 
         void_pointer, typename boost::intrusive::pointer_traits<pointer>::template
            rebind_pointer<void>)
               void_pointer;
      //const_void_pointer
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_EVAL_DEFAULT(boost::container::container_detail::, Alloc,
         const_void_pointer, typename boost::intrusive::pointer_traits<pointer>::template
            rebind_pointer<const void>)
               const_void_pointer;
      //difference_type
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, Alloc,
         difference_type, std::ptrdiff_t)
            difference_type;
      //size_type
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, Alloc,
         size_type, std::size_t)
            size_type;
      //propagate_on_container_copy_assignment
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, Alloc,
         propagate_on_container_copy_assignment, boost::false_type)
            propagate_on_container_copy_assignment;
      //propagate_on_container_move_assignment
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, Alloc,
         propagate_on_container_move_assignment, boost::false_type)
            propagate_on_container_move_assignment;
      //propagate_on_container_swap
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT(boost::container::container_detail::, Alloc,
         propagate_on_container_swap, boost::false_type)
            propagate_on_container_swap;

      #if !defined(BOOST_NO_TEMPLATE_ALIASES)
         //C++11
         template <typename T> using rebind_alloc  = boost::intrusive::detail::type_rebinder<Alloc, T>::type;
         template <typename T> using rebind_traits = allocator_traits< rebind_alloc<T> >;
      #else    //!defined(BOOST_NO_TEMPLATE_ALIASES)
         //Some workaround for C++03 or C++11 compilers with no template aliases
         template <typename T>
         struct rebind_alloc : boost::intrusive::detail::type_rebinder<Alloc,T>::type
         {
            typedef typename boost::intrusive::detail::type_rebinder<Alloc,T>::type Base;
            #if !defined(BOOST_NO_VARIADIC_TEMPLATES)
            template <typename... Args>
            rebind_alloc(Args&&... args)
               : Base(boost::forward<Args>(args)...)
            {}
            #else    //!defined(BOOST_NO_VARIADIC_TEMPLATES)
            #define BOOST_PP_LOCAL_MACRO(n)                                                        \
            BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >) \
            rebind_alloc(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                       \
               : Base(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _))                       \
            {}                                                                                     \
            //
            #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
            #include BOOST_PP_LOCAL_ITERATE()
            #endif   //!defined(BOOST_NO_VARIADIC_TEMPLATES)
         };

         template <typename T>
         struct rebind_traits
            : allocator_traits<typename boost::intrusive::detail::type_rebinder<Alloc, T>::type>
         {};
      #endif   //!defined(BOOST_NO_TEMPLATE_ALIASES)
      template <class T>
      struct portable_rebind_alloc
      {  typedef typename boost::intrusive::detail::type_rebinder<Alloc, T>::type type;  };
   #endif   //BOOST_CONTAINER_DOXYGEN_INVOKED

   //!<b>Returns</b>: a.allocate(n)
   //!
   static pointer allocate(Alloc &a, size_type n)
   {  return a.allocate(n);  }

   //!<b>Returns</b>: a.deallocate(p, n)
   //!
   //!<b>Throws</b>: Nothing
   static void deallocate(Alloc &a, pointer p, size_type n)
   {  return a.deallocate(p, n);  }

   //!<b>Effects</b>: calls `a.construct(p, std::forward<Args>(args)...)` if that call is well-formed;
   //!otherwise, invokes `::new (static_cast<void*>(p)) T(std::forward<Args>(args)...)`
   static pointer allocate(Alloc &a, size_type n, const_void_pointer p)
   {
      const bool value = boost::container::container_detail::
         has_member_function_callable_with_allocate
            <Alloc, const size_type, const const_void_pointer>::value;
      ::boost::integral_constant<bool, value> flag;
      return allocator_traits::priv_allocate(flag, a, n, p);
   }

   //!<b>Effects</b>: calls a.destroy(p) if that call is well-formed;
   //!otherwise, invokes `p->~T()`.
   template<class T>
   static void destroy(Alloc &a, T*p)
   {
      typedef T* destroy_pointer;
      const bool value = boost::container::container_detail::
         has_member_function_callable_with_destroy
            <Alloc, const destroy_pointer>::value;
      ::boost::integral_constant<bool, value> flag;
      allocator_traits::priv_destroy(flag, a, p);
   }

   //!<b>Returns</b>: a.max_size() if that expression is well-formed; otherwise,
   //!`numeric_limits<size_type>::max()`.
   static size_type max_size(const Alloc &a)
   {
      const bool value = boost::container::container_detail::
         has_member_function_callable_with_max_size
            <const Alloc>::value;
      ::boost::integral_constant<bool, value> flag;
      return allocator_traits::priv_max_size(flag, a);
   }

   //!<b>Returns</b>: a.select_on_container_copy_construction() if that expres sion is well- formed;
   //!otherwise, a.
   static Alloc select_on_container_copy_construction(const Alloc &a)
   {
      const bool value = boost::container::container_detail::
         has_member_function_callable_with_select_on_container_copy_construction
            <const Alloc>::value;
      ::boost::integral_constant<bool, value> flag;
      return allocator_traits::priv_select_on_container_copy_construction(flag, a);
   }

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      //!Effects: calls a.construct(p, std::forward<Args>(args)...) if that call is well-formed; 
      //!otherwise, invokes `::new (static_cast<void*>(p)) T(std::forward<Args>(args)...)`
      template <class T, class ...Args>
      static void construct(Alloc & a, T* p, Args&&... args)
      {
         ::boost::integral_constant<bool, container_detail::is_std_allocator<Alloc>::value> flag;
         allocator_traits::priv_construct(flag, a, p, ::boost::forward<Args>(args)...);
      }
   #endif

   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      private:
      static pointer priv_allocate(boost::true_type, Alloc &a, size_type n, const_void_pointer p)
      {  return a.allocate(n, p);  }

      static pointer priv_allocate(boost::false_type, Alloc &a, size_type n, const_void_pointer)
      {  return allocator_traits::allocate(a, n);  }

      template<class T>
      static void priv_destroy(boost::true_type, Alloc &a, T* p)
      {  a.destroy(p);  }

      template<class T>
      static void priv_destroy(boost::false_type, Alloc &, T* p)
      {  p->~T(); (void)p;  }

      static size_type priv_max_size(boost::true_type, const Alloc &a)
      {  return a.max_size();  }

      static size_type priv_max_size(boost::false_type, const Alloc &)
      {  return (std::numeric_limits<size_type>::max)();  }

      static Alloc priv_select_on_container_copy_construction(boost::true_type, const Alloc &a)
      {  return a.select_on_container_copy_construction();  }

      static Alloc priv_select_on_container_copy_construction(boost::false_type, const Alloc &a)
      {  return a;  }

      #if defined(BOOST_CONTAINER_PERFECT_FORWARDING)
         template<class T, class ...Args>
         static void priv_construct(boost::false_type, Alloc &a, T *p, Args && ...args)                    
         {                                                                                                  
            const bool value = boost::container::container_detail::
                  has_member_function_callable_with_construct
                     < Alloc, T*, Args... >::value;
            ::boost::integral_constant<bool, value> flag;
            priv_construct_dispatch2(flag, a, p, ::boost::forward<Args>(args)...);
         }

         template<class T, class ...Args>
         static void priv_construct(boost::true_type, Alloc &a, T *p, Args && ...args)
         {
            priv_construct_dispatch2(boost::false_type(), a, p, ::boost::forward<Args>(args)...);
         }

         template<class T, class ...Args>
         static void priv_construct_dispatch2(boost::true_type, Alloc &a, T *p, Args && ...args)
         {  a.construct( p, ::boost::forward<Args>(args)...);  }

         template<class T, class ...Args>
         static void priv_construct_dispatch2(boost::false_type, Alloc &, T *p, Args && ...args)
         {  ::new((void*)p) T(::boost::forward<Args>(args)...); }
      #else
         public:
         #define BOOST_PP_LOCAL_MACRO(n)                                                              \
         template<class T BOOST_PP_ENUM_TRAILING_PARAMS(n, class P) >                                 \
         static void construct(Alloc &a, T *p                                                         \
                              BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))            \
         {                                                                                            \
            ::boost::integral_constant<bool, container_detail::is_std_allocator<Alloc>::value> flag;  \
            allocator_traits::priv_construct(flag, a, p                                               \
               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));                       \
         }                                                                                            \
         //
         #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
         #include BOOST_PP_LOCAL_ITERATE()
      
         private:
         #define BOOST_PP_LOCAL_MACRO(n)                                                                    \
         template<class T  BOOST_PP_ENUM_TRAILING_PARAMS(n, class P) >                                      \
         static void priv_construct(boost::false_type, Alloc &a, T *p                                       \
                        BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST,_))                         \
         {                                                                                                  \
            const bool value =                                                                              \
               boost::container::container_detail::has_member_function_callable_with_construct              \
                     < Alloc, T* BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_FWD_TYPE, _) >::value;        \
            ::boost::integral_constant<bool, value> flag;                                                   \
            priv_construct_dispatch2(flag, a, p                                                             \
               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _) );                            \
         }                                                                                                  \
                                                                                                            \
         template<class T  BOOST_PP_ENUM_TRAILING_PARAMS(n, class P) >                                      \
         static void priv_construct(boost::true_type, Alloc &a, T *p                                        \
                        BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST,_))                         \
         {                                                                                                  \
            priv_construct_dispatch2(boost::false_type(), a, p                                              \
               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _) );                            \
         }                                                                                                  \
                                                                                                            \
         template<class T  BOOST_PP_ENUM_TRAILING_PARAMS(n, class P) >                                      \
         static void priv_construct_dispatch2(boost::true_type, Alloc &a, T *p                              \
                        BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST,_))                         \
         {  a.construct( p BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _) );  }             \
                                                                                                            \
         template<class T  BOOST_PP_ENUM_TRAILING_PARAMS(n, class P) >                                      \
         static void priv_construct_dispatch2(boost::false_type, Alloc &, T *p                              \
                        BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _) )                       \
         {  ::new((void*)p) T(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }                     \
         //
         #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
         #include BOOST_PP_LOCAL_ITERATE()
      #endif   //BOOST_CONTAINER_PERFECT_FORWARDING
   #endif   //#if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   ///@endcond
};

}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif // ! defined(BOOST_CONTAINER_ALLOCATOR_ALLOCATOR_TRAITS_HPP)
