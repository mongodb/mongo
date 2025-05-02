//
//  sp_debug_hooks.cpp
//
//  Copyright (c) 2002, 2003 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)

#include <boost/assert.hpp>
#include <new>
#include <cstdlib>

int const m = 2; // m * sizeof(int) must be aligned appropriately

// magic values to mark heap blocks with

int const allocated_scalar  = 0x1234560C;
int const allocated_array   = 0x1234560A;
int const adopted_scalar    = 0x0567890C;
int const adopted_array     = 0x0567890A;
int const deleted           = 0x498769DE;

using namespace std; // for compilers where things aren't in std

// operator new

static new_handler get_new_handler()
{
    new_handler p = set_new_handler(0);
    set_new_handler(p);
    return p;
}

static void * allocate(size_t n, int mark)
{
    int * pm;

    for(;;)
    {
        pm = static_cast<int*>(malloc(n + m * sizeof(int)));

        if(pm != 0) break;

        if(new_handler pnh = get_new_handler())
        {
            pnh();
        }
        else
        {
            return 0;
        }
    }

    *pm = mark;

    return pm + m;
}

void * operator new(size_t n) throw(bad_alloc)
{
    void * p = allocate(n, allocated_scalar);

#if !defined(BOOST_NO_EXCEPTIONS)

    if(p == 0) throw bad_alloc();

#endif

    return p;
}

void * operator new(size_t n, nothrow_t const &) throw()
{
    return allocate(n, allocated_scalar);
}

void * operator new[](size_t n) throw(bad_alloc)
{
    void * p = allocate(n, allocated_array);

#if !defined(BOOST_NO_EXCEPTIONS)

    if(p == 0) throw bad_alloc();

#endif

    return p;
}

void * operator new[](size_t n, nothrow_t const &) throw()
{
    return allocate(n, allocated_array);
}

// debug hooks

namespace boost
{

void sp_scalar_constructor_hook(void * p)
{
    if(p == 0) return;

    int * pm = static_cast<int*>(p);
    pm -= m;

    BOOST_ASSERT(*pm != adopted_scalar);    // second smart pointer to the same address
    BOOST_ASSERT(*pm != allocated_array);   // allocated with new[]
    BOOST_ASSERT(*pm == allocated_scalar);  // not allocated with new

    *pm = adopted_scalar;
}

void sp_scalar_constructor_hook(void * px, std::size_t, void *)
{
    sp_scalar_constructor_hook(px);
}

void sp_scalar_destructor_hook(void * p)
{
    if(p == 0) return;

    int * pm = static_cast<int*>(p);
    pm -= m;

    BOOST_ASSERT(*pm == adopted_scalar);    // attempt to destroy nonmanaged block

    *pm = allocated_scalar;
}

void sp_scalar_destructor_hook(void * px, std::size_t, void *)
{
    sp_scalar_destructor_hook(px);
}

// It is not possible to handle the array hooks in a portable manner.
// The implementation typically reserves a bit of storage for the number
// of objects in the array, and the argument of the array hook isn't
// equal to the return value of operator new[].

void sp_array_constructor_hook(void * /* p */)
{
/*
    if(p == 0) return;

    // adjust p depending on the implementation

    int * pm = static_cast<int*>(p);
    pm -= m;

    BOOST_ASSERT(*pm != adopted_array);     // second smart array pointer to the same address
    BOOST_ASSERT(*pm != allocated_scalar);  // allocated with new
    BOOST_ASSERT(*pm == allocated_array);   // not allocated with new[]

    *pm = adopted_array;
*/
}

void sp_array_destructor_hook(void * /* p */)
{
/*
    if(p == 0) return;

    // adjust p depending on the implementation

    int * pm = static_cast<int*>(p);
    pm -= m;

    BOOST_ASSERT(*pm == adopted_array); // attempt to destroy nonmanaged block

    *pm = allocated_array;
*/
}

} // namespace boost

// operator delete

void operator delete(void * p) throw()
{
    if(p == 0) return;

    int * pm = static_cast<int*>(p);
    pm -= m;

    BOOST_ASSERT(*pm != deleted);           // double delete
    BOOST_ASSERT(*pm != adopted_scalar);    // delete p.get();
    BOOST_ASSERT(*pm != allocated_array);   // allocated with new[]
    BOOST_ASSERT(*pm == allocated_scalar);  // not allocated with new

    *pm = deleted;

    free(pm);
}

void operator delete(void * p, nothrow_t const &) throw()
{
    ::operator delete(p);
}

void operator delete[](void * p) throw()
{
    if(p == 0) return;

    int * pm = static_cast<int*>(p);
    pm -= m;

    BOOST_ASSERT(*pm != deleted);           // double delete
    BOOST_ASSERT(*pm != adopted_scalar);    // delete p.get();
    BOOST_ASSERT(*pm != allocated_scalar);  // allocated with new
    BOOST_ASSERT(*pm == allocated_array);   // not allocated with new[]

    *pm = deleted;

    free(pm);
}

void operator delete[](void * p, nothrow_t const &) throw()
{
    ::operator delete[](p);
}

#endif // defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
