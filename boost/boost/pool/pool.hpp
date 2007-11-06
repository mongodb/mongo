// Copyright (C) 2000, 2001 Stephen Cleary
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_POOL_HPP
#define BOOST_POOL_HPP

#include <boost/config.hpp>  // for workarounds

// std::less, std::less_equal, std::greater
#include <functional>
// new[], delete[], std::nothrow
#include <new>
// std::size_t, std::ptrdiff_t
#include <cstddef>
// std::malloc, std::free
#include <cstdlib>
// std::invalid_argument
#include <exception>
// std::max
#include <algorithm>

#include <boost/pool/poolfwd.hpp>

// boost::details::pool::ct_lcm
#include <boost/pool/detail/ct_gcd_lcm.hpp>
// boost::details::pool::lcm
#include <boost/pool/detail/gcd_lcm.hpp>
// boost::simple_segregated_storage
#include <boost/pool/simple_segregated_storage.hpp>

#ifdef BOOST_NO_STDC_NAMESPACE
 namespace std { using ::malloc; using ::free; }
#endif

// There are a few places in this file where the expression "this->m" is used.
// This expression is used to force instantiation-time name lookup, which I am
//   informed is required for strict Standard compliance.  It's only necessary
//   if "m" is a member of a base class that is dependent on a template
//   parameter.
// Thanks to Jens Maurer for pointing this out!

namespace boost {

struct default_user_allocator_new_delete
{
  typedef std::size_t size_type;
  typedef std::ptrdiff_t difference_type;

  static char * malloc(const size_type bytes)
  { return new (std::nothrow) char[bytes]; }
  static void free(char * const block)
  { delete [] block; }
};

struct default_user_allocator_malloc_free
{
  typedef std::size_t size_type;
  typedef std::ptrdiff_t difference_type;

  static char * malloc(const size_type bytes)
  { return reinterpret_cast<char *>(std::malloc(bytes)); }
  static void free(char * const block)
  { std::free(block); }
};

namespace details {

// PODptr is a class that pretends to be a "pointer" to different class types
//  that don't really exist.  It provides member functions to access the "data"
//  of the "object" it points to.  Since these "class" types are of variable
//  size, and contains some information at the *end* of its memory (for
//  alignment reasons), PODptr must contain the size of this "class" as well as
//  the pointer to this "object".
template <typename SizeType>
class PODptr
{
  public:
    typedef SizeType size_type;

  private:
    char * ptr;
    size_type sz;

    char * ptr_next_size() const
    { return (ptr + sz - sizeof(size_type)); }
    char * ptr_next_ptr() const
    {
      return (ptr_next_size() -
          pool::ct_lcm<sizeof(size_type), sizeof(void *)>::value);
    }

  public:
    PODptr(char * const nptr, const size_type nsize)
    :ptr(nptr), sz(nsize) { }
    PODptr()
    :ptr(0), sz(0) { }

    bool valid() const { return (begin() != 0); }
    void invalidate() { begin() = 0; }
    char * & begin() { return ptr; }
    char * begin() const { return ptr; }
    char * end() const { return ptr_next_ptr(); }
    size_type total_size() const { return sz; }
    size_type element_size() const
    {
      return (sz - sizeof(size_type) -
          pool::ct_lcm<sizeof(size_type), sizeof(void *)>::value);
    }

    size_type & next_size() const
    { return *(reinterpret_cast<size_type *>(ptr_next_size())); }
    char * & next_ptr() const
    { return *(reinterpret_cast<char **>(ptr_next_ptr())); }

    PODptr next() const
    { return PODptr<size_type>(next_ptr(), next_size()); }
    void next(const PODptr & arg) const
    {
      next_ptr() = arg.begin();
      next_size() = arg.total_size();
    }
};

} // namespace details

template <typename UserAllocator>
class pool: protected simple_segregated_storage<
    typename UserAllocator::size_type>
{
  public:
    typedef UserAllocator user_allocator;
    typedef typename UserAllocator::size_type size_type;
    typedef typename UserAllocator::difference_type difference_type;

  private:
    BOOST_STATIC_CONSTANT(unsigned, min_alloc_size =
        (::boost::details::pool::ct_lcm<sizeof(void *), sizeof(size_type)>::value) );

    // Returns 0 if out-of-memory
    // Called if malloc/ordered_malloc needs to resize the free list
    void * malloc_need_resize();
    void * ordered_malloc_need_resize();

  protected:
    details::PODptr<size_type> list;

    simple_segregated_storage<size_type> & store() { return *this; }
    const simple_segregated_storage<size_type> & store() const { return *this; }
    const size_type requested_size;
    size_type next_size;

    // finds which POD in the list 'chunk' was allocated from
    details::PODptr<size_type> find_POD(void * const chunk) const;

    // is_from() tests a chunk to determine if it belongs in a block
    static bool is_from(void * const chunk, char * const i,
        const size_type sizeof_i)
    {
      // We use std::less_equal and std::less to test 'chunk'
      //  against the array bounds because standard operators
      //  may return unspecified results.
      // This is to ensure portability.  The operators < <= > >= are only
      //  defined for pointers to objects that are 1) in the same array, or
      //  2) subobjects of the same object [5.9/2].
      // The functor objects guarantee a total order for any pointer [20.3.3/8]
//WAS:
//      return (std::less_equal<void *>()(static_cast<void *>(i), chunk)
//          && std::less<void *>()(chunk,
//              static_cast<void *>(i + sizeof_i)));
      std::less_equal<void *> lt_eq;
      std::less<void *> lt;
      return (lt_eq(i, chunk) && lt(chunk, i + sizeof_i));
    }

    size_type alloc_size() const
    {
      const unsigned min_size = min_alloc_size;
      return details::pool::lcm<size_type>(requested_size, min_size);
    }

    // for the sake of code readability :)
    static void * & nextof(void * const ptr)
    { return *(static_cast<void **>(ptr)); }

  public:
    // The second parameter here is an extension!
    // pre: npartition_size != 0 && nnext_size != 0
    explicit pool(const size_type nrequested_size,
        const size_type nnext_size = 32)
    :list(0, 0), requested_size(nrequested_size), next_size(nnext_size)
    { }

    ~pool() { purge_memory(); }

    // Releases memory blocks that don't have chunks allocated
    // pre: lists are ordered
    //  Returns true if memory was actually deallocated
    bool release_memory();

    // Releases *all* memory blocks, even if chunks are still allocated
    //  Returns true if memory was actually deallocated
    bool purge_memory();

    // These functions are extensions!
    size_type get_next_size() const { return next_size; }
    void set_next_size(const size_type nnext_size) { next_size = nnext_size; }

    // Both malloc and ordered_malloc do a quick inlined check first for any
    //  free chunks.  Only if we need to get another memory block do we call
    //  the non-inlined *_need_resize() functions.
    // Returns 0 if out-of-memory
    void * malloc()
    {
      // Look for a non-empty storage
      if (!store().empty())
        return store().malloc();
      return malloc_need_resize();
    }

    void * ordered_malloc()
    {
      // Look for a non-empty storage
      if (!store().empty())
        return store().malloc();
      return ordered_malloc_need_resize();
    }

    // Returns 0 if out-of-memory
    // Allocate a contiguous section of n chunks
    void * ordered_malloc(size_type n);

    // pre: 'chunk' must have been previously
    //        returned by *this.malloc().
    void free(void * const chunk)
    { store().free(chunk); }

    // pre: 'chunk' must have been previously
    //        returned by *this.malloc().
    void ordered_free(void * const chunk)
    { store().ordered_free(chunk); }

    // pre: 'chunk' must have been previously
    //        returned by *this.malloc(n).
    void free(void * const chunks, const size_type n)
    {
      const size_type partition_size = alloc_size();
      const size_type total_req_size = n * requested_size;
      const size_type num_chunks = total_req_size / partition_size +
          ((total_req_size % partition_size) ? true : false);

      store().free_n(chunks, num_chunks, partition_size);
    }

    // pre: 'chunk' must have been previously
    //        returned by *this.malloc(n).
    void ordered_free(void * const chunks, const size_type n)
    {
      const size_type partition_size = alloc_size();
      const size_type total_req_size = n * requested_size;
      const size_type num_chunks = total_req_size / partition_size +
          ((total_req_size % partition_size) ? true : false);

      store().ordered_free_n(chunks, num_chunks, partition_size);
    }

    // is_from() tests a chunk to determine if it was allocated from *this
    bool is_from(void * const chunk) const
    {
      return (find_POD(chunk).valid());
    }
};

template <typename UserAllocator>
bool pool<UserAllocator>::release_memory()
{
  // This is the return value: it will be set to true when we actually call
  //  UserAllocator::free(..)
  bool ret = false;

  // This is a current & previous iterator pair over the memory block list
  details::PODptr<size_type> ptr = list;
  details::PODptr<size_type> prev;

  // This is a current & previous iterator pair over the free memory chunk list
  //  Note that "prev_free" in this case does NOT point to the previous memory
  //  chunk in the free list, but rather the last free memory chunk before the
  //  current block.
  void * free = this->first;
  void * prev_free = 0;

  const size_type partition_size = alloc_size();

  // Search through all the all the allocated memory blocks
  while (ptr.valid())
  {
    // At this point:
    //  ptr points to a valid memory block
    //  free points to either:
    //    0 if there are no more free chunks
    //    the first free chunk in this or some next memory block
    //  prev_free points to either:
    //    the last free chunk in some previous memory block
    //    0 if there is no such free chunk
    //  prev is either:
    //    the PODptr whose next() is ptr
    //    !valid() if there is no such PODptr

    // If there are no more free memory chunks, then every remaining
    //  block is allocated out to its fullest capacity, and we can't
    //  release any more memory
    if (free == 0)
      return ret;

    // We have to check all the chunks.  If they are *all* free (i.e., present
    //  in the free list), then we can free the block.
    bool all_chunks_free = true;

    // Iterate 'i' through all chunks in the memory block
    // if free starts in the memory block, be careful to keep it there
    void * saved_free = free;
    for (char * i = ptr.begin(); i != ptr.end(); i += partition_size)
    {
      // If this chunk is not free
      if (i != free)
      {
        // We won't be able to free this block
        all_chunks_free = false;

        // free might have travelled outside ptr
        free = saved_free;
        // Abort searching the chunks; we won't be able to free this
        //  block because a chunk is not free.
        break;
      }

      // We do not increment prev_free because we are in the same block
      free = nextof(free);
    }

    // post: if the memory block has any chunks, free points to one of them
    // otherwise, our assertions above are still valid

    const details::PODptr<size_type> next = ptr.next();

    if (!all_chunks_free)
    {
      if (is_from(free, ptr.begin(), ptr.element_size()))
      {
        std::less<void *> lt;
        void * const end = ptr.end();
        do
        {
          prev_free = free;
          free = nextof(free);
        } while (free && lt(free, end));
      }
      // This invariant is now restored:
      //     free points to the first free chunk in some next memory block, or
      //       0 if there is no such chunk.
      //     prev_free points to the last free chunk in this memory block.
      
      // We are just about to advance ptr.  Maintain the invariant:
      // prev is the PODptr whose next() is ptr, or !valid()
      // if there is no such PODptr
      prev = ptr;
    }
    else
    {
      // All chunks from this block are free

      // Remove block from list
      if (prev.valid())
        prev.next(next);
      else
        list = next;

      // Remove all entries in the free list from this block
      if (prev_free != 0)
        nextof(prev_free) = free;
      else
        this->first = free;

      // And release memory
      UserAllocator::free(ptr.begin());
      ret = true;
    }

    // Increment ptr
    ptr = next;
  }

  return ret;
}

template <typename UserAllocator>
bool pool<UserAllocator>::purge_memory()
{
  details::PODptr<size_type> iter = list;

  if (!iter.valid())
    return false;

  do
  {
    // hold "next" pointer
    const details::PODptr<size_type> next = iter.next();

    // delete the storage
    UserAllocator::free(iter.begin());

    // increment iter
    iter = next;
  } while (iter.valid());

  list.invalidate();
  this->first = 0;

  return true;
}

template <typename UserAllocator>
void * pool<UserAllocator>::malloc_need_resize()
{
  // No memory in any of our storages; make a new storage,
  const size_type partition_size = alloc_size();
  const size_type POD_size = next_size * partition_size +
      details::pool::ct_lcm<sizeof(size_type), sizeof(void *)>::value + sizeof(size_type);
  char * const ptr = UserAllocator::malloc(POD_size);
  if (ptr == 0)
    return 0;
  const details::PODptr<size_type> node(ptr, POD_size);
  next_size <<= 1;

  //  initialize it,
  store().add_block(node.begin(), node.element_size(), partition_size);

  //  insert it into the list,
  node.next(list);
  list = node;

  //  and return a chunk from it.
  return store().malloc();
}

template <typename UserAllocator>
void * pool<UserAllocator>::ordered_malloc_need_resize()
{
  // No memory in any of our storages; make a new storage,
  const size_type partition_size = alloc_size();
  const size_type POD_size = next_size * partition_size +
      details::pool::ct_lcm<sizeof(size_type), sizeof(void *)>::value + sizeof(size_type);
  char * const ptr = UserAllocator::malloc(POD_size);
  if (ptr == 0)
    return 0;
  const details::PODptr<size_type> node(ptr, POD_size);
  next_size <<= 1;

  //  initialize it,
  //  (we can use "add_block" here because we know that
  //  the free list is empty, so we don't have to use
  //  the slower ordered version)
  store().add_block(node.begin(), node.element_size(), partition_size);

  //  insert it into the list,
  //   handle border case
  if (!list.valid() || std::greater<void *>()(list.begin(), node.begin()))
  {
    node.next(list);
    list = node;
  }
  else
  {
    details::PODptr<size_type> prev = list;

    while (true)
    {
      // if we're about to hit the end or
      //  if we've found where "node" goes
      if (prev.next_ptr() == 0
          || std::greater<void *>()(prev.next_ptr(), node.begin()))
        break;

      prev = prev.next();
    }

    node.next(prev.next());
    prev.next(node);
  }

  //  and return a chunk from it.
  return store().malloc();
}

template <typename UserAllocator>
void * pool<UserAllocator>::ordered_malloc(const size_type n)
{
  const size_type partition_size = alloc_size();
  const size_type total_req_size = n * requested_size;
  const size_type num_chunks = total_req_size / partition_size +
      ((total_req_size % partition_size) ? true : false);

  void * ret = store().malloc_n(num_chunks, partition_size);

  if (ret != 0)
    return ret;

  // Not enougn memory in our storages; make a new storage,
  BOOST_USING_STD_MAX();
  next_size = max BOOST_PREVENT_MACRO_SUBSTITUTION(next_size, num_chunks);
  const size_type POD_size = next_size * partition_size +
      details::pool::ct_lcm<sizeof(size_type), sizeof(void *)>::value + sizeof(size_type);
  char * const ptr = UserAllocator::malloc(POD_size);
  if (ptr == 0)
    return 0;
  const details::PODptr<size_type> node(ptr, POD_size);

  // Split up block so we can use what wasn't requested
  //  (we can use "add_block" here because we know that
  //  the free list is empty, so we don't have to use
  //  the slower ordered version)
  if (next_size > num_chunks)
    store().add_block(node.begin() + num_chunks * partition_size,
        node.element_size() - num_chunks * partition_size, partition_size);

  next_size <<= 1;

  //  insert it into the list,
  //   handle border case
  if (!list.valid() || std::greater<void *>()(list.begin(), node.begin()))
  {
    node.next(list);
    list = node;
  }
  else
  {
    details::PODptr<size_type> prev = list;

    while (true)
    {
      // if we're about to hit the end or
      //  if we've found where "node" goes
      if (prev.next_ptr() == 0
          || std::greater<void *>()(prev.next_ptr(), node.begin()))
        break;

      prev = prev.next();
    }

    node.next(prev.next());
    prev.next(node);
  }

  //  and return it.
  return node.begin();
}

template <typename UserAllocator>
details::PODptr<typename pool<UserAllocator>::size_type>
pool<UserAllocator>::find_POD(void * const chunk) const
{
  // We have to find which storage this chunk is from.
  details::PODptr<size_type> iter = list;
  while (iter.valid())
  {
    if (is_from(chunk, iter.begin(), iter.element_size()))
      return iter;
    iter = iter.next();
  }

  return iter;
}

} // namespace boost

#endif
