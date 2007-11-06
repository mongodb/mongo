// Copyright (C) 2000, 2001 Stephen Cleary
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_SIMPLE_SEGREGATED_STORAGE_HPP
#define BOOST_SIMPLE_SEGREGATED_STORAGE_HPP

// std::greater
#include <functional>

#include <boost/pool/poolfwd.hpp>

namespace boost {

template <typename SizeType>
class simple_segregated_storage
{
  public:
    typedef SizeType size_type;

  private:
    simple_segregated_storage(const simple_segregated_storage &);
    void operator=(const simple_segregated_storage &);

    // pre: (n > 0), (start != 0), (nextof(start) != 0)
    // post: (start != 0)
    static void * try_malloc_n(void * & start, size_type n,
        size_type partition_size);

  protected:
    void * first;

    // Traverses the free list referred to by "first",
    //  and returns the iterator previous to where
    //  "ptr" would go if it was in the free list.
    // Returns 0 if "ptr" would go at the beginning
    //  of the free list (i.e., before "first")
    void * find_prev(void * ptr);

    // for the sake of code readability :)
    static void * & nextof(void * const ptr)
    { return *(static_cast<void **>(ptr)); }

  public:
    // Post: empty()
    simple_segregated_storage()
    :first(0) { }

    // pre: npartition_sz >= sizeof(void *)
    //      npartition_sz = sizeof(void *) * i, for some integer i
    //      nsz >= npartition_sz
    //      block is properly aligned for an array of object of
    //        size npartition_sz and array of void *
    // The requirements above guarantee that any pointer to a chunk
    //  (which is a pointer to an element in an array of npartition_sz)
    //  may be cast to void **.
    static void * segregate(void * block,
        size_type nsz, size_type npartition_sz,
        void * end = 0);

    // Same preconditions as 'segregate'
    // Post: !empty()
    void add_block(void * const block,
        const size_type nsz, const size_type npartition_sz)
    {
      // Segregate this block and merge its free list into the
      //  free list referred to by "first"
      first = segregate(block, nsz, npartition_sz, first);
    }

    // Same preconditions as 'segregate'
    // Post: !empty()
    void add_ordered_block(void * const block,
        const size_type nsz, const size_type npartition_sz)
    {
      // This (slower) version of add_block segregates the
      //  block and merges its free list into our free list
      //  in the proper order

      // Find where "block" would go in the free list
      void * const loc = find_prev(block);

      // Place either at beginning or in middle/end
      if (loc == 0)
        add_block(block, nsz, npartition_sz);
      else
        nextof(loc) = segregate(block, nsz, npartition_sz, nextof(loc));
    }

    // default destructor

    bool empty() const { return (first == 0); }

    // pre: !empty()
    void * malloc()
    {
      void * const ret = first;

      // Increment the "first" pointer to point to the next chunk
      first = nextof(first);
      return ret;
    }

    // pre: chunk was previously returned from a malloc() referring to the
    //  same free list
    // post: !empty()
    void free(void * const chunk)
    {
      nextof(chunk) = first;
      first = chunk;
    }

    // pre: chunk was previously returned from a malloc() referring to the
    //  same free list
    // post: !empty()
    void ordered_free(void * const chunk)
    {
      // This (slower) implementation of 'free' places the memory
      //  back in the list in its proper order.

      // Find where "chunk" goes in the free list
      void * const loc = find_prev(chunk);

      // Place either at beginning or in middle/end
      if (loc == 0)
        free(chunk);
      else
      {
        nextof(chunk) = nextof(loc);
        nextof(loc) = chunk;
      }
    }

    // Note: if you're allocating/deallocating n a lot, you should
    //  be using an ordered pool.
    void * malloc_n(size_type n, size_type partition_size);

    // pre: chunks was previously allocated from *this with the same
    //   values for n and partition_size
    // post: !empty()
    // Note: if you're allocating/deallocating n a lot, you should
    //  be using an ordered pool.
    void free_n(void * const chunks, const size_type n,
        const size_type partition_size)
    {
      add_block(chunks, n * partition_size, partition_size);
    }

    // pre: chunks was previously allocated from *this with the same
    //   values for n and partition_size
    // post: !empty()
    void ordered_free_n(void * const chunks, const size_type n,
        const size_type partition_size)
    {
      add_ordered_block(chunks, n * partition_size, partition_size);
    }
};

template <typename SizeType>
void * simple_segregated_storage<SizeType>::find_prev(void * const ptr)
{
  // Handle border case
  if (first == 0 || std::greater<void *>()(first, ptr))
    return 0;

  void * iter = first;
  while (true)
  {
    // if we're about to hit the end or
    //  if we've found where "ptr" goes
    if (nextof(iter) == 0 || std::greater<void *>()(nextof(iter), ptr))
      return iter;

    iter = nextof(iter);
  }
}

template <typename SizeType>
void * simple_segregated_storage<SizeType>::segregate(
    void * const block,
    const size_type sz,
    const size_type partition_sz,
    void * const end)
{
  // Get pointer to last valid chunk, preventing overflow on size calculations
  //  The division followed by the multiplication just makes sure that
  //    old == block + partition_sz * i, for some integer i, even if the
  //    block size (sz) is not a multiple of the partition size.
  char * old = static_cast<char *>(block)
      + ((sz - partition_sz) / partition_sz) * partition_sz;

  // Set it to point to the end
  nextof(old) = end;

  // Handle border case where sz == partition_sz (i.e., we're handling an array
  //  of 1 element)
  if (old == block)
    return block;

  // Iterate backwards, building a singly-linked list of pointers
  for (char * iter = old - partition_sz; iter != block;
      old = iter, iter -= partition_sz)
    nextof(iter) = old;

  // Point the first pointer, too
  nextof(block) = old;

  return block;
}

// The following function attempts to find n contiguous chunks
//  of size partition_size in the free list, starting at start.
// If it succeds, it returns the last chunk in that contiguous
//  sequence, so that the sequence is known by [start, {retval}]
// If it fails, it does do either because it's at the end of the
//  free list or hits a non-contiguous chunk.  In either case,
//  it will return 0, and set start to the last considered
//  chunk.  You are at the end of the free list if
//  nextof(start) == 0.  Otherwise, start points to the last
//  chunk in the contiguous sequence, and nextof(start) points
//  to the first chunk in the next contiguous sequence (assuming
//  an ordered free list)
template <typename SizeType>
void * simple_segregated_storage<SizeType>::try_malloc_n(
    void * & start, size_type n, const size_type partition_size)
{
  void * iter = nextof(start);
  while (--n != 0)
  {
    void * next = nextof(iter);
    if (next != static_cast<char *>(iter) + partition_size)
    {
      // next == 0 (end-of-list) or non-contiguous chunk found
      start = iter;
      return 0;
    }
    iter = next;
  }
  return iter;
}

template <typename SizeType>
void * simple_segregated_storage<SizeType>::malloc_n(const size_type n,
    const size_type partition_size)
{
  void * start = &first;
  void * iter;
  do
  {
    if (nextof(start) == 0)
      return 0;
    iter = try_malloc_n(start, n, partition_size);
  } while (iter == 0);
  void * const ret = nextof(start);
  nextof(start) = nextof(iter);
  return ret;
}

} // namespace boost

#endif
