// Copyright (C) 2000, 2001 Stephen Cleary
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_OBJECT_POOL_HPP
#define BOOST_OBJECT_POOL_HPP

#include <boost/pool/poolfwd.hpp>

// boost::pool
#include <boost/pool/pool.hpp>

// The following code will be put into Boost.Config in a later revision
#if defined(BOOST_MSVC) || defined(__KCC)
# define BOOST_NO_TEMPLATE_CV_REF_OVERLOADS
#endif

// The following code might be put into some Boost.Config header in a later revision
#ifdef __BORLANDC__
# pragma option push -w-inl
#endif

// There are a few places in this file where the expression "this->m" is used.
// This expression is used to force instantiation-time name lookup, which I am
//   informed is required for strict Standard compliance.  It's only necessary
//   if "m" is a member of a base class that is dependent on a template
//   parameter.
// Thanks to Jens Maurer for pointing this out!

namespace boost {

// T must have a non-throwing destructor
template <typename T, typename UserAllocator>
class object_pool: protected pool<UserAllocator>
{
  public:
    typedef T element_type;
    typedef UserAllocator user_allocator;
    typedef typename pool<UserAllocator>::size_type size_type;
    typedef typename pool<UserAllocator>::difference_type difference_type;

  protected:
    pool<UserAllocator> & store() { return *this; }
    const pool<UserAllocator> & store() const { return *this; }

    // for the sake of code readability :)
    static void * & nextof(void * const ptr)
    { return *(static_cast<void **>(ptr)); }

  public:
    // This constructor parameter is an extension!
    explicit object_pool(const size_type next_size = 32)
    :pool<UserAllocator>(sizeof(T), next_size) { }

    ~object_pool();

    // Returns 0 if out-of-memory
    element_type * malloc()
    { return static_cast<element_type *>(store().ordered_malloc()); }
    void free(element_type * const chunk)
    { store().ordered_free(chunk); }
    bool is_from(element_type * const chunk) const
    { return store().is_from(chunk); }

    element_type * construct()
    {
      element_type * const ret = malloc();
      if (ret == 0)
        return ret;
      try { new (ret) element_type(); }
      catch (...) { free(ret); throw; }
      return ret;
    }

    // Include automatically-generated file for family of template construct()
    //  functions
#ifndef BOOST_NO_TEMPLATE_CV_REF_OVERLOADS
#   include <boost/pool/detail/pool_construct.inc>
#else
#   include <boost/pool/detail/pool_construct_simple.inc>
#endif

    void destroy(element_type * const chunk)
    {
      chunk->~T();
      free(chunk);
    }

    // These functions are extensions!
    size_type get_next_size() const { return store().get_next_size(); }
    void set_next_size(const size_type x) { store().set_next_size(x); }
};

template <typename T, typename UserAllocator>
object_pool<T, UserAllocator>::~object_pool()
{
  // handle trivial case
  if (!this->list.valid())
    return;

  details::PODptr<size_type> iter = this->list;
  details::PODptr<size_type> next = iter;

  // Start 'freed_iter' at beginning of free list
  void * freed_iter = this->first;

  const size_type partition_size = this->alloc_size();

  do
  {
    // increment next
    next = next.next();

    // delete all contained objects that aren't freed

    // Iterate 'i' through all chunks in the memory block
    for (char * i = iter.begin(); i != iter.end(); i += partition_size)
    {
      // If this chunk is free
      if (i == freed_iter)
      {
        // Increment freed_iter to point to next in free list
        freed_iter = nextof(freed_iter);

        // Continue searching chunks in the memory block
        continue;
      }

      // This chunk is not free (allocated), so call its destructor
      static_cast<T *>(static_cast<void *>(i))->~T();
      // and continue searching chunks in the memory block
    }

    // free storage
    UserAllocator::free(iter.begin());

    // increment iter
    iter = next;
  } while (iter.valid());

  // Make the block list empty so that the inherited destructor doesn't try to
  //  free it again.
  this->list.invalidate();
}

} // namespace boost

// The following code might be put into some Boost.Config header in a later revision
#ifdef __BORLANDC__
# pragma option pop
#endif

#endif
