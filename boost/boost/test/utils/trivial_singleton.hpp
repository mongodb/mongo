//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: trivial_singleton.hpp,v $
//
//  Version     : $Revision: 1.3 $
//
//  Description : simple helpers for creating cusom output manipulators
// ***************************************************************************

#ifndef BOOST_TEST_TRIVIAL_SIGNLETON_HPP_020505GER
#define BOOST_TEST_TRIVIAL_SIGNLETON_HPP_020505GER

#include <boost/noncopyable.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************                   singleton                  ************** //
// ************************************************************************** //

template<typename Derived>
class singleton : private boost::noncopyable {
public:
    static Derived& instance() { static Derived the_inst; return the_inst; }    
protected:
    singleton()  {}
    ~singleton() {}
};

} // namespace unit_test

#define BOOST_TEST_SINGLETON_CONS( type )       \
friend class boost::unit_test::singleton<type>; \
type() {}                                       \
/**/

#if BOOST_WORKAROUND(__DECCXX_VER, BOOST_TESTED_AT(60590042))

#define BOOST_TEST_SINGLETON_INST( inst ) \
template class unit_test::singleton< BOOST_JOIN( inst, _t ) > ; \
namespace { BOOST_JOIN( inst, _t)& inst = BOOST_JOIN( inst, _t)::instance(); }

#elif defined(__APPLE_CC__) && defined(__GNUC__) && __GNUC__ < 4
#define BOOST_TEST_SINGLETON_INST( inst ) \
static BOOST_JOIN( inst, _t)& inst = BOOST_JOIN (inst, _t)::instance();

#else

#define BOOST_TEST_SINGLETON_INST( inst ) \
namespace { BOOST_JOIN( inst, _t)& inst = BOOST_JOIN( inst, _t)::instance(); }

#endif

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: trivial_singleton.hpp,v $
//  Revision 1.3  2006/01/01 17:29:38  dgregor
//  Work around anonymous namespace bug in Apple GCC 3.3
//
//  Revision 1.2  2005/06/15 07:21:51  schoepflin
//  Tru64 needs an explicit instantiation of the singleton template. Otherwise we
//  end up with multiple singleton instances.
//
//  Revision 1.1  2005/02/20 08:27:08  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_TEST_TRIVIAL_SIGNLETON_HPP_020505GER
