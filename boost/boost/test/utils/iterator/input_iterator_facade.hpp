//  (C) Copyright Gennadiy Rozental 2004-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: input_iterator_facade.hpp,v $
//
//  Version     : $Revision: 1.5 $
//
//  Description : Input iterator facade 
// ***************************************************************************

#ifndef BOOST_INPUT_ITERATOR_FACADE_HPP_071894GER
#define BOOST_INPUT_ITERATOR_FACADE_HPP_071894GER

// Boost
#include <boost/iterator/iterator_facade.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************          input_iterator_core_access          ************** //
// ************************************************************************** //

class input_iterator_core_access
{
#if defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS) || BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x551))
public:
#else
    template <class I, class V, class R, class TC> friend class input_iterator_facade;
#endif

    template <class Facade>
    static bool get( Facade& f )
    {
        return f.get();
    }

private:
    // objects of this class are useless
    input_iterator_core_access(); //undefined
};

// ************************************************************************** //
// **************            input_iterator_facade             ************** //
// ************************************************************************** //

template<typename Derived,
         typename ValueType,
         typename Reference = ValueType const&,
         typename Traversal = single_pass_traversal_tag>
class input_iterator_facade : public iterator_facade<Derived,ValueType,Traversal,Reference>
{
public:
    // Constructor
    input_iterator_facade() : m_valid( false ), m_value() {}

protected: // provide access to the Derived
    void                init()
    {
        m_valid = true;
        increment();
    }

    // Data members
    mutable bool        m_valid;
    ValueType           m_value;

private:
    friend class boost::iterator_core_access;

    // iterator facade interface implementation
    void                increment()
    {
        // we make post-end incrementation indefinetly safe 
        if( m_valid )
            m_valid = input_iterator_core_access::get( *static_cast<Derived*>(this) );
    }
    Reference           dereference() const
    {
        return m_value;
    }

    // iterator facade interface implementation
    bool                equal( input_iterator_facade const& rhs ) const
    {
        // two invalid iterator equals, inequal otherwise
        return !m_valid && !rhs.m_valid;
    }
};

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: input_iterator_facade.hpp,v $
//  Revision 1.5  2005/05/08 08:55:09  rogeeff
//  typos and missing descriptions fixed
//
//  Revision 1.4  2005/04/12 06:47:46  rogeeff
//  help iterator copying
//
//  Revision 1.3  2005/02/20 08:27:09  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.2  2005/02/01 06:40:08  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.1  2005/01/22 18:21:40  rogeeff
//  moved sharable staff into utils
//
// ***************************************************************************

#endif // BOOST_INPUT_ITERATOR_FACADE_HPP_071894GER

