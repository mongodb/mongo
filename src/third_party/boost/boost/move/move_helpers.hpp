//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2010-2011.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/move for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_MOVE_MOVE_HELPERS_HPP
#define BOOST_MOVE_MOVE_HELPERS_HPP

#include <boost/move/move.hpp>
#include <boost/type_traits/is_class.hpp>

#if defined(BOOST_NO_RVALUE_REFERENCES) || (defined(_MSC_VER) && (_MSC_VER == 1600))
#include <boost/type_traits/is_same.hpp>
#include <boost/utility/enable_if.hpp>
#endif
#if defined(BOOST_NO_RVALUE_REFERENCES) 
#include <boost/mpl/if.hpp>
#endif


#if defined(BOOST_NO_RVALUE_REFERENCES)
struct not_a_type;
#define BOOST_MOVE_CATCH_CONST(U)  \
   typename ::boost::mpl::if_< ::boost::is_class<U>, BOOST_CATCH_CONST_RLVALUE(U), const U &>::type
#define BOOST_MOVE_CATCH_RVALUE(U)\
   typename ::boost::mpl::if_< ::boost::is_class<U>, BOOST_RV_REF(U), not_a_type>::type
#define BOOST_MOVE_CATCH_FWD(U) BOOST_FWD_REF(U)
#else
#define BOOST_MOVE_CATCH_CONST(U)  const U &
#define BOOST_MOVE_CATCH_RVALUE(U) U &&
#define BOOST_MOVE_CATCH_FWD(U)    U &&
#endif

#ifdef BOOST_NO_RVALUE_REFERENCES

#define BOOST_MOVE_CONVERSION_AWARE_CATCH(PUB_FUNCTION, TYPE, RETURN_VALUE, FWD_FUNCTION)\
   RETURN_VALUE PUB_FUNCTION(BOOST_MOVE_CATCH_CONST(TYPE) x)\
   {  return FWD_FUNCTION(static_cast<const TYPE&>(x)); }\
\
   RETURN_VALUE PUB_FUNCTION(BOOST_MOVE_CATCH_RVALUE(TYPE) x) \
   {  return FWD_FUNCTION(::boost::move(x));  }\
\
   RETURN_VALUE PUB_FUNCTION(TYPE &x)\
   {  return FWD_FUNCTION(const_cast<const TYPE &>(x)); }\
\
   template<class BOOST_MOVE_TEMPL_PARAM>\
   typename ::boost::enable_if_c\
                     <  ::boost::is_class<TYPE>::value &&\
                        ::boost::is_same<TYPE, BOOST_MOVE_TEMPL_PARAM>::value &&\
                       !::boost::has_move_emulation_enabled<BOOST_MOVE_TEMPL_PARAM>::value\
                     , RETURN_VALUE >::type\
   PUB_FUNCTION(const BOOST_MOVE_TEMPL_PARAM &u)\
   { return FWD_FUNCTION(u); }\
\
   template<class BOOST_MOVE_TEMPL_PARAM>\
   typename ::boost::enable_if_c\
                     < (!::boost::is_class<BOOST_MOVE_TEMPL_PARAM>::value || \
                        !::boost::move_detail::is_rv<BOOST_MOVE_TEMPL_PARAM>::value) && \
                       !::boost::is_same<TYPE, BOOST_MOVE_TEMPL_PARAM>::value \
                     , RETURN_VALUE >::type\
   PUB_FUNCTION(const BOOST_MOVE_TEMPL_PARAM &u)\
   {\
      TYPE t(u);\
      return FWD_FUNCTION(::boost::move(t));\
   }\
//

#elif (defined(_MSC_VER) && (_MSC_VER == 1600))

#define BOOST_MOVE_CONVERSION_AWARE_CATCH(PUB_FUNCTION, TYPE, RETURN_VALUE, FWD_FUNCTION)\
   RETURN_VALUE PUB_FUNCTION(BOOST_MOVE_CATCH_CONST(TYPE) x)\
   {  return FWD_FUNCTION(static_cast<const TYPE&>(x)); }\
\
   RETURN_VALUE PUB_FUNCTION(BOOST_MOVE_CATCH_RVALUE(TYPE) x) \
   {  return FWD_FUNCTION(::boost::move(x));  }\
\
   template<class BOOST_MOVE_TEMPL_PARAM>\
   typename ::boost::enable_if_c\
                     <  !::boost::is_same<TYPE, BOOST_MOVE_TEMPL_PARAM>::value\
                     , RETURN_VALUE >::type\
   PUB_FUNCTION(const BOOST_MOVE_TEMPL_PARAM &u)\
   {\
      TYPE t(u);\
      return FWD_FUNCTION(::boost::move(t));\
   }\
//

#else

#define BOOST_MOVE_CONVERSION_AWARE_CATCH(PUB_FUNCTION, TYPE, RETURN_VALUE, FWD_FUNCTION)\
   RETURN_VALUE PUB_FUNCTION(BOOST_MOVE_CATCH_CONST(TYPE) x)\
   {  return FWD_FUNCTION(static_cast<const TYPE&>(x)); }\
\
   RETURN_VALUE PUB_FUNCTION(BOOST_MOVE_CATCH_RVALUE(TYPE) x) \
   {  return FWD_FUNCTION(::boost::move(x));  }\
//

#endif


#ifdef BOOST_NO_RVALUE_REFERENCES

#define BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(PUB_FUNCTION, TYPE, RETURN_VALUE, FWD_FUNCTION, ARG1)\
   RETURN_VALUE PUB_FUNCTION(ARG1 arg1, BOOST_MOVE_CATCH_CONST(TYPE) x)\
   {  return FWD_FUNCTION(arg1, static_cast<const TYPE&>(x)); }\
\
   RETURN_VALUE PUB_FUNCTION(ARG1 arg1, BOOST_MOVE_CATCH_RVALUE(TYPE) x) \
   {  return FWD_FUNCTION(arg1, ::boost::move(x));  }\
\
   RETURN_VALUE PUB_FUNCTION(ARG1 arg1, TYPE &x)\
   {  return FWD_FUNCTION(arg1, const_cast<const TYPE &>(x)); }\
\
   template<class BOOST_MOVE_TEMPL_PARAM>\
   typename ::boost::enable_if_c\
                     <  ::boost::is_class<TYPE>::value &&\
                        ::boost::is_same<TYPE, BOOST_MOVE_TEMPL_PARAM>::value &&\
                       !::boost::has_move_emulation_enabled<BOOST_MOVE_TEMPL_PARAM>::value\
                     , RETURN_VALUE >::type\
   PUB_FUNCTION(ARG1 arg1, const BOOST_MOVE_TEMPL_PARAM &u)\
   { return FWD_FUNCTION(arg1, u); }\
\
   template<class BOOST_MOVE_TEMPL_PARAM>\
   typename ::boost::enable_if_c\
                     < (!::boost::is_class<BOOST_MOVE_TEMPL_PARAM>::value || \
                        !::boost::move_detail::is_rv<BOOST_MOVE_TEMPL_PARAM>::value) && \
                       !::boost::is_same<TYPE, BOOST_MOVE_TEMPL_PARAM>::value \
                     , RETURN_VALUE >::type\
   PUB_FUNCTION(ARG1 arg1, const BOOST_MOVE_TEMPL_PARAM &u)\
   {\
      TYPE t(u);\
      return FWD_FUNCTION(arg1, ::boost::move(t));\
   }\
//

#elif (defined(_MSC_VER) && (_MSC_VER == 1600))

#define BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(PUB_FUNCTION, TYPE, RETURN_VALUE, FWD_FUNCTION, ARG1)\
   RETURN_VALUE PUB_FUNCTION(ARG1 arg1, BOOST_MOVE_CATCH_CONST(TYPE) x)\
   {  return FWD_FUNCTION(arg1, static_cast<const TYPE&>(x)); }\
\
   RETURN_VALUE PUB_FUNCTION(ARG1 arg1, BOOST_MOVE_CATCH_RVALUE(TYPE) x) \
   {  return FWD_FUNCTION(arg1, ::boost::move(x));  }\
\
   template<class BOOST_MOVE_TEMPL_PARAM>\
   typename ::boost::enable_if_c\
                     <  !::boost::is_same<TYPE, BOOST_MOVE_TEMPL_PARAM>::value\
                     , RETURN_VALUE >::type\
   PUB_FUNCTION(ARG1 arg1, const BOOST_MOVE_TEMPL_PARAM &u)\
   {\
      TYPE t(u);\
      return FWD_FUNCTION(arg1, ::boost::move(t));\
   }\
//

#else

#define BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(PUB_FUNCTION, TYPE, RETURN_VALUE, FWD_FUNCTION, ARG1)\
   RETURN_VALUE PUB_FUNCTION(ARG1 arg1, BOOST_MOVE_CATCH_CONST(TYPE) x)\
   {  return FWD_FUNCTION(arg1, static_cast<const TYPE&>(x)); }\
\
   RETURN_VALUE PUB_FUNCTION(ARG1 arg1, BOOST_MOVE_CATCH_RVALUE(TYPE) x) \
   {  return FWD_FUNCTION(arg1, ::boost::move(x));  }\
//

#endif

#endif //#ifndef BOOST_MOVE_MOVE_HELPERS_HPP
