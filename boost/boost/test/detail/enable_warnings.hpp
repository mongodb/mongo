//  (C) Copyright Gennadiy Rozental 2004-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: enable_warnings.hpp,v $
//
//  Version     : $Revision: 1.6 $
//
//  Description : enable previosly suppressed warnings
// ***************************************************************************

#ifdef BOOST_MSVC
# pragma warning(default: 4511) // copy constructor could not be generated
# pragma warning(default: 4512) // assignment operator could not be generated
# pragma warning(default: 4100) // unreferenced formal parameter 
# pragma warning(default: 4996) // <symbol> was declared deprecated 
# pragma warning(default: 4355) // 'this' : used in base member initializer list
# pragma warning(default: 4706) // assignment within conditional expression
# pragma warning(default: 4251) // class 'A<T>' needs to have dll-interface to be used by clients of class 'B'
# pragma warning(default: 4127) // conditional expression is constant
# pragma warning(default: 4290) // C++ exception specification ignored except to ...
# pragma warning(default: 4180) // qualifier applied to function type has no meaning; ignored
# pragma warning(pop)
#endif

// ***************************************************************************
//  Revision History :
//  
//  $Log: enable_warnings.hpp,v $
//  Revision 1.6  2006/01/28 07:09:08  rogeeff
//  4180 suppressed
//
//  Revision 1.5  2005/12/14 04:57:50  rogeeff
//  extra warnings suppressed
//
//  Revision 1.4  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.3  2005/02/01 06:40:07  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.2  2005/01/31 06:00:37  rogeeff
//  deprecated std symbols warning suppressed
//
// ***************************************************************************
