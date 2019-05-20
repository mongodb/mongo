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
#include <boost/container/detail/thread_mutex.hpp>

#include <boost/container/pmr/synchronized_pool_resource.hpp>
#include <cstddef>

namespace {

using namespace boost::container::dtl;

class thread_mutex_lock
{
   thread_mutex &m_mut;

   public:
   explicit thread_mutex_lock(thread_mutex &m)
      : m_mut(m)
   {
      m_mut.lock();
   }

   ~thread_mutex_lock()
   {
      m_mut.unlock();
   }
};

}  //namespace {

namespace boost {
namespace container {
namespace pmr {

synchronized_pool_resource::synchronized_pool_resource(const pool_options& opts, memory_resource* upstream) BOOST_NOEXCEPT
   : m_mut(), m_pool_resource(opts, upstream)
{}

synchronized_pool_resource::synchronized_pool_resource() BOOST_NOEXCEPT
   : m_mut(), m_pool_resource()
{}

synchronized_pool_resource::synchronized_pool_resource(memory_resource* upstream) BOOST_NOEXCEPT
   : m_mut(), m_pool_resource(upstream)
{}

synchronized_pool_resource::synchronized_pool_resource(const pool_options& opts) BOOST_NOEXCEPT
   : m_mut(), m_pool_resource(opts)
{}

synchronized_pool_resource::~synchronized_pool_resource() //virtual
{}

void synchronized_pool_resource::release()
{
   thread_mutex_lock lck(m_mut); (void)lck;
   m_pool_resource.release();
}

memory_resource* synchronized_pool_resource::upstream_resource() const
{  return m_pool_resource.upstream_resource();  }

pool_options synchronized_pool_resource::options() const
{  return m_pool_resource.options();  }

void* synchronized_pool_resource::do_allocate(std::size_t bytes, std::size_t alignment) //virtual
{
   thread_mutex_lock lck(m_mut); (void)lck;
   return m_pool_resource.do_allocate(bytes, alignment);
}

void synchronized_pool_resource::do_deallocate(void* p, std::size_t bytes, std::size_t alignment) //virtual
{
   thread_mutex_lock lck(m_mut); (void)lck;
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
