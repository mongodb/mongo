#ifndef BOOST_SCOPED_PTR_HPP_INCLUDED
#define BOOST_SCOPED_PTR_HPP_INCLUDED

//  (C) Copyright Greg Colvin and Beman Dawes 1998, 1999.
//  Copyright (c) 2001, 2002 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  http://www.boost.org/libs/smart_ptr/scoped_ptr.htm
//

#include <boost/assert.hpp>
#include <boost/checked_delete.hpp>
#include <boost/detail/workaround.hpp>

#ifndef BOOST_NO_AUTO_PTR
# include <memory>          // for std::auto_ptr
#endif

namespace boost
{

// Debug hooks

#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)

void sp_scalar_constructor_hook(void * p);
void sp_scalar_destructor_hook(void * p);

#endif

//  scoped_ptr mimics a built-in pointer except that it guarantees deletion
//  of the object pointed to, either on destruction of the scoped_ptr or via
//  an explicit reset(). scoped_ptr is a simple solution for simple needs;
//  use shared_ptr or std::auto_ptr if your needs are more complex.

template<class T> class scoped_ptr // noncopyable
{
private:

    T * ptr;

    scoped_ptr(scoped_ptr const &);
    scoped_ptr & operator=(scoped_ptr const &);

    typedef scoped_ptr<T> this_type;

public:

    typedef T element_type;

    explicit scoped_ptr(T * p = 0): ptr(p) // never throws
    {
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        boost::sp_scalar_constructor_hook(ptr);
#endif
    }

#ifndef BOOST_NO_AUTO_PTR

    explicit scoped_ptr(std::auto_ptr<T> p): ptr(p.release()) // never throws
    {
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        boost::sp_scalar_constructor_hook(ptr);
#endif
    }

#endif

    ~scoped_ptr() // never throws
    {
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        boost::sp_scalar_destructor_hook(ptr);
#endif
        boost::checked_delete(ptr);
    }

    void reset(T * p = 0) // never throws
    {
        BOOST_ASSERT(p == 0 || p != ptr); // catch self-reset errors
        this_type(p).swap(*this);
    }

    T & operator*() const // never throws
    {
        BOOST_ASSERT(ptr != 0);
        return *ptr;
    }

    T * operator->() const // never throws
    {
        BOOST_ASSERT(ptr != 0);
        return ptr;
    }

    T * get() const // never throws
    {
        return ptr;
    }

    // implicit conversion to "bool"

#if defined(__SUNPRO_CC) && BOOST_WORKAROUND(__SUNPRO_CC, <= 0x530)

    operator bool () const
    {
        return ptr != 0;
    }

#elif defined(__MWERKS__) && BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3003))
    typedef T * (this_type::*unspecified_bool_type)() const;
    
    operator unspecified_bool_type() const // never throws
    {
        return ptr == 0? 0: &this_type::get;
    }

#else 
    typedef T * this_type::*unspecified_bool_type;

    operator unspecified_bool_type() const // never throws
    {
        return ptr == 0? 0: &this_type::ptr;
    }

#endif

    bool operator! () const // never throws
    {
        return ptr == 0;
    }

    void swap(scoped_ptr & b) // never throws
    {
        T * tmp = b.ptr;
        b.ptr = ptr;
        ptr = tmp;
    }
};

template<class T> inline void swap(scoped_ptr<T> & a, scoped_ptr<T> & b) // never throws
{
    a.swap(b);
}

// get_pointer(p) is a generic way to say p.get()

template<class T> inline T * get_pointer(scoped_ptr<T> const & p)
{
    return p.get();
}

} // namespace boost

#endif // #ifndef BOOST_SCOPED_PTR_HPP_INCLUDED
