#ifndef BOOST_ENABLE_SHARED_FROM_THIS_HPP_INCLUDED
#define BOOST_ENABLE_SHARED_FROM_THIS_HPP_INCLUDED

//
//  enable_shared_from_this.hpp
//
//  Copyright (c) 2002 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
//  http://www.boost.org/libs/smart_ptr/enable_shared_from_this.html
//

#include <boost/weak_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/assert.hpp>
#include <boost/config.hpp>

namespace boost
{

template<class T> class enable_shared_from_this
{
protected:

    enable_shared_from_this()
    {
    }

    enable_shared_from_this(enable_shared_from_this const &)
    {
    }

    enable_shared_from_this & operator=(enable_shared_from_this const &)
    {
        return *this;
    }

    ~enable_shared_from_this()
    {
    }

public:

    shared_ptr<T> shared_from_this()
    {
        shared_ptr<T> p(_internal_weak_this);
        BOOST_ASSERT(p.get() == this);
        return p;
    }

    shared_ptr<T const> shared_from_this() const
    {
        shared_ptr<T const> p(_internal_weak_this);
        BOOST_ASSERT(p.get() == this);
        return p;
    }

//  Note: No, you don't need to initialize _internal_weak_this
//
//  Please read the documentation, not the code
//
//  http://www.boost.org/libs/smart_ptr/enable_shared_from_this.html

    typedef T _internal_element_type; // for bcc 5.5.1
    mutable weak_ptr<_internal_element_type> _internal_weak_this;
};

} // namespace boost

#endif  // #ifndef BOOST_ENABLE_SHARED_FROM_THIS_HPP_INCLUDED
