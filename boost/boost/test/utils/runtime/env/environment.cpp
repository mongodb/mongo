//  (C) Copyright Gennadiy Rozental 2004.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: environment.cpp,v $
//
//  Version     : $Revision: 1.1 $
//
//  Description : implements offline model of program environment 
// ***************************************************************************

#include <boost/test/utils/runtime/config.hpp>

#ifdef BOOST_MSVC
# pragma warning(disable: 4127) // conditional expression is constant
# pragma warning(disable: 4701) // local environment 'result' may be used without having been initialized
#endif

#define BOOST_RT_PARAM_INLINE
#include <boost/test/utils/runtime/env/environment.ipp>

// ************************************************************************** //
//   Revision History:
//
//   $Log: environment.cpp,v $
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //
