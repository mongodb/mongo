//  (C) Copyright Gennadiy Rozental 2005-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision$
//
//  Description : overloadable assignment
// ***************************************************************************

#ifndef BOOST_TEST_ASSIGN_OP_033005GER
#define BOOST_TEST_ASSIGN_OP_033005GER

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************             generic assign operator          ************** //
// ************************************************************************** //

// generic
template<typename T,typename S>
inline void
assign_op( T& t, S const& s, long )
{
    t = s;
}

//____________________________________________________________________________//

} // namespace unit_test

} // namespace boost

#endif // BOOST_TEST_ASSIGN_OP_033005GER

