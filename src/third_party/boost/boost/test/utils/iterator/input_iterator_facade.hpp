//  (C) Copyright Gennadiy Rozental 2004-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision: 49312 $
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

#endif // BOOST_INPUT_ITERATOR_FACADE_HPP_071894GER

