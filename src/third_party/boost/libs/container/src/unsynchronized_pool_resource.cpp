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

#include <boost/container/pmr/unsynchronized_pool_resource.hpp>

namespace boost {
namespace container {
namespace pmr {

unsynchronized_pool_resource::unsynchronized_pool_resource(const pool_options& opts, memory_resource* upstream) BOOST_NOEXCEPT
   : m_resource(opts, upstream)
{}

unsynchronized_pool_resource::unsynchronized_pool_resource() BOOST_NOEXCEPT
   : m_resource()
{}

unsynchronized_pool_resource::unsynchronized_pool_resource(memory_resource* upstream) BOOST_NOEXCEPT
   : m_resource(upstream)
{}

unsynchronized_pool_resource::unsynchronized_pool_resource(const pool_options& opts) BOOST_NOEXCEPT
   : m_resource(opts)
{}

unsynchronized_pool_resource::~unsynchronized_pool_resource() //virtual
{}

void unsynchronized_pool_resource::release()
{
   m_resource.release();
}

memory_resource* unsynchronized_pool_resource::upstream_resource() const
{  return m_resource.upstream_resource();  }

pool_options unsynchronized_pool_resource::options() const
{  return m_resource.options();  }

void* unsynchronized_pool_resource::do_allocate(std::size_t bytes, std::size_t alignment) //virtual
{  return m_resource.do_allocate(bytes, alignment);  }

void unsynchronized_pool_resource::do_deallocate(void* p, std::size_t bytes, std::size_t alignment) //virtual
{  return m_resource.do_deallocate(p, bytes, alignment);  }

bool unsynchronized_pool_resource::do_is_equal(const memory_resource& other) const BOOST_NOEXCEPT //virtual
{  return this == &other;  }

std::size_t unsynchronized_pool_resource::pool_count() const
{  return m_resource.pool_count();  }

std::size_t unsynchronized_pool_resource::pool_index(std::size_t bytes) const
{  return m_resource.pool_index(bytes);  }

std::size_t unsynchronized_pool_resource::pool_next_blocks_per_chunk(std::size_t pool_idx) const
{  return m_resource.pool_next_blocks_per_chunk(pool_idx);  }

std::size_t unsynchronized_pool_resource::pool_block(std::size_t pool_idx) const
{  return m_resource.pool_block(pool_idx);  }

std::size_t unsynchronized_pool_resource::pool_cached_blocks(std::size_t pool_idx) const
{  return m_resource.pool_cached_blocks(pool_idx);  }

}  //namespace pmr {
}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>
