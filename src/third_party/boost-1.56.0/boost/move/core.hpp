//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2012-2012.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/move for documentation.
//
//////////////////////////////////////////////////////////////////////////////

//! \file core.hpp
//! This header implements macros to define movable classes and
//! move-aware functions

#ifndef BOOST_MOVE_CORE_HPP
#define BOOST_MOVE_CORE_HPP

#include <boost/move/detail/config_begin.hpp>

//boost_move_no_copy_constructor_or_assign typedef
//used to detect noncopyable types for other Boost libraries.
#ifdef BOOST_NO_CXX11_DELETED_FUNCTIONS
   #define BOOST_MOVE_IMPL_NO_COPY_CTOR_OR_ASSIGN(TYPE) \
      private:\
      TYPE(TYPE &);\
      TYPE& operator=(TYPE &);\
      public:\
      typedef int boost_move_no_copy_constructor_or_assign; \
      private:\
   //
#else
   #define BOOST_MOVE_IMPL_NO_COPY_CTOR_OR_ASSIGN(TYPE) \
      public:\
      TYPE(TYPE const &) = delete;\
      TYPE& operator=(TYPE const &) = delete;\
      public:\
      typedef int boost_move_no_copy_constructor_or_assign; \
      private:\
   //
#endif   //BOOST_NO_CXX11_DELETED_FUNCTIONS

#if defined(BOOST_NO_CXX11_RVALUE_REFERENCES) && !defined(BOOST_MOVE_DOXYGEN_INVOKED)

   #include <boost/move/detail/meta_utils.hpp>

   //Move emulation rv breaks standard aliasing rules so add workarounds for some compilers
   #if defined(__GNUC__) && (__GNUC__ >= 4)
      #define BOOST_MOVE_ATTRIBUTE_MAY_ALIAS __attribute__((__may_alias__))
   #else
      #define BOOST_MOVE_ATTRIBUTE_MAY_ALIAS
   #endif

   namespace boost {

   //////////////////////////////////////////////////////////////////////////////
   //
   //                            struct rv
   //
   //////////////////////////////////////////////////////////////////////////////
   template <class T>
   class rv
      : public ::boost::move_detail::if_c
         < ::boost::move_detail::is_class_or_union<T>::value
         , T
         , ::boost::move_detail::empty
         >::type
   {
      rv();
      ~rv();
      rv(rv const&);
      void operator=(rv const&);
   } BOOST_MOVE_ATTRIBUTE_MAY_ALIAS;


   //////////////////////////////////////////////////////////////////////////////
   //
   //                            move_detail::is_rv
   //
   //////////////////////////////////////////////////////////////////////////////

   namespace move_detail {

   template <class T>
   struct is_rv
      : ::boost::move_detail::integral_constant<bool, false>
   {};

   template <class T>
   struct is_rv< rv<T> >
      : ::boost::move_detail::integral_constant<bool, true>
   {};

   template <class T>
   struct is_rv< const rv<T> >
      : ::boost::move_detail::integral_constant<bool, true>
   {};

   }  //namespace move_detail {

   //////////////////////////////////////////////////////////////////////////////
   //
   //                               has_move_emulation_enabled
   //
   //////////////////////////////////////////////////////////////////////////////
   template<class T>
   struct has_move_emulation_enabled
      : ::boost::move_detail::is_convertible< T, ::boost::rv<T>& >
   {};

   template<class T>
   struct has_move_emulation_enabled<T&>
      : ::boost::move_detail::integral_constant<bool, false>
   {};

   template<class T>
   struct has_move_emulation_enabled< ::boost::rv<T> >
      : ::boost::move_detail::integral_constant<bool, false>
   {};

   }  //namespace boost {

   #define BOOST_RV_REF(TYPE)\
      ::boost::rv< TYPE >& \
   //

   #define BOOST_RV_REF_2_TEMPL_ARGS(TYPE, ARG1, ARG2)\
      ::boost::rv< TYPE<ARG1, ARG2> >& \
   //

   #define BOOST_RV_REF_3_TEMPL_ARGS(TYPE, ARG1, ARG2, ARG3)\
      ::boost::rv< TYPE<ARG1, ARG2, ARG3> >& \
   //

   #define BOOST_RV_REF_BEG\
      ::boost::rv<   \
   //

   #define BOOST_RV_REF_END\
      >& \
   //

   #define BOOST_FWD_REF(TYPE)\
      const TYPE & \
   //

   #define BOOST_COPY_ASSIGN_REF(TYPE)\
      const ::boost::rv< TYPE >& \
   //

   #define BOOST_COPY_ASSIGN_REF_BEG \
      const ::boost::rv<  \
   //

   #define BOOST_COPY_ASSIGN_REF_END \
      >& \
   //

   #define BOOST_COPY_ASSIGN_REF_2_TEMPL_ARGS(TYPE, ARG1, ARG2)\
      const ::boost::rv< TYPE<ARG1, ARG2> >& \
   //

   #define BOOST_COPY_ASSIGN_REF_3_TEMPL_ARGS(TYPE, ARG1, ARG2, ARG3)\
      const ::boost::rv< TYPE<ARG1, ARG2, ARG3> >& \
   //

   #define BOOST_CATCH_CONST_RLVALUE(TYPE)\
      const ::boost::rv< TYPE >& \
   //

   namespace boost {
   namespace move_detail {

   template <class Ret, class T>
   inline typename ::boost::move_detail::enable_if_c
      <  ::boost::move_detail::is_lvalue_reference<Ret>::value ||
        !::boost::has_move_emulation_enabled<T>::value
      , T&>::type
         move_return(T& x) BOOST_NOEXCEPT
   {
      return x;
   }

   template <class Ret, class T>
   inline typename ::boost::move_detail::enable_if_c
      < !::boost::move_detail::is_lvalue_reference<Ret>::value &&
         ::boost::has_move_emulation_enabled<T>::value
      , ::boost::rv<T>&>::type
         move_return(T& x) BOOST_NOEXCEPT
   {
      return *static_cast< ::boost::rv<T>* >(::boost::move_detail::addressof(x));
   }

   template <class Ret, class T>
   inline typename ::boost::move_detail::enable_if_c
      < !::boost::move_detail::is_lvalue_reference<Ret>::value &&
         ::boost::has_move_emulation_enabled<T>::value
      , ::boost::rv<T>&>::type
         move_return(::boost::rv<T>& x) BOOST_NOEXCEPT
   {
      return x;
   }

   }  //namespace move_detail {
   }  //namespace boost {

   #define BOOST_MOVE_RET(RET_TYPE, REF)\
      boost::move_detail::move_return< RET_TYPE >(REF)
   //

   //////////////////////////////////////////////////////////////////////////////
   //
   //                         BOOST_MOVABLE_BUT_NOT_COPYABLE
   //
   //////////////////////////////////////////////////////////////////////////////
   #define BOOST_MOVABLE_BUT_NOT_COPYABLE(TYPE)\
      BOOST_MOVE_IMPL_NO_COPY_CTOR_OR_ASSIGN(TYPE)\
      public:\
      operator ::boost::rv<TYPE>&() \
      {  return *static_cast< ::boost::rv<TYPE>* >(this);  }\
      operator const ::boost::rv<TYPE>&() const \
      {  return *static_cast<const ::boost::rv<TYPE>* >(this);  }\
      private:\
   //

   //////////////////////////////////////////////////////////////////////////////
   //
   //                         BOOST_COPYABLE_AND_MOVABLE
   //
   //////////////////////////////////////////////////////////////////////////////

   #define BOOST_COPYABLE_AND_MOVABLE(TYPE)\
      public:\
      TYPE& operator=(TYPE &t)\
      {  this->operator=(static_cast<const ::boost::rv<TYPE> &>(const_cast<const TYPE &>(t))); return *this;}\
      public:\
      operator ::boost::rv<TYPE>&() \
      {  return *static_cast< ::boost::rv<TYPE>* >(this);  }\
      operator const ::boost::rv<TYPE>&() const \
      {  return *static_cast<const ::boost::rv<TYPE>* >(this);  }\
      private:\
   //

   #define BOOST_COPYABLE_AND_MOVABLE_ALT(TYPE)\
      public:\
      operator ::boost::rv<TYPE>&() \
      {  return *static_cast< ::boost::rv<TYPE>* >(this);  }\
      operator const ::boost::rv<TYPE>&() const \
      {  return *static_cast<const ::boost::rv<TYPE>* >(this);  }\
      private:\
   //

#else    //BOOST_NO_CXX11_RVALUE_REFERENCES

   //Compiler workaround detection
   #if !defined(BOOST_MOVE_DOXYGEN_INVOKED)
      #if defined(__GNUC__) && (__GNUC__ == 4) && (__GNUC_MINOR__ < 5) && !defined(__clang__)
         //Pre-standard rvalue binding rules
         #define BOOST_MOVE_OLD_RVALUE_REF_BINDING_RULES
      #elif defined(_MSC_VER) && (_MSC_VER == 1600)
         //Standard rvalue binding rules but with some bugs
         #define BOOST_MOVE_MSVC_10_MEMBER_RVALUE_REF_BUG
         #define BOOST_MOVE_MSVC_AUTO_MOVE_RETURN_BUG
         //Use standard library for MSVC to avoid namespace issues as
         //some move calls in the STL are not fully qualified.
         //#define BOOST_MOVE_USE_STANDARD_LIBRARY_MOVE
      #elif defined(_MSC_VER) && (_MSC_VER == 1700)
         #define BOOST_MOVE_MSVC_AUTO_MOVE_RETURN_BUG
      #endif
   #endif

   //! This macro marks a type as movable but not copyable, disabling copy construction
   //! and assignment. The user will need to write a move constructor/assignment as explained
   //! in the documentation to fully write a movable but not copyable class.
   #define BOOST_MOVABLE_BUT_NOT_COPYABLE(TYPE)\
      BOOST_MOVE_IMPL_NO_COPY_CTOR_OR_ASSIGN(TYPE)\
      public:\
      typedef int boost_move_emulation_t;\
   //

   //! This macro marks a type as copyable and movable.
   //! The user will need to write a move constructor/assignment and a copy assignment
   //! as explained in the documentation to fully write a copyable and movable class.
   #define BOOST_COPYABLE_AND_MOVABLE(TYPE)\
   //

   #if !defined(BOOST_MOVE_DOXYGEN_INVOKED)
   #define BOOST_COPYABLE_AND_MOVABLE_ALT(TYPE)\
   //
   #endif   //#if !defined(BOOST_MOVE_DOXYGEN_INVOKED)

   namespace boost {

   //!This trait yields to a compile-time true boolean if T was marked as
   //!BOOST_MOVABLE_BUT_NOT_COPYABLE or BOOST_COPYABLE_AND_MOVABLE and
   //!rvalue references are not available on the platform. False otherwise.
   template<class T>
   struct has_move_emulation_enabled
   {
      static const bool value = false;
   };

   }  //namespace boost{

   //!This macro is used to achieve portable syntax in move
   //!constructors and assignments for classes marked as
   //!BOOST_COPYABLE_AND_MOVABLE or BOOST_MOVABLE_BUT_NOT_COPYABLE
   #define BOOST_RV_REF(TYPE)\
      TYPE && \
   //

   //!This macro is used to achieve portable syntax in move
   //!constructors and assignments for template classes marked as
   //!BOOST_COPYABLE_AND_MOVABLE or BOOST_MOVABLE_BUT_NOT_COPYABLE.
   //!As macros have problems with comma-separated template arguments,
   //!the template argument must be preceded with BOOST_RV_REF_BEG
   //!and ended with BOOST_RV_REF_END
   #define BOOST_RV_REF_BEG\
         \
   //

   //!This macro is used to achieve portable syntax in move
   //!constructors and assignments for template classes marked as
   //!BOOST_COPYABLE_AND_MOVABLE or BOOST_MOVABLE_BUT_NOT_COPYABLE.
   //!As macros have problems with comma-separated template arguments,
   //!the template argument must be preceded with BOOST_RV_REF_BEG
   //!and ended with BOOST_RV_REF_END
   #define BOOST_RV_REF_END\
      && \

   //!This macro is used to achieve portable syntax in copy
   //!assignment for classes marked as BOOST_COPYABLE_AND_MOVABLE.
   #define BOOST_COPY_ASSIGN_REF(TYPE)\
      const TYPE & \
   //

   //! This macro is used to implement portable perfect forwarding
   //! as explained in the documentation.
   #define BOOST_FWD_REF(TYPE)\
      TYPE && \
   //

   #if !defined(BOOST_MOVE_DOXYGEN_INVOKED)

   #define BOOST_RV_REF_2_TEMPL_ARGS(TYPE, ARG1, ARG2)\
      TYPE<ARG1, ARG2> && \
   //

   #define BOOST_RV_REF_3_TEMPL_ARGS(TYPE, ARG1, ARG2, ARG3)\
      TYPE<ARG1, ARG2, ARG3> && \
   //

   #define BOOST_COPY_ASSIGN_REF_BEG \
      const \
   //

   #define BOOST_COPY_ASSIGN_REF_END \
      & \
   //

   #define BOOST_COPY_ASSIGN_REF_2_TEMPL_ARGS(TYPE, ARG1, ARG2)\
      const TYPE<ARG1, ARG2> & \
   //

   #define BOOST_COPY_ASSIGN_REF_3_TEMPL_ARGS(TYPE, ARG1, ARG2, ARG3)\
      const TYPE<ARG1, ARG2, ARG3>& \
   //

   #define BOOST_CATCH_CONST_RLVALUE(TYPE)\
      const TYPE & \
   //


   #endif   //#if !defined(BOOST_MOVE_DOXYGEN_INVOKED)

   #if !defined(BOOST_MOVE_MSVC_AUTO_MOVE_RETURN_BUG) || defined(BOOST_MOVE_DOXYGEN_INVOKED)

      //!This macro is used to achieve portable move return semantics.
      //!The Standard allows implicit move returns when the object to be returned 
      //!is designated by an lvalue and:
      //!   - The criteria for elision of a copy operation are met OR
      //!   - The criteria would be met save for the fact that the source object is a function parameter
      //!
      //!For C++11 conforming compilers this macros only yields to REF:
      //! <code>return BOOST_MOVE_RET(RET_TYPE, REF);</code> -> <code>return REF;</code>
      //!
      //!For compilers without rvalue references
      //!this macro does an explicit move if the move emulation is activated
      //!and the return type (RET_TYPE) is not a reference.
      //!
      //!For non-conforming compilers with rvalue references like Visual 2010 & 2012,
      //!an explicit move is performed if RET_TYPE is not a reference.
      //!
      //! <b>Caution</b>: When using this macro in a non-conforming or C++03
      //!compilers, a move will be performed even if the C++11 standard does not allow it
      //!(e.g. returning a static variable). The user is responsible for using this macro
      //!only used to return local objects that met C++11 criteria.
      #define BOOST_MOVE_RET(RET_TYPE, REF)\
         (REF)
      //

   #else //!defined(BOOST_MOVE_MSVC_AUTO_MOVE_RETURN_BUG) || defined(BOOST_MOVE_DOXYGEN_INVOKED)

      #include <boost/move/detail/meta_utils.hpp>

      namespace boost {
      namespace move_detail {

      template <class Ret, class T>
      inline typename ::boost::move_detail::enable_if_c
         <  ::boost::move_detail::is_lvalue_reference<Ret>::value
         , T&>::type
            move_return(T& x) BOOST_NOEXCEPT
      {
         return x;
      }

      template <class Ret, class T>
      inline typename ::boost::move_detail::enable_if_c
         < !::boost::move_detail::is_lvalue_reference<Ret>::value
         , Ret && >::type
            move_return(T&& t) BOOST_NOEXCEPT
      {
         return static_cast< Ret&& >(t);
      }

      }  //namespace move_detail {
      }  //namespace boost {

      #define BOOST_MOVE_RET(RET_TYPE, REF)\
         boost::move_detail::move_return< RET_TYPE >(REF)
      //

   #endif   //!defined(BOOST_MOVE_MSVC_AUTO_MOVE_RETURN_BUG) || defined(BOOST_MOVE_DOXYGEN_INVOKED)

#endif   //BOOST_NO_CXX11_RVALUE_REFERENCES

#include <boost/move/detail/config_end.hpp>

#endif //#ifndef BOOST_MOVE_CORE_HPP
