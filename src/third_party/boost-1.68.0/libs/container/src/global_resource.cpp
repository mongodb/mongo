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
#include <boost/container/pmr/memory_resource.hpp>

#include <boost/core/no_exceptions_support.hpp>
#include <boost/container/throw_exception.hpp>
#include <boost/container/detail/dlmalloc.hpp>  //For global lock

#include <cstddef>
#include <new>

namespace boost {
namespace container {
namespace pmr {

class new_delete_resource_imp
   : public memory_resource
{
   public:

   virtual ~new_delete_resource_imp()
   {}

   virtual void* do_allocate(std::size_t bytes, std::size_t alignment)
   {  (void)bytes; (void)alignment; return new char[bytes];  }

   virtual void do_deallocate(void* p, std::size_t bytes, std::size_t alignment)
   {  (void)bytes; (void)alignment; delete[]((char*)p);  }

   virtual bool do_is_equal(const memory_resource& other) const BOOST_NOEXCEPT
   {  return &other == this;   }
} new_delete_resource_instance;

struct null_memory_resource_imp
   : public memory_resource
{
   public:

   virtual ~null_memory_resource_imp()
   {}

   virtual void* do_allocate(std::size_t bytes, std::size_t alignment)
   {
      (void)bytes; (void)alignment;
      throw_bad_alloc();
      return 0;
   }

   virtual void do_deallocate(void* p, std::size_t bytes, std::size_t alignment)
   {  (void)p;  (void)bytes; (void)alignment;  }

   virtual bool do_is_equal(const memory_resource& other) const BOOST_NOEXCEPT
   {  return &other == this;   }
} null_memory_resource_instance;

BOOST_CONTAINER_DECL memory_resource* new_delete_resource() BOOST_NOEXCEPT
{
   return &new_delete_resource_instance;
}

BOOST_CONTAINER_DECL memory_resource* null_memory_resource() BOOST_NOEXCEPT
{
   return &null_memory_resource_instance;
}

static memory_resource *default_memory_resource = &new_delete_resource_instance;

BOOST_CONTAINER_DECL memory_resource* set_default_resource(memory_resource* r) BOOST_NOEXCEPT
{
   //TO-DO: synchronizes-with part using atomics
   if(dlmalloc_global_sync_lock()){
      memory_resource *previous = default_memory_resource;
      default_memory_resource = r ? r : new_delete_resource();
      dlmalloc_global_sync_unlock();
      return previous;
   }
   else{
      return new_delete_resource();
   }
}

BOOST_CONTAINER_DECL memory_resource* get_default_resource() BOOST_NOEXCEPT
{
   //TO-DO: synchronizes-with part using atomics
   if(dlmalloc_global_sync_lock()){
      memory_resource *current = default_memory_resource;
      dlmalloc_global_sync_unlock();
      return current;
   }
   else{
      return new_delete_resource();
   }
}

}  //namespace pmr {
}  //namespace container {
}  //namespace boost {
