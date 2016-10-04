//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2012-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#define BOOST_CONTAINER_SOURCE
#include <boost/container/detail/dlmalloc.hpp>

namespace boost{
namespace container{

BOOST_CONTAINER_DECL size_t dlmalloc_size(const void *p)
{  return boost_cont_size(p);  }

BOOST_CONTAINER_DECL void* dlmalloc_malloc(size_t bytes)
{  return boost_cont_malloc(bytes);  }

BOOST_CONTAINER_DECL void  dlmalloc_free(void* mem)
{  return boost_cont_free(mem);  }

BOOST_CONTAINER_DECL void* dlmalloc_memalign(size_t bytes, size_t alignment)
{  return boost_cont_memalign(bytes, alignment);  }

BOOST_CONTAINER_DECL int dlmalloc_multialloc_nodes
   (size_t n_elements, size_t elem_size, size_t contiguous_elements, boost_cont_memchain *pchain)
{  return boost_cont_multialloc_nodes(n_elements, elem_size, contiguous_elements, pchain);  }

BOOST_CONTAINER_DECL int dlmalloc_multialloc_arrays
   (size_t n_elements, const size_t *sizes, size_t sizeof_element, size_t contiguous_elements, boost_cont_memchain *pchain)
{  return boost_cont_multialloc_arrays(n_elements, sizes, sizeof_element, contiguous_elements, pchain); }

BOOST_CONTAINER_DECL void dlmalloc_multidealloc(boost_cont_memchain *pchain)
{  return boost_cont_multidealloc(pchain); }

BOOST_CONTAINER_DECL size_t dlmalloc_footprint()
{  return boost_cont_footprint(); }

BOOST_CONTAINER_DECL size_t dlmalloc_allocated_memory()
{  return boost_cont_allocated_memory(); }

BOOST_CONTAINER_DECL size_t dlmalloc_chunksize(const void *p)
{  return boost_cont_chunksize(p); }

BOOST_CONTAINER_DECL int dlmalloc_all_deallocated()
{  return boost_cont_all_deallocated(); }

BOOST_CONTAINER_DECL boost_cont_malloc_stats_t dlmalloc_malloc_stats()
{  return boost_cont_malloc_stats(); }

BOOST_CONTAINER_DECL size_t dlmalloc_in_use_memory()
{  return boost_cont_in_use_memory(); }

BOOST_CONTAINER_DECL int dlmalloc_trim(size_t pad)
{  return boost_cont_trim(pad); }

BOOST_CONTAINER_DECL int dlmalloc_mallopt(int parameter_number, int parameter_value)
{  return boost_cont_mallopt(parameter_number, parameter_value); }

BOOST_CONTAINER_DECL int dlmalloc_grow
   (void* oldmem, size_t minbytes, size_t maxbytes, size_t *received)
{  return boost_cont_grow(oldmem, minbytes, maxbytes, received); }

BOOST_CONTAINER_DECL int dlmalloc_shrink
   (void* oldmem, size_t minbytes, size_t maxbytes, size_t *received, int do_commit)
{  return boost_cont_shrink(oldmem, minbytes, maxbytes, received, do_commit); }

BOOST_CONTAINER_DECL void* dlmalloc_alloc
   (size_t minbytes, size_t preferred_bytes, size_t *received_bytes)
{  return boost_cont_alloc(minbytes, preferred_bytes, received_bytes); }

BOOST_CONTAINER_DECL int dlmalloc_malloc_check()
{  return boost_cont_malloc_check(); }

BOOST_CONTAINER_DECL boost_cont_command_ret_t dlmalloc_allocation_command
   ( allocation_type command
   , size_t sizeof_object
   , size_t limit_objects
   , size_t preferred_objects
   , size_t *received_objects
   , void *reuse_ptr
   )
{  return boost_cont_allocation_command(command, sizeof_object, limit_objects, preferred_objects, received_objects, reuse_ptr); }

BOOST_CONTAINER_DECL void *dlmalloc_sync_create()
{  return boost_cont_sync_create();  }

BOOST_CONTAINER_DECL void dlmalloc_sync_destroy(void *sync)
{  return boost_cont_sync_destroy(sync);  }

BOOST_CONTAINER_DECL bool dlmalloc_sync_lock(void *sync)
{  return boost_cont_sync_lock(sync) != 0;  }

BOOST_CONTAINER_DECL void dlmalloc_sync_unlock(void *sync)
{  return boost_cont_sync_unlock(sync);  }

BOOST_CONTAINER_DECL bool dlmalloc_global_sync_lock()
{  return boost_cont_global_sync_lock() != 0;  }

BOOST_CONTAINER_DECL void dlmalloc_global_sync_unlock()
{  return boost_cont_global_sync_unlock();  }

}  //namespace container{
}  //namespace boost{
