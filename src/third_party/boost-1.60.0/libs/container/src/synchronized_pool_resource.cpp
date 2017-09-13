//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2015-2015. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#define BOOST_CONTAINER_SOURCE
#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/detail/dlmalloc.hpp>

#include <boost/container/pmr/synchronized_pool_resource.hpp>
#include <cstddef>

namespace {

using namespace boost::container;

class dlmalloc_sync_scoped_lock
{
   void *m_sync;

   public:
   explicit dlmalloc_sync_scoped_lock(void *sync)
      : m_sync(sync)
   {
      if(!dlmalloc_sync_lock(m_sync)){
         throw_bad_alloc();
      }
   }

   ~dlmalloc_sync_scoped_lock()
   {
      dlmalloc_sync_unlock(m_sync);
   }
};

}  //namespace {

namespace boost {
namespace container {
namespace pmr {

synchronized_pool_resource::synchronized_pool_resource(const pool_options& opts, memory_resource* upstream) BOOST_NOEXCEPT
   : m_pool_resource(opts, upstream), m_opaque_sync()
{}

synchronized_pool_resource::synchronized_pool_resource() BOOST_NOEXCEPT
   : m_pool_resource(), m_opaque_sync()
{}

synchronized_pool_resource::synchronized_pool_resource(memory_resource* upstream) BOOST_NOEXCEPT
   : m_pool_resource(upstream), m_opaque_sync()
{}

synchronized_pool_resource::synchronized_pool_resource(const pool_options& opts) BOOST_NOEXCEPT
   : m_pool_resource(opts), m_opaque_sync()
{}

synchronized_pool_resource::~synchronized_pool_resource() //virtual
{
   if(m_opaque_sync)
      dlmalloc_sync_destroy(m_opaque_sync);
}

void synchronized_pool_resource::release()
{
   if(m_opaque_sync){   //If there is no mutex, no allocation could be done
      m_pool_resource.release();
   }
}

memory_resource* synchronized_pool_resource::upstream_resource() const
{  return m_pool_resource.upstream_resource();  }

pool_options synchronized_pool_resource::options() const
{  return m_pool_resource.options();  }

void* synchronized_pool_resource::do_allocate(std::size_t bytes, std::size_t alignment) //virtual
{
   if(!m_opaque_sync){   //If there is no mutex, no allocation could be done
      m_opaque_sync = dlmalloc_sync_create();
      if(!m_opaque_sync){
         throw_bad_alloc();
      }
   }
   dlmalloc_sync_scoped_lock lock(m_opaque_sync); (void)lock;
   return m_pool_resource.do_allocate(bytes, alignment);
}

void synchronized_pool_resource::do_deallocate(void* p, std::size_t bytes, std::size_t alignment) //virtual
{
   dlmalloc_sync_scoped_lock lock(m_opaque_sync); (void)lock;
   return m_pool_resource.do_deallocate(p, bytes, alignment);
}

bool synchronized_pool_resource::do_is_equal(const memory_resource& other) const BOOST_NOEXCEPT //virtual
{  return this == dynamic_cast<const synchronized_pool_resource*>(&other);  }

std::size_t synchronized_pool_resource::pool_count() const
{  return m_pool_resource.pool_count();  }

std::size_t synchronized_pool_resource::pool_index(std::size_t bytes) const
{  return m_pool_resource.pool_index(bytes);  }

std::size_t synchronized_pool_resource::pool_next_blocks_per_chunk(std::size_t pool_idx) const
{  return m_pool_resource.pool_next_blocks_per_chunk(pool_idx);  }

std::size_t synchronized_pool_resource::pool_block(std::size_t pool_idx) const
{  return m_pool_resource.pool_block(pool_idx);  }

std::size_t synchronized_pool_resource::pool_cached_blocks(std::size_t pool_idx) const
{  return m_pool_resource.pool_cached_blocks(pool_idx);  }

}  //namespace pmr {
}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>
