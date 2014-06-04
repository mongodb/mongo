//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Pablo Halpern 2009. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2011-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_ALLOCATOR_SCOPED_ALLOCATOR_HPP
#define BOOST_CONTAINER_ALLOCATOR_SCOPED_ALLOCATOR_HPP

#if defined (_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/scoped_allocator_fwd.hpp>
#include <boost/type_traits/integral_constant.hpp>
#include <boost/container/allocator_traits.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/utilities.hpp>
#include <utility>
#include <boost/container/detail/pair.hpp>
#include <boost/move/utility.hpp>
#include <boost/detail/no_exceptions_support.hpp>

namespace boost { namespace container {

//! <b>Remark</b>: if a specialization is derived from true_type, indicates that T may be constructed
//! with an allocator as its last constructor argument.  Ideally, all constructors of T (including the
//! copy and move constructors) should have a variant that accepts a final argument of
//! allocator_type.
//!
//! <b>Requires</b>: if a specialization is derived from true_type, T must have a nested type,
//! allocator_type and at least one constructor for which allocator_type is the last
//! parameter.  If not all constructors of T can be called with a final allocator_type argument,
//! and if T is used in a context where a container must call such a constructor, then the program is
//! ill-formed.
//!
//! [Example:
//!  template <class T, class Allocator = allocator<T> >  
//!  class Z {
//!    public:
//!      typedef Allocator allocator_type;
//!
//!    // Default constructor with optional allocator suffix
//!    Z(const allocator_type& a = allocator_type());
//!
//!    // Copy constructor and allocator-extended copy constructor
//!    Z(const Z& zz);
//!    Z(const Z& zz, const allocator_type& a);
//! };
//!
//! // Specialize trait for class template Z
//! template <class T, class Allocator = allocator<T> >
//! struct constructible_with_allocator_suffix<Z<T,Allocator> > 
//!      : ::boost::true_type { };
//! -- end example]
//!
//! <b>Note</b>: This trait is a workaround inspired by "N2554: The Scoped Allocator Model (Rev 2)"
//! (Pablo Halpern, 2008-02-29) to backport the scoped allocator model to C++03, as
//! in C++03 there is no mechanism to detect if a type can be constructed from arbitrary arguments.
//! Applications aiming portability with several compilers should always define this trait.
//!
//! In conforming C++11 compilers or compilers supporting SFINAE expressions
//! (when BOOST_NO_SFINAE_EXPR is NOT defined), this trait is ignored and C++11 rules will be used
//! to detect if a type should be constructed with suffix or prefix allocator arguments.
template <class T>
struct constructible_with_allocator_suffix
   : ::boost::false_type
{};

//! <b>Remark</b>: if a specialization is derived from true_type, indicates that T may be constructed
//! with allocator_arg and T::allocator_type as its first two constructor arguments. 
//! Ideally, all constructors of T (including the copy and move constructors) should have a variant
//! that accepts these two initial arguments.
//!
//! <b>Requires</b>: if a specialization is derived from true_type, T must have a nested type,
//! allocator_type and at least one constructor for which allocator_arg_t is the first
//! parameter and allocator_type is the second parameter.  If not all constructors of T can be
//! called with these initial arguments, and if T is used in a context where a container must call such
//! a constructor, then the program is ill-formed.
//!
//! [Example:
//! template <class T, class Allocator = allocator<T> >
//! class Y {
//!    public:
//!       typedef Allocator allocator_type;
//! 
//!       // Default constructor with and allocator-extended default constructor
//!       Y();
//!       Y(allocator_arg_t, const allocator_type& a);
//! 
//!       // Copy constructor and allocator-extended copy constructor
//!       Y(const Y& yy);
//!       Y(allocator_arg_t, const allocator_type& a, const Y& yy);
//! 
//!       // Variadic constructor and allocator-extended variadic constructor
//!       template<class ...Args> Y(Args&& args...);
//!       template<class ...Args> 
//!       Y(allocator_arg_t, const allocator_type& a, Args&&... args);
//! };
//! 
//! // Specialize trait for class template Y
//! template <class T, class Allocator = allocator<T> >
//! struct constructible_with_allocator_prefix<Y<T,Allocator> > 
//!       : ::boost::true_type { };
//! 
//! -- end example]
//!
//! <b>Note</b>: This trait is a workaround inspired by "N2554: The Scoped Allocator Model (Rev 2)"
//! (Pablo Halpern, 2008-02-29) to backport the scoped allocator model to C++03, as
//! in C++03 there is no mechanism to detect if a type can be constructed from arbitrary arguments.
//! Applications aiming portability with several compilers should always define this trait.
//!
//! In conforming C++11 compilers or compilers supporting SFINAE expressions
//! (when BOOST_NO_SFINAE_EXPR is NOT defined), this trait is ignored and C++11 rules will be used
//! to detect if a type should be constructed with suffix or prefix allocator arguments.
template <class T>
struct constructible_with_allocator_prefix
    : ::boost::false_type
{};

///@cond

namespace container_detail {

template<typename T, typename Alloc>
struct uses_allocator_imp
{
   // Use SFINAE (Substitution Failure Is Not An Error) to detect the
   // presence of an 'allocator_type' nested type convertilble from Alloc.

   private:
   // Match this function if TypeT::allocator_type exists and is
   // implicitly convertible from Alloc
   template <typename U>
   static char test(int, typename U::allocator_type);

   // Match this function if TypeT::allocator_type does not exist or is
   // not convertible from Alloc.
   template <typename U>
   static int test(LowPriorityConversion<int>, LowPriorityConversion<Alloc>);

   static Alloc alloc;  // Declared but not defined

   public:
   enum { value = sizeof(test<T>(0, alloc)) == sizeof(char) };
};

}  //namespace container_detail {

///@endcond

//! <b>Remark</b>: Automatically detects if T has a nested allocator_type that is convertible from
//! Alloc. Meets the BinaryTypeTrait requirements ([meta.rqmts] 20.4.1). A program may
//! specialize this type to derive from true_type for a T of user-defined type if T does not
//! have a nested allocator_type but is nonetheless constructible using the specified Alloc.
//!
//! <b>Result</b>: derived from true_type if Convertible<Alloc,T::allocator_type> and
//! derived from false_type otherwise.
template <typename T, typename Alloc>
struct uses_allocator
   : boost::integral_constant<bool, container_detail::uses_allocator_imp<T, Alloc>::value>
{};

///@cond

namespace container_detail {

template <typename Alloc>
struct is_scoped_allocator_imp
{
   template <typename T>
   static char test(int, typename T::outer_allocator_type*);

   template <typename T>
   static int test(LowPriorityConversion<int>, void*);

   static const bool value = (sizeof(char) == sizeof(test<Alloc>(0, 0)));
};

template<class MaybeScopedAlloc, bool = is_scoped_allocator_imp<MaybeScopedAlloc>::value >
struct outermost_allocator_type_impl
{
   typedef typename MaybeScopedAlloc::outer_allocator_type outer_type;
   typedef typename outermost_allocator_type_impl<outer_type>::type type;
};

template<class MaybeScopedAlloc>
struct outermost_allocator_type_impl<MaybeScopedAlloc, false>
{
   typedef MaybeScopedAlloc type;
};

template<class MaybeScopedAlloc, bool = is_scoped_allocator_imp<MaybeScopedAlloc>::value >
struct outermost_allocator_imp
{
   typedef MaybeScopedAlloc type;

   static type &get(MaybeScopedAlloc &a)
   {  return a;  }

   static const type &get(const MaybeScopedAlloc &a)
   {  return a;  }
};

template<class MaybeScopedAlloc>
struct outermost_allocator_imp<MaybeScopedAlloc, true>
{
   typedef typename MaybeScopedAlloc::outer_allocator_type outer_type;
   typedef typename outermost_allocator_type_impl<outer_type>::type type;

   static type &get(MaybeScopedAlloc &a)
   {  return outermost_allocator_imp<outer_type>::get(a.outer_allocator());  }

   static const type &get(const MaybeScopedAlloc &a)
   {  return outermost_allocator_imp<outer_type>::get(a.outer_allocator());  }
};

}  //namespace container_detail {

template <typename Alloc>
struct is_scoped_allocator
   : boost::integral_constant<bool, container_detail::is_scoped_allocator_imp<Alloc>::value>
{};

template <typename Alloc>
struct outermost_allocator
   : container_detail::outermost_allocator_imp<Alloc>
{};

template <typename Alloc>
typename container_detail::outermost_allocator_imp<Alloc>::type &
   get_outermost_allocator(Alloc &a)
{  return container_detail::outermost_allocator_imp<Alloc>::get(a);   }

template <typename Alloc>
const typename container_detail::outermost_allocator_imp<Alloc>::type &
   get_outermost_allocator(const Alloc &a)
{  return container_detail::outermost_allocator_imp<Alloc>::get(a);   }

namespace container_detail {

// Check if we can detect is_convertible using advanced SFINAE expressions
#if !defined(BOOST_NO_SFINAE_EXPR)

   //! Code inspired by Mathias Gaunard's is_convertible.cpp found in the Boost mailing list
   //! http://boost.2283326.n4.nabble.com/type-traits-is-constructible-when-decltype-is-supported-td3575452.html
   //! Thanks Mathias!

   //With variadic templates, we need a single class to implement the trait
   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   template<class T, class ...Args>
   struct is_constructible_impl
   {
      typedef char yes_type;
      struct no_type
      { char padding[2]; };

      template<std::size_t N>
      struct dummy;

      template<class X>
      static yes_type test(dummy<sizeof(X(boost::move_detail::declval<Args>()...))>*);

      template<class X>
      static no_type test(...);

      static const bool value = sizeof(test<T>(0)) == sizeof(yes_type);
   };

   template<class T, class ...Args>
   struct is_constructible
      : boost::integral_constant<bool, is_constructible_impl<T, Args...>::value>
   {};

   template <class T, class InnerAlloc, class ...Args>
   struct is_constructible_with_allocator_prefix
      : is_constructible<T, allocator_arg_t, InnerAlloc, Args...>
   {};

   #else // #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   //Without variadic templates, we need to use de preprocessor to generate
   //some specializations.

   #define BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS \
      BOOST_PP_ADD(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, 3)
   //!

   //Generate N+1 template parameters so that we can specialize N
   template<class T
            BOOST_PP_ENUM_TRAILING( BOOST_PP_ADD(BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS, 1)
                                 , BOOST_CONTAINER_PP_TEMPLATE_PARAM_WITH_DEFAULT
                                 , void)
         >
   struct is_constructible_impl;

   //Generate N specializations, from 0 to
   //BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS parameters
   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   template<class T BOOST_PP_ENUM_TRAILING_PARAMS(n, class P)>                                     \
   struct is_constructible_impl                                                                    \
      <T BOOST_PP_ENUM_TRAILING_PARAMS(n, P)                                                       \
         BOOST_PP_ENUM_TRAILING                                                                    \
            ( BOOST_PP_SUB(BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS, n)                     \
            , BOOST_CONTAINER_PP_IDENTITY, void)                                                   \
      , void>                                                                                      \
   {                                                                                               \
      typedef char yes_type;                                                                       \
      struct no_type                                                                               \
      { char padding[2]; };                                                                        \
                                                                                                   \
      template<std::size_t N>                                                                      \
      struct dummy;                                                                                \
                                                                                                   \
      template<class X>                                                                            \
      static yes_type test(dummy<sizeof(X(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_DECLVAL, ~)))>*);    \
                                                                                                   \
      template<class X>                                                                            \
      static no_type test(...);                                                                    \
                                                                                                   \
      static const bool value = sizeof(test<T>(0)) == sizeof(yes_type);                            \
   };                                                                                              \
   //!

   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   //Finally just inherit from the implementation to define he trait
   template< class T
           BOOST_PP_ENUM_TRAILING( BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS
                                 , BOOST_CONTAINER_PP_TEMPLATE_PARAM_WITH_DEFAULT
                                 , void)
           >
   struct is_constructible
      : boost::integral_constant
         < bool
         , is_constructible_impl
            < T
            BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS, P)
            , void>::value
         >
   {};

   //Finally just inherit from the implementation to define he trait
   template <class T
            ,class InnerAlloc
            BOOST_PP_ENUM_TRAILING( BOOST_PP_SUB(BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS, 2)
                                 , BOOST_CONTAINER_PP_TEMPLATE_PARAM_WITH_DEFAULT
                                 , void)
            >
   struct is_constructible_with_allocator_prefix
      : is_constructible
         < T, allocator_arg_t, InnerAlloc
         BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_PP_SUB(BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS, 2), P)
         >
   {};
/*
   template <class T
            ,class InnerAlloc
            BOOST_PP_ENUM_TRAILING( BOOST_PP_SUB(BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS, 1)
                                 , BOOST_CONTAINER_PP_TEMPLATE_PARAM_WITH_DEFAULT
                                 , void)
            >
   struct is_constructible_with_allocator_suffix
      : is_constructible
         < T
         BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_PP_SUB(BOOST_CONTAINER_MAX_IS_CONSTRUCTIBLE_PARAMETERS, 1), P)
         , InnerAlloc
         >
   {};*/

   #endif   // #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

#else    // #if !defined(BOOST_NO_SFINAE_EXPR)

   //Without advanced SFINAE expressions, we can't use is_constructible
   //so backup to constructible_with_allocator_xxx

   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   template < class T, class InnerAlloc, class ...Args>
   struct is_constructible_with_allocator_prefix
      : constructible_with_allocator_prefix<T>
   {};
/*
   template < class T, class InnerAlloc, class ...Args>
   struct is_constructible_with_allocator_suffix
      : constructible_with_allocator_suffix<T>
   {};*/

   #else    // #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   template < class T
            , class InnerAlloc
            BOOST_PP_ENUM_TRAILING( BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS
                                  , BOOST_CONTAINER_PP_TEMPLATE_PARAM_WITH_DEFAULT
                                  , void)
            >
   struct is_constructible_with_allocator_prefix
      : constructible_with_allocator_prefix<T>
   {};
/*
   template < class T
            , class InnerAlloc
            BOOST_PP_ENUM_TRAILING( BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS
                                  , BOOST_CONTAINER_PP_TEMPLATE_PARAM_WITH_DEFAULT
                                  , void)
            >
   struct is_constructible_with_allocator_suffix
      : constructible_with_allocator_suffix<T>
   {};*/

   #endif   // #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

#endif   // #if !defined(BOOST_NO_SFINAE_EXPR)

#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

template < typename OutermostAlloc
         , typename InnerAlloc
         , typename T
         , class ...Args
         >
inline void dispatch_allocator_prefix_suffix
   ( boost::true_type  use_alloc_prefix, OutermostAlloc& outermost_alloc
   , InnerAlloc& inner_alloc, T* p, BOOST_FWD_REF(Args) ...args)
{
   (void)use_alloc_prefix;
   allocator_traits<OutermostAlloc>::construct
      ( outermost_alloc, p, allocator_arg, inner_alloc, ::boost::forward<Args>(args)...);
}

template < typename OutermostAlloc
         , typename InnerAlloc
         , typename T
         , class ...Args
         >
inline void dispatch_allocator_prefix_suffix
   ( boost::false_type use_alloc_prefix, OutermostAlloc& outermost_alloc
   , InnerAlloc &inner_alloc, T* p, BOOST_FWD_REF(Args)...args)
{
   (void)use_alloc_prefix;
   allocator_traits<OutermostAlloc>::construct
      (outermost_alloc, p, ::boost::forward<Args>(args)..., inner_alloc);
}

template < typename OutermostAlloc
         , typename InnerAlloc
         , typename T
         , class ...Args
         >
inline void dispatch_uses_allocator
   ( boost::true_type uses_allocator, OutermostAlloc& outermost_alloc
   , InnerAlloc& inner_alloc, T* p, BOOST_FWD_REF(Args)...args)
{
   (void)uses_allocator;
   //BOOST_STATIC_ASSERT((is_constructible_with_allocator_prefix<T, InnerAlloc, Args...>::value ||
   //                     is_constructible_with_allocator_suffix<T, InnerAlloc, Args...>::value ));
   dispatch_allocator_prefix_suffix
      ( is_constructible_with_allocator_prefix<T, InnerAlloc, Args...>()
      , outermost_alloc, inner_alloc, p, ::boost::forward<Args>(args)...);
}

template < typename OutermostAlloc
         , typename InnerAlloc
         , typename T
         , class ...Args
         >
inline void dispatch_uses_allocator
   ( boost::false_type uses_allocator, OutermostAlloc & outermost_alloc
   , InnerAlloc & inner_alloc
   ,T* p, BOOST_FWD_REF(Args)...args)
{
   (void)uses_allocator; (void)inner_alloc;
   allocator_traits<OutermostAlloc>::construct
      (outermost_alloc, p, ::boost::forward<Args>(args)...);
}

#else    //#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

#define BOOST_PP_LOCAL_MACRO(n)                                                              \
template < typename OutermostAlloc                                                           \
         , typename InnerAlloc                                                               \
         , typename T                                                                        \
         BOOST_PP_ENUM_TRAILING_PARAMS(n, class P)                                           \
         >                                                                                   \
inline void dispatch_allocator_prefix_suffix(                                                \
                                       boost::true_type  use_alloc_prefix,                   \
                                       OutermostAlloc& outermost_alloc,                      \
                                       InnerAlloc&    inner_alloc,                           \
                                       T* p                                                  \
                              BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))   \
{                                                                                            \
   (void)use_alloc_prefix,                                                                   \
   allocator_traits<OutermostAlloc>::construct                                               \
      (outermost_alloc, p, allocator_arg, inner_alloc                                        \
       BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));                      \
}                                                                                            \
                                                                                             \
template < typename OutermostAlloc                                                           \
         , typename InnerAlloc                                                               \
         , typename T                                                                        \
         BOOST_PP_ENUM_TRAILING_PARAMS(n, class P)                                           \
         >                                                                                   \
inline void dispatch_allocator_prefix_suffix(                                                \
                           boost::false_type use_alloc_prefix,                               \
                           OutermostAlloc& outermost_alloc,                                  \
                           InnerAlloc&    inner_alloc,                                       \
                           T* p BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _)) \
{                                                                                            \
   (void)use_alloc_prefix;                                                                   \
   allocator_traits<OutermostAlloc>::construct                                               \
      (outermost_alloc, p                                                                    \
       BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)                        \
      , inner_alloc);                                                                        \
}                                                                                            \
                                                                                             \
template < typename OutermostAlloc                                                           \
         , typename InnerAlloc                                                               \
         , typename T                                                                        \
         BOOST_PP_ENUM_TRAILING_PARAMS(n, class P)                                           \
         >                                                                                   \
inline void dispatch_uses_allocator(boost::true_type uses_allocator,                         \
                        OutermostAlloc& outermost_alloc,                                     \
                        InnerAlloc&    inner_alloc,                                          \
                        T* p BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))    \
{                                                                                            \
   (void)uses_allocator;                                                                     \
   dispatch_allocator_prefix_suffix                                                          \
      (is_constructible_with_allocator_prefix                                                \
         < T, InnerAlloc BOOST_PP_ENUM_TRAILING_PARAMS(n, P)>()                              \
         , outermost_alloc, inner_alloc, p                                                   \
         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));                    \
}                                                                                            \
                                                                                             \
template < typename OutermostAlloc                                                           \
         , typename InnerAlloc                                                               \
         , typename T                                                                        \
         BOOST_PP_ENUM_TRAILING_PARAMS(n, class P)                                           \
         >                                                                                   \
inline void dispatch_uses_allocator(boost::false_type uses_allocator                         \
                        ,OutermostAlloc &    outermost_alloc                                 \
                        ,InnerAlloc &    inner_alloc                                         \
                        ,T* p BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))   \
{                                                                                            \
   (void)uses_allocator; (void)inner_alloc;                                                  \
   allocator_traits<OutermostAlloc>::construct                                               \
      (outermost_alloc, p BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));   \
}                                                                                            \
//!
#define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
#include BOOST_PP_LOCAL_ITERATE()

#endif   //#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

template <typename OuterAlloc, class ...InnerAllocs>
class scoped_allocator_adaptor_base
   : public OuterAlloc
{
   typedef allocator_traits<OuterAlloc> outer_traits_type;
   BOOST_COPYABLE_AND_MOVABLE(scoped_allocator_adaptor_base)

   public:
   template <class OuterA2>
   struct rebind_base
   {
      typedef scoped_allocator_adaptor_base<OuterA2, InnerAllocs...> other;
   };

   typedef OuterAlloc outer_allocator_type;
   typedef scoped_allocator_adaptor<InnerAllocs...>   inner_allocator_type;
   typedef allocator_traits<inner_allocator_type>     inner_traits_type;
   typedef scoped_allocator_adaptor
      <OuterAlloc, InnerAllocs...>                    scoped_allocator_type;
   typedef boost::integral_constant<
      bool,
      outer_traits_type::propagate_on_container_copy_assignment::value ||
      inner_allocator_type::propagate_on_container_copy_assignment::value
      > propagate_on_container_copy_assignment;
   typedef boost::integral_constant<
      bool,
      outer_traits_type::propagate_on_container_move_assignment::value ||
      inner_allocator_type::propagate_on_container_move_assignment::value
      > propagate_on_container_move_assignment;
   typedef boost::integral_constant<
      bool,
      outer_traits_type::propagate_on_container_swap::value ||
      inner_allocator_type::propagate_on_container_swap::value
      > propagate_on_container_swap;

   scoped_allocator_adaptor_base()
      {}

   template <class OuterA2>
   scoped_allocator_adaptor_base(BOOST_FWD_REF(OuterA2) outerAlloc, const InnerAllocs &...args)
      : outer_allocator_type(::boost::forward<OuterA2>(outerAlloc))
      , m_inner(args...)
      {}

   scoped_allocator_adaptor_base(const scoped_allocator_adaptor_base& other)
      : outer_allocator_type(other.outer_allocator())
      , m_inner(other.inner_allocator())
      {}

   scoped_allocator_adaptor_base(BOOST_RV_REF(scoped_allocator_adaptor_base) other)
      : outer_allocator_type(::boost::move(other.outer_allocator()))
      , m_inner(::boost::move(other.inner_allocator()))
      {}

   template <class OuterA2>
   scoped_allocator_adaptor_base
      (const scoped_allocator_adaptor_base<OuterA2, InnerAllocs...>& other)
      : outer_allocator_type(other.outer_allocator())
      , m_inner(other.inner_allocator())
      {}

   template <class OuterA2>
   scoped_allocator_adaptor_base
      (BOOST_RV_REF_BEG scoped_allocator_adaptor_base
         <OuterA2, InnerAllocs...> BOOST_RV_REF_END other)
      : outer_allocator_type(other.outer_allocator())
      , m_inner(other.inner_allocator())
      {}

   public:
   struct internal_type_t{};

   template <class OuterA2>
   scoped_allocator_adaptor_base
      ( internal_type_t
      , BOOST_FWD_REF(OuterA2) outerAlloc
      , const inner_allocator_type &inner)
      : outer_allocator_type(::boost::forward<OuterA2>(outerAlloc))
      , m_inner(inner)
   {}

   public:

   scoped_allocator_adaptor_base &operator=
      (BOOST_COPY_ASSIGN_REF(scoped_allocator_adaptor_base) other)
   {
      outer_allocator_type::operator=(other.outer_allocator());
      m_inner = other.inner_allocator();
      return *this;
   }

   scoped_allocator_adaptor_base &operator=(BOOST_RV_REF(scoped_allocator_adaptor_base) other)
   {
      outer_allocator_type::operator=(boost::move(other.outer_allocator()));
      m_inner = ::boost::move(other.inner_allocator());
      return *this;
   }

   void swap(scoped_allocator_adaptor_base &r)
   {
      boost::container::swap_dispatch(this->outer_allocator(), r.outer_allocator());
      boost::container::swap_dispatch(this->m_inner, r.inner_allocator());
   }

   friend void swap(scoped_allocator_adaptor_base &l, scoped_allocator_adaptor_base &r)
   {  l.swap(r);  }

   inner_allocator_type&       inner_allocator()
      { return m_inner; }

   inner_allocator_type const& inner_allocator() const
      { return m_inner; }

   outer_allocator_type      & outer_allocator()
      { return static_cast<outer_allocator_type&>(*this); }

   const outer_allocator_type &outer_allocator() const
      { return static_cast<const outer_allocator_type&>(*this); }

   scoped_allocator_type select_on_container_copy_construction() const
   {
      return scoped_allocator_type
         (internal_type_t()
         ,outer_traits_type::select_on_container_copy_construction(this->outer_allocator())
         ,inner_traits_type::select_on_container_copy_construction(this->inner_allocator())
         );
   }

   private:
   inner_allocator_type m_inner;
};

#else //#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

//Let's add a dummy first template parameter to allow creating
//specializations up to maximum InnerAlloc count
template <
   typename OuterAlloc
   , bool Dummy
   BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, class Q)
   >
class scoped_allocator_adaptor_base;

//Specializations for the adaptor with InnerAlloc allocators

#define BOOST_PP_LOCAL_MACRO(n)                                                                 \
template <typename OuterAlloc                                                                   \
BOOST_PP_ENUM_TRAILING_PARAMS(n, class Q)                                                       \
>                                                                                               \
class scoped_allocator_adaptor_base<OuterAlloc, true                                            \
   BOOST_PP_ENUM_TRAILING_PARAMS(n, Q)                                                          \
   BOOST_PP_ENUM_TRAILING( BOOST_PP_SUB(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, n)          \
                         , BOOST_CONTAINER_PP_IDENTITY, nat)                                    \
   >                                                                                            \
   : public OuterAlloc                                                                          \
{                                                                                               \
   typedef allocator_traits<OuterAlloc> outer_traits_type;                                      \
   BOOST_COPYABLE_AND_MOVABLE(scoped_allocator_adaptor_base)                                    \
                                                                                                \
   public:                                                                                      \
   template <class OuterA2>                                                                     \
   struct rebind_base                                                                           \
   {                                                                                            \
      typedef scoped_allocator_adaptor_base<OuterA2, true BOOST_PP_ENUM_TRAILING_PARAMS(n, Q)   \
         BOOST_PP_ENUM_TRAILING                                                                 \
            ( BOOST_PP_SUB(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, n)                       \
            , BOOST_CONTAINER_PP_IDENTITY, nat)                                                 \
         > other;                                                                               \
   };                                                                                           \
                                                                                                \
   typedef OuterAlloc outer_allocator_type;                                                     \
   typedef scoped_allocator_adaptor<BOOST_PP_ENUM_PARAMS(n, Q)                                  \
      BOOST_PP_ENUM_TRAILING                                                                    \
         ( BOOST_PP_SUB(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, n)                          \
         , BOOST_CONTAINER_PP_IDENTITY, nat)                                                    \
      > inner_allocator_type;                                                                   \
   typedef scoped_allocator_adaptor<OuterAlloc, BOOST_PP_ENUM_PARAMS(n, Q)                      \
      BOOST_PP_ENUM_TRAILING                                                                    \
         ( BOOST_PP_SUB(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, n)                          \
         , BOOST_CONTAINER_PP_IDENTITY, nat)                                                    \
      > scoped_allocator_type;                                                                  \
   typedef allocator_traits<inner_allocator_type>   inner_traits_type;                          \
   typedef boost::integral_constant<                                                            \
      bool,                                                                                     \
      outer_traits_type::propagate_on_container_copy_assignment::value ||                       \
      inner_allocator_type::propagate_on_container_copy_assignment::value                       \
      > propagate_on_container_copy_assignment;                                                 \
   typedef boost::integral_constant<                                                            \
      bool,                                                                                     \
      outer_traits_type::propagate_on_container_move_assignment::value ||                       \
      inner_allocator_type::propagate_on_container_move_assignment::value                       \
      > propagate_on_container_move_assignment;                                                 \
   typedef boost::integral_constant<                                                            \
      bool,                                                                                     \
      outer_traits_type::propagate_on_container_swap::value ||                                  \
      inner_allocator_type::propagate_on_container_swap::value                                  \
      > propagate_on_container_swap;                                                            \
                                                                                                \
   scoped_allocator_adaptor_base()                                                              \
      {}                                                                                        \
                                                                                                \
   template <class OuterA2>                                                                     \
   scoped_allocator_adaptor_base(BOOST_FWD_REF(OuterA2) outerAlloc                              \
      BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_CONST_REF_PARAM_LIST_Q, _))                  \
      : outer_allocator_type(::boost::forward<OuterA2>(outerAlloc))                             \
      , m_inner(BOOST_PP_ENUM_PARAMS(n, q))                                                     \
      {}                                                                                        \
                                                                                                \
   scoped_allocator_adaptor_base(const scoped_allocator_adaptor_base& other)                    \
      : outer_allocator_type(other.outer_allocator())                                           \
      , m_inner(other.inner_allocator())                                                        \
      {}                                                                                        \
                                                                                                \
   scoped_allocator_adaptor_base(BOOST_RV_REF(scoped_allocator_adaptor_base) other)             \
      : outer_allocator_type(::boost::move(other.outer_allocator()))                            \
      , m_inner(::boost::move(other.inner_allocator()))                                         \
      {}                                                                                        \
                                                                                                \
   template <class OuterA2>                                                                     \
   scoped_allocator_adaptor_base(const scoped_allocator_adaptor_base<OuterA2, true              \
          BOOST_PP_ENUM_TRAILING_PARAMS(n, Q)                                                   \
          BOOST_PP_ENUM_TRAILING                                                                \
            ( BOOST_PP_SUB(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, n)                       \
            , BOOST_CONTAINER_PP_IDENTITY, nat)                                                 \
         >& other)                                                                              \
      : outer_allocator_type(other.outer_allocator())                                           \
      , m_inner(other.inner_allocator())                                                        \
      {}                                                                                        \
                                                                                                \
   template <class OuterA2>                                                                     \
   scoped_allocator_adaptor_base                                                                \
      (BOOST_RV_REF_BEG scoped_allocator_adaptor_base<OuterA2, true                             \
         BOOST_PP_ENUM_TRAILING_PARAMS(n, Q)                                                    \
         BOOST_PP_ENUM_TRAILING                                                                 \
            ( BOOST_PP_SUB(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, n)                       \
            , BOOST_CONTAINER_PP_IDENTITY, nat)                                                 \
         > BOOST_RV_REF_END other)                                                              \
      : outer_allocator_type(other.outer_allocator())                                           \
      , m_inner(other.inner_allocator())                                                        \
      {}                                                                                        \
                                                                                                \
   public:                                                                                      \
   struct internal_type_t{};                                                                    \
                                                                                                \
   template <class OuterA2>                                                                     \
   scoped_allocator_adaptor_base                                                                \
      ( internal_type_t                                                                         \
      , BOOST_FWD_REF(OuterA2) outerAlloc                                                       \
      , const inner_allocator_type &inner)                                                      \
      : outer_allocator_type(::boost::forward<OuterA2>(outerAlloc))                             \
      , m_inner(inner)                                                                          \
   {}                                                                                           \
                                                                                                \
   public:                                                                                      \
   scoped_allocator_adaptor_base &operator=                                                     \
      (BOOST_COPY_ASSIGN_REF(scoped_allocator_adaptor_base) other)                              \
   {                                                                                            \
      outer_allocator_type::operator=(other.outer_allocator());                                 \
      m_inner = other.inner_allocator();                                                        \
      return *this;                                                                             \
   }                                                                                            \
                                                                                                \
   scoped_allocator_adaptor_base &operator=(BOOST_RV_REF(scoped_allocator_adaptor_base) other)  \
   {                                                                                            \
      outer_allocator_type::operator=(boost::move(other.outer_allocator()));                    \
      m_inner = ::boost::move(other.inner_allocator());                                         \
      return *this;                                                                             \
   }                                                                                            \
                                                                                                \
   void swap(scoped_allocator_adaptor_base &r)                                                  \
   {                                                                                            \
      boost::container::swap_dispatch(this->outer_allocator(), r.outer_allocator());            \
      boost::container::swap_dispatch(this->m_inner, r.inner_allocator());                      \
   }                                                                                            \
                                                                                                \
   friend void swap(scoped_allocator_adaptor_base &l, scoped_allocator_adaptor_base &r)         \
   {  l.swap(r);  }                                                                             \
                                                                                                \
   inner_allocator_type&       inner_allocator()                                                \
      { return m_inner; }                                                                       \
                                                                                                \
   inner_allocator_type const& inner_allocator() const                                          \
      { return m_inner; }                                                                       \
                                                                                                \
   outer_allocator_type      & outer_allocator()                                                \
      { return static_cast<outer_allocator_type&>(*this); }                                     \
                                                                                                \
   const outer_allocator_type &outer_allocator() const                                          \
      { return static_cast<const outer_allocator_type&>(*this); }                               \
                                                                                                \
   scoped_allocator_type select_on_container_copy_construction() const                          \
   {                                                                                            \
      return scoped_allocator_type                                                              \
         (internal_type_t()                                                                     \
         ,outer_traits_type::select_on_container_copy_construction(this->outer_allocator())     \
         ,inner_traits_type::select_on_container_copy_construction(this->inner_allocator())     \
         );                                                                                     \
   }                                                                                            \
   private:                                                                                     \
   inner_allocator_type m_inner;                                                                \
};                                                                                              \
//!
#define BOOST_PP_LOCAL_LIMITS (1, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
#include BOOST_PP_LOCAL_ITERATE()

#endif   //#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

//Specialization for adaptor without any InnerAlloc
template <typename OuterAlloc>
class scoped_allocator_adaptor_base
   < OuterAlloc
   #if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
      , true
      BOOST_PP_ENUM_TRAILING(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, BOOST_CONTAINER_PP_IDENTITY, nat)
   #endif
   >
   : public OuterAlloc
{
   BOOST_COPYABLE_AND_MOVABLE(scoped_allocator_adaptor_base)
   public:

   template <class U>
   struct rebind_base
   {
      typedef scoped_allocator_adaptor_base
         <typename allocator_traits<OuterAlloc>::template portable_rebind_alloc<U>::type
         #if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
         , true
         BOOST_PP_ENUM_TRAILING(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, BOOST_CONTAINER_PP_IDENTITY, container_detail::nat)
         #endif
         > other;
   };

   typedef OuterAlloc                           outer_allocator_type;
   typedef allocator_traits<OuterAlloc>         outer_traits_type;
   typedef scoped_allocator_adaptor<OuterAlloc> inner_allocator_type;
   typedef inner_allocator_type                 scoped_allocator_type;
   typedef allocator_traits<inner_allocator_type>   inner_traits_type;
   typedef typename outer_traits_type::
      propagate_on_container_copy_assignment    propagate_on_container_copy_assignment;
   typedef typename outer_traits_type::
      propagate_on_container_move_assignment    propagate_on_container_move_assignment;
   typedef typename outer_traits_type::
      propagate_on_container_swap               propagate_on_container_swap;

   scoped_allocator_adaptor_base()
      {}

   template <class OuterA2>
   scoped_allocator_adaptor_base(BOOST_FWD_REF(OuterA2) outerAlloc)
      : outer_allocator_type(::boost::forward<OuterA2>(outerAlloc))
      {}

   scoped_allocator_adaptor_base(const scoped_allocator_adaptor_base& other)
      : outer_allocator_type(other.outer_allocator())
      {}

   scoped_allocator_adaptor_base(BOOST_RV_REF(scoped_allocator_adaptor_base) other)
      : outer_allocator_type(::boost::move(other.outer_allocator()))
      {}

   template <class OuterA2>
   scoped_allocator_adaptor_base
      (const scoped_allocator_adaptor_base<
         OuterA2
         #if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
         , true
         BOOST_PP_ENUM_TRAILING(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, BOOST_CONTAINER_PP_IDENTITY, container_detail::nat)
         #endif
         >& other)
      : outer_allocator_type(other.outer_allocator())
      {}

   template <class OuterA2>
   scoped_allocator_adaptor_base
      (BOOST_RV_REF_BEG scoped_allocator_adaptor_base<
         OuterA2
         #if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
         , true
         BOOST_PP_ENUM_TRAILING(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, BOOST_CONTAINER_PP_IDENTITY, container_detail::nat)
         #endif
         > BOOST_RV_REF_END other)
      : outer_allocator_type(other.outer_allocator())
      {}

   public:
   struct internal_type_t{};

   template <class OuterA2>
   scoped_allocator_adaptor_base(internal_type_t, BOOST_FWD_REF(OuterA2) outerAlloc, const inner_allocator_type &)
      : outer_allocator_type(::boost::forward<OuterA2>(outerAlloc))
      {}
 
   public:
   scoped_allocator_adaptor_base &operator=(BOOST_COPY_ASSIGN_REF(scoped_allocator_adaptor_base) other)
   {
      outer_allocator_type::operator=(other.outer_allocator());
      return *this;
   }

   scoped_allocator_adaptor_base &operator=(BOOST_RV_REF(scoped_allocator_adaptor_base) other)
   {
      outer_allocator_type::operator=(boost::move(other.outer_allocator()));
      return *this;
   }

   void swap(scoped_allocator_adaptor_base &r)
   {
      boost::container::swap_dispatch(this->outer_allocator(), r.outer_allocator());
   }

   friend void swap(scoped_allocator_adaptor_base &l, scoped_allocator_adaptor_base &r)
   {  l.swap(r);  }

   inner_allocator_type&       inner_allocator()
      { return static_cast<inner_allocator_type&>(*this); }

   inner_allocator_type const& inner_allocator() const
      { return static_cast<const inner_allocator_type&>(*this); }

   outer_allocator_type      & outer_allocator()
      { return static_cast<outer_allocator_type&>(*this); }

   const outer_allocator_type &outer_allocator() const
      { return static_cast<const outer_allocator_type&>(*this); }

   scoped_allocator_type select_on_container_copy_construction() const
   {
      return scoped_allocator_type
         (internal_type_t()
         ,outer_traits_type::select_on_container_copy_construction(this->outer_allocator())
         //Don't use inner_traits_type::select_on_container_copy_construction(this->inner_allocator())
         //as inner_allocator() is equal to *this and that would trigger an infinite loop
         , this->inner_allocator()
         );
   }
};

}  //namespace container_detail {

///@endcond

//Scoped allocator
#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #if !defined(BOOST_CONTAINER_UNIMPLEMENTED_PACK_EXPANSION_TO_FIXED_LIST)

   //! This class is a C++03-compatible implementation of std::scoped_allocator_adaptor.
   //! The class template scoped_allocator_adaptor is an allocator template that specifies
   //! the memory resource (the outer allocator) to be used by a container (as any other
   //! allocator does) and also specifies an inner allocator resource to be passed to
   //! the constructor of every element within the container.
   //!
   //! This adaptor is
   //! instantiated with one outer and zero or more inner allocator types. If
   //! instantiated with only one allocator type, the inner allocator becomes the
   //! scoped_allocator_adaptor itself, thus using the same allocator resource for the
   //! container and every element within the container and, if the elements themselves
   //! are containers, each of their elements recursively. If instantiated with more than
   //! one allocator, the first allocator is the outer allocator for use by the container,
   //! the second allocator is passed to the constructors of the container's elements,
   //! and, if the elements themselves are containers, the third allocator is passed to
   //! the elements' elements, and so on. If containers are nested to a depth greater
   //! than the number of allocators, the last allocator is used repeatedly, as in the
   //! single-allocator case, for any remaining recursions.
   //!
   //! [<b>Note</b>: The
   //! scoped_allocator_adaptor is derived from the outer allocator type so it can be
   //! substituted for the outer allocator type in most expressions. -end note]
   //!
   //! In the construct member functions, `OUTERMOST(x)` is x if x does not have
   //! an `outer_allocator()` member function and
   //! `OUTERMOST(x.outer_allocator())` otherwise; `OUTERMOST_ALLOC_TRAITS(x)` is
   //! `allocator_traits<decltype(OUTERMOST(x))>`.
   //!
   //! [<b>Note</b>: `OUTERMOST(x)` and
   //! `OUTERMOST_ALLOC_TRAITS(x)` are recursive operations. It is incumbent upon
   //! the definition of `outer_allocator()` to ensure that the recursion terminates.
   //! It will terminate for all instantiations of scoped_allocator_adaptor. -end note]
   template <typename OuterAlloc, typename ...InnerAllocs>
   class scoped_allocator_adaptor

   #else // #if !defined(BOOST_CONTAINER_UNIMPLEMENTED_PACK_EXPANSION_TO_FIXED_LIST)

   template <typename OuterAlloc, typename ...InnerAllocs>
   class scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>

   #endif   // #if !defined(BOOST_CONTAINER_UNIMPLEMENTED_PACK_EXPANSION_TO_FIXED_LIST)

#else // #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

template <typename OuterAlloc
         BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, class Q)
         >
class scoped_allocator_adaptor
#endif
   : public container_detail::scoped_allocator_adaptor_base
         <OuterAlloc
         #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
         , InnerAllocs...
         #else
         , true BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, Q)
         #endif
         >
{
   BOOST_COPYABLE_AND_MOVABLE(scoped_allocator_adaptor)

   public:
   /// @cond
   typedef container_detail::scoped_allocator_adaptor_base
      <OuterAlloc
      #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , InnerAllocs...
      #else
      , true BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, Q)
      #endif
      >                       base_type;
   typedef typename base_type::internal_type_t              internal_type_t;
   /// @endcond
   typedef OuterAlloc                                       outer_allocator_type;
   //! Type: For exposition only
   //!
   typedef allocator_traits<OuterAlloc>                     outer_traits_type;
   //! Type: `scoped_allocator_adaptor<OuterAlloc>` if `sizeof...(InnerAllocs)` is zero; otherwise,
   //! `scoped_allocator_adaptor<InnerAllocs...>`.
   typedef typename base_type::inner_allocator_type         inner_allocator_type;
   typedef allocator_traits<inner_allocator_type>           inner_traits_type;
   typedef typename outer_traits_type::value_type           value_type;
   typedef typename outer_traits_type::size_type            size_type;
   typedef typename outer_traits_type::difference_type      difference_type;
   typedef typename outer_traits_type::pointer              pointer;
   typedef typename outer_traits_type::const_pointer        const_pointer;
   typedef typename outer_traits_type::void_pointer         void_pointer;
   typedef typename outer_traits_type::const_void_pointer   const_void_pointer;
   //! Type: `true_type` if `allocator_traits<Allocator>::propagate_on_container_copy_assignment::value` is
   //! true for any `Allocator` in the set of `OuterAlloc` and `InnerAllocs...`; otherwise, false_type.
   typedef typename base_type::
      propagate_on_container_copy_assignment                propagate_on_container_copy_assignment;
   //! Type: `true_type` if `allocator_traits<Allocator>::propagate_on_container_move_assignment::value` is
   //! true for any `Allocator` in the set of `OuterAlloc` and `InnerAllocs...`; otherwise, false_type.
   typedef typename base_type::
      propagate_on_container_move_assignment                propagate_on_container_move_assignment;
   //! Type: `true_type` if `allocator_traits<Allocator>::propagate_on_container_swap::value` is true for any
   //! `Allocator` in the set of `OuterAlloc` and `InnerAllocs...`; otherwise, false_type.
   typedef typename base_type::
      propagate_on_container_swap                           propagate_on_container_swap;

   //! Type: Rebinds scoped allocator to
   //!    `typedef scoped_allocator_adaptor
   //!      < typename outer_traits_type::template portable_rebind_alloc<U>::type
   //!      , InnerAllocs... >`
   template <class U>
   struct rebind
   {
      typedef scoped_allocator_adaptor
         < typename outer_traits_type::template portable_rebind_alloc<U>::type
         #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
         , InnerAllocs...
         #else
         BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, Q)
         #endif
         > other;
   };

   //! <b>Effects</b>: value-initializes the OuterAlloc base class
   //! and the inner allocator object.
   scoped_allocator_adaptor()
      {}

   ~scoped_allocator_adaptor()
      {}

   //! <b>Effects</b>: initializes each allocator within the adaptor with
   //! the corresponding allocator from other.
   scoped_allocator_adaptor(const scoped_allocator_adaptor& other)
      : base_type(other.base())
      {}

   //! <b>Effects</b>: move constructs each allocator within the adaptor with
   //! the corresponding allocator from other.
   scoped_allocator_adaptor(BOOST_RV_REF(scoped_allocator_adaptor) other)
      : base_type(::boost::move(other.base()))
      {}

   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Requires</b>: OuterAlloc shall be constructible from OuterA2.
   //!
   //! <b>Effects</b>: initializes the OuterAlloc base class with boost::forward<OuterA2>(outerAlloc) and inner
   //! with innerAllocs...(hence recursively initializing each allocator within the adaptor with the
   //! corresponding allocator from the argument list).
   template <class OuterA2>
   scoped_allocator_adaptor(BOOST_FWD_REF(OuterA2) outerAlloc, const InnerAllocs & ...innerAllocs)
      : base_type(::boost::forward<OuterA2>(outerAlloc), innerAllocs...)
      {}
   #else // #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #define BOOST_PP_LOCAL_MACRO(n)                                                              \
   template <class OuterA2>                                                                     \
   scoped_allocator_adaptor(BOOST_FWD_REF(OuterA2) outerAlloc                                   \
                     BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_CONST_REF_PARAM_LIST_Q, _))   \
      : base_type(::boost::forward<OuterA2>(outerAlloc)                                         \
                  BOOST_PP_ENUM_TRAILING_PARAMS(n, q)                                           \
                  )                                                                             \
      {}                                                                                        \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   // #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Requires</b>: OuterAlloc shall be constructible from OuterA2.
   //!
   //! <b>Effects</b>: initializes each allocator within the adaptor with the corresponding allocator from other.
   template <class OuterA2>
   scoped_allocator_adaptor(const scoped_allocator_adaptor<OuterA2
      #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , InnerAllocs...
      #else
      BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, Q)
      #endif
      > &other)
      : base_type(other.base())
      {}

   //! <b>Requires</b>: OuterAlloc shall be constructible from OuterA2.
   //!
   //! <b>Effects</b>: initializes each allocator within the adaptor with the corresponding allocator
   //! rvalue from other.
   template <class OuterA2>
   scoped_allocator_adaptor(BOOST_RV_REF_BEG scoped_allocator_adaptor<OuterA2
      #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , InnerAllocs...
      #else
      BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, Q)
      #endif
      > BOOST_RV_REF_END other)
      : base_type(::boost::move(other.base()))
      {}

   scoped_allocator_adaptor &operator=(BOOST_COPY_ASSIGN_REF(scoped_allocator_adaptor) other)
   {  return static_cast<scoped_allocator_adaptor&>(base_type::operator=(static_cast<const base_type &>(other))); }

   scoped_allocator_adaptor &operator=(BOOST_RV_REF(scoped_allocator_adaptor) other)
   {  return static_cast<scoped_allocator_adaptor&>(base_type::operator=(boost::move(static_cast<base_type&>(other)))); }

   #ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
   //! <b>Effects</b>: swaps *this with r.
   //!
   void swap(scoped_allocator_adaptor &r);

   //! <b>Effects</b>: swaps *this with r.
   //!
   friend void swap(scoped_allocator_adaptor &l, scoped_allocator_adaptor &r);

   //! <b>Returns</b>:
   //!   `static_cast<OuterAlloc&>(*this)`.
   outer_allocator_type      & outer_allocator();

   //! <b>Returns</b>:
   //!   `static_cast<const OuterAlloc&>(*this)`.
   const outer_allocator_type &outer_allocator() const;

   //! <b>Returns</b>:
   //!   *this if `sizeof...(InnerAllocs)` is zero; otherwise, inner.
   inner_allocator_type&       inner_allocator();

   //! <b>Returns</b>:
   //!   *this if `sizeof...(InnerAllocs)` is zero; otherwise, inner.
   inner_allocator_type const& inner_allocator() const;

   #endif   //BOOST_CONTAINER_DOXYGEN_INVOKED

   //! <b>Returns</b>:
   //!   `allocator_traits<OuterAlloc>::max_size(outer_allocator())`.
   size_type max_size() const
   {
      return outer_traits_type::max_size(this->outer_allocator());
   }

   //! <b>Effects</b>:
   //!   calls `OUTERMOST_ALLOC_TRAITS(*this)::destroy(OUTERMOST(*this), p)`.
   template <class T>
   void destroy(T* p)
   {
      allocator_traits<typename outermost_allocator<OuterAlloc>::type>
         ::destroy(get_outermost_allocator(this->outer_allocator()), p);
   }

   //! <b>Returns</b>:
   //! `allocator_traits<OuterAlloc>::allocate(outer_allocator(), n)`.
   pointer allocate(size_type n)
   {
      return outer_traits_type::allocate(this->outer_allocator(), n);
   }

   //! <b>Returns</b>:
   //! `allocator_traits<OuterAlloc>::allocate(outer_allocator(), n, hint)`.
   pointer allocate(size_type n, const_void_pointer hint)
   {
      return outer_traits_type::allocate(this->outer_allocator(), n, hint);
   }

   //! <b>Effects</b>:
   //! `allocator_traits<OuterAlloc>::deallocate(outer_allocator(), p, n)`.
   void deallocate(pointer p, size_type n)
   {
      outer_traits_type::deallocate(this->outer_allocator(), p, n);
   }

   #ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
   //! <b>Returns</b>: Allocator new scoped_allocator_adaptor object where each allocator
   //! A in the adaptor is initialized from the result of calling
   //! `allocator_traits<Allocator>::select_on_container_copy_construction()` on
   //! the corresponding allocator in *this.
   scoped_allocator_adaptor select_on_container_copy_construction() const;
   #endif   //BOOST_CONTAINER_DOXYGEN_INVOKED

   /// @cond
   base_type &base()             { return *this; }

   const base_type &base() const { return *this; }
   /// @endcond

   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>:
   //! 1) If `uses_allocator<T, inner_allocator_type>::value` is false calls
   //!    `OUTERMOST_ALLOC_TRAITS(*this)::construct
   //!       (OUTERMOST(*this), p, std::forward<Args>(args)...)`.
   //!
   //! 2) Otherwise, if `uses_allocator<T, inner_allocator_type>::value` is true and
   //!    `is_constructible<T, allocator_arg_t, inner_allocator_type, Args...>::value` is true, calls
   //!    `OUTERMOST_ALLOC_TRAITS(*this)::construct(OUTERMOST(*this), p, allocator_arg,
   //!    inner_allocator(), std::forward<Args>(args)...)`.
   //!
   //! [<b>Note</b>: In compilers without advanced decltype SFINAE support, `is_constructible` can't
   //! be implemented so that condition will be replaced by
   //! constructible_with_allocator_prefix<T>::value. -end note]
   //!
   //! 3) Otherwise, if uses_allocator<T, inner_allocator_type>::value is true and
   //!    `is_constructible<T, Args..., inner_allocator_type>::value` is true, calls
   //!    `OUTERMOST_ALLOC_TRAITS(*this)::construct(OUTERMOST(*this), p,
   //!    std::forward<Args>(args)..., inner_allocator())`.
   //!
   //! [<b>Note</b>: In compilers without advanced decltype SFINAE support, `is_constructible` can't be
   //! implemented so that condition will be replaced by
   //! `constructible_with_allocator_suffix<T>::value`. -end note]
   //!
   //! 4) Otherwise, the program is ill-formed.
   //!
   //! [<b>Note</b>: An error will result if `uses_allocator` evaluates
   //! to true but the specific constructor does not take an allocator. This definition prevents a silent
   //! failure to pass an inner allocator to a contained element. -end note]
   template < typename T, class ...Args>
   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   void
   #else
   typename container_detail::enable_if_c<!container_detail::is_pair<T>::value, void>::type
   #endif
   construct(T* p, BOOST_FWD_REF(Args)...args)
   {
      container_detail::dispatch_uses_allocator
         ( uses_allocator<T, inner_allocator_type>()
         , get_outermost_allocator(this->outer_allocator())
         , this->inner_allocator()
         , p, ::boost::forward<Args>(args)...);
   }

   #else // #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //Disable this overload if the first argument is pair as some compilers have
   //overload selection problems when the first parameter is a pair.
   #define BOOST_PP_LOCAL_MACRO(n)                                                              \
   template < typename T                                                                        \
            BOOST_PP_ENUM_TRAILING_PARAMS(n, class P)                                           \
            >                                                                                   \
   typename container_detail::enable_if_c<!container_detail::is_pair<T>::value, void>::type     \
      construct(T* p BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))               \
   {                                                                                            \
      container_detail::dispatch_uses_allocator                                                 \
         ( uses_allocator<T, inner_allocator_type>()                                            \
         , get_outermost_allocator(this->outer_allocator())                                     \
         , this->inner_allocator()                                                              \
         , p BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));                   \
   }                                                                                            \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   // #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   template <class T1, class T2>
   void construct(std::pair<T1,T2>* p)
   {  this->construct_pair(p);  }

   template <class T1, class T2>
   void construct(container_detail::pair<T1,T2>* p)
   {  this->construct_pair(p);  }

   template <class T1, class T2, class U, class V>
   void construct(std::pair<T1, T2>* p, BOOST_FWD_REF(U) x, BOOST_FWD_REF(V) y)
   {  this->construct_pair(p, ::boost::forward<U>(x), ::boost::forward<V>(y));   }

   template <class T1, class T2, class U, class V>
   void construct(container_detail::pair<T1, T2>* p, BOOST_FWD_REF(U) x, BOOST_FWD_REF(V) y)
   {  this->construct_pair(p, ::boost::forward<U>(x), ::boost::forward<V>(y));   }
  
   template <class T1, class T2, class U, class V>
   void construct(std::pair<T1, T2>* p, const std::pair<U, V>& x)
   {  this->construct_pair(p, x);   }

   template <class T1, class T2, class U, class V>
   void construct( container_detail::pair<T1, T2>* p
                 , const container_detail::pair<U, V>& x)
   {  this->construct_pair(p, x);   }
  
   template <class T1, class T2, class U, class V>
   void construct( std::pair<T1, T2>* p
                 , BOOST_RV_REF_BEG std::pair<U, V> BOOST_RV_REF_END x)
   {  this->construct_pair(p, x);   }

   template <class T1, class T2, class U, class V>
   void construct( container_detail::pair<T1, T2>* p
                 , BOOST_RV_REF_BEG container_detail::pair<U, V> BOOST_RV_REF_END x)
   {  this->construct_pair(p, x);   }

   /// @cond
   private:
   template <class Pair>
   void construct_pair(Pair* p)
   {
      this->construct(container_detail::addressof(p->first));
      BOOST_TRY{
         this->construct(container_detail::addressof(p->second));
      }
      BOOST_CATCH(...){
         this->destroy(container_detail::addressof(p->first));
         BOOST_RETHROW
      }
      BOOST_CATCH_END
   }

   template <class Pair, class U, class V>
   void construct_pair(Pair* p, BOOST_FWD_REF(U) x, BOOST_FWD_REF(V) y)
   {
      this->construct(container_detail::addressof(p->first), ::boost::forward<U>(x));
      BOOST_TRY{
         this->construct(container_detail::addressof(p->second), ::boost::forward<V>(y));
      }
      BOOST_CATCH(...){
         this->destroy(container_detail::addressof(p->first));
         BOOST_RETHROW
      }
      BOOST_CATCH_END
   }

   template <class Pair, class Pair2>
   void construct_pair(Pair* p, const Pair2& pr)
   {
      this->construct(container_detail::addressof(p->first), pr.first);
      BOOST_TRY{
         this->construct(container_detail::addressof(p->second), pr.second);
      }
      BOOST_CATCH(...){
         this->destroy(container_detail::addressof(p->first));
         BOOST_RETHROW
      }
      BOOST_CATCH_END
   }

   template <class Pair, class Pair2>
   void construct_pair(Pair* p, BOOST_RV_REF(Pair2) pr)
   {
      this->construct(container_detail::addressof(p->first), ::boost::move(pr.first));
      BOOST_TRY{
         this->construct(container_detail::addressof(p->second), ::boost::move(pr.second));
      }
      BOOST_CATCH(...){
         this->destroy(container_detail::addressof(p->first));
         BOOST_RETHROW
      }
      BOOST_CATCH_END
   }

   //template <class T1, class T2, class... Args1, class... Args2>
   //void construct(pair<T1, T2>* p, piecewise_construct_t, tuple<Args1...> x, tuple<Args2...> y);

   public:
   //Internal function
   template <class OuterA2>
   scoped_allocator_adaptor(internal_type_t, BOOST_FWD_REF(OuterA2) outer, const inner_allocator_type& inner)
      : base_type(internal_type_t(), ::boost::forward<OuterA2>(outer), inner)
   {}

   /// @endcond
};

template <typename OuterA1, typename OuterA2
   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   , typename... InnerAllocs
   #else
   BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, class Q)
   #endif
   >
inline bool operator==(
   const scoped_allocator_adaptor<OuterA1
      #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      ,InnerAllocs...
      #else
      BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, Q)
      #endif
      >& a,
   const scoped_allocator_adaptor<OuterA2
      #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      ,InnerAllocs...
      #else
      BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, Q)
      #endif
   >& b)
{
   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)  
   const bool has_zero_inner = sizeof...(InnerAllocs) == 0u;
   #else
   const bool has_zero_inner =
      boost::container::container_detail::is_same
         <Q0, container_detail::nat>::value;
   #endif

    return a.outer_allocator() == b.outer_allocator()
        && (has_zero_inner || a.inner_allocator() == b.inner_allocator());
}

template <typename OuterA1, typename OuterA2
   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   , typename... InnerAllocs
   #else
   BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, class Q)
   #endif
   >
inline bool operator!=(
   const scoped_allocator_adaptor<OuterA1
      #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      ,InnerAllocs...
      #else
      BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, Q)
      #endif
      >& a,
   const scoped_allocator_adaptor<OuterA2
      #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      ,InnerAllocs...
      #else
      BOOST_PP_ENUM_TRAILING_PARAMS(BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS, Q)
      #endif
   >& b)
{
    return ! (a == b);
}

}} // namespace boost { namespace container {

#include <boost/container/detail/config_end.hpp>

#endif //  BOOST_CONTAINER_ALLOCATOR_SCOPED_ALLOCATOR_HPP
