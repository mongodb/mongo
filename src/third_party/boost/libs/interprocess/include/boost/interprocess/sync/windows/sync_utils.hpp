//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_DETAIL_SYNC_UTILS_HPP
#define BOOST_INTERPROCESS_DETAIL_SYNC_UTILS_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif
#
#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>
#include <boost/interprocess/detail/win32_api.hpp>
#include <boost/interprocess/sync/spin/mutex.hpp>
#include <boost/interprocess/exceptions.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/interprocess/sync/windows/winapi_semaphore_wrapper.hpp>
#include <boost/interprocess/sync/windows/winapi_mutex_wrapper.hpp>

//Shield against external warnings
#include <boost/interprocess/detail/config_external_begin.hpp>
#include <boost/container/map.hpp>
#include <boost/interprocess/detail/config_external_end.hpp>
#include <boost/container/flat_map.hpp>

#include <cstddef>

namespace boost {
namespace interprocess {
namespace ipcdetail {

inline bool bytes_to_str(const void *mem, const std::size_t mem_length, char *out_str, std::size_t &out_length)
{
   const std::size_t need_mem = mem_length*2+1;
   if(out_length < need_mem){
      out_length = need_mem;
      return false;
   }

   const char Characters [] =
      { '0', '1', '2', '3', '4', '5', '6', '7'
      , '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

   std::size_t char_counter = 0;
   const char *buf = (const char *)mem;
   for(std::size_t i = 0; i != mem_length; ++i){
      out_str[char_counter++] = Characters[(buf[i]&0xF0)>>4];
      out_str[char_counter++] = Characters[(buf[i]&0x0F)];
   }
   out_str[char_counter] = 0;
   return true;
}

inline bool bytes_to_str(const void *mem, const std::size_t mem_length, wchar_t *out_str, std::size_t &out_length)
{
   const std::size_t need_mem = mem_length*2+1;
   if(out_length < need_mem){
      out_length = need_mem;
      return false;
   }

   const wchar_t Characters [] =
      { L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7'
      , L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F' };

   std::size_t char_counter = 0;
   const char *buf = (const char *)mem;
   for(std::size_t i = 0; i != mem_length; ++i){
      out_str[char_counter++] = Characters[(buf[i]&0xF0)>>4];
      out_str[char_counter++] = Characters[(buf[i]&0x0F)];
   }
   out_str[char_counter] = 0;
   return true;
}

class sync_id
{
   public:
   typedef __int64 internal_type;
   sync_id()
   {  winapi::query_performance_counter(&rand_);  }

   explicit sync_id(internal_type val)
   {  rand_ = val;  }

   const internal_type &internal_pod() const
   {  return rand_;  }

   internal_type &internal_pod()
   {  return rand_;  }

   friend std::size_t hash_value(const sync_id &m)
   {  return static_cast<std::size_t>(m.rand_);  }

   friend bool operator==(const sync_id &l, const sync_id &r)
   {  return l.rand_ == r.rand_;  }

   friend bool operator<(const sync_id &l, const sync_id &r)
   {  return l.rand_ < r.rand_;  }

   private:
   internal_type rand_;
};

class sync_handles
{
   public:
   enum type { MUTEX, SEMAPHORE };

   private:

   //key: id -> mapped: HANDLE. Hash map to allow efficient sync operations
   typedef boost::container::flat_map<sync_id, void*> id_map_type;
   //key: ordered address of the sync type -> key from id_map_type. Ordered map to allow closing handles when unmapping
   // Can't store iterators into id_map_type because they would get invalidated.
   typedef boost::container::flat_map<const void*, sync_id> addr_map_type;
   static const std::size_t LengthOfGlobal = sizeof("Global\\boost.ipc")-1;
   static const std::size_t StrSize        = LengthOfGlobal + (sizeof(sync_id)*2+1);
   typedef char NameBuf[StrSize];

   void fill_name(NameBuf &name, const sync_id &id)
   {
      const char *n = "Global\\boost.ipc";
      std::size_t i = 0;
      do{
         name[i] = n[i];
         ++i;
      } while(n[i]);
      std::size_t len = sizeof(NameBuf) - LengthOfGlobal;
      bytes_to_str(&id.internal_pod(), sizeof(id.internal_pod()), &name[LengthOfGlobal], len);
   }

   void throw_if_error(void *hnd_val)
   {
      if(!hnd_val){
         error_info err(static_cast<int>(winapi::get_last_error()));
         throw interprocess_exception(err);
      }
   }

   void* open_or_create_semaphore(const sync_id &id, unsigned int initial_count)
   {
      NameBuf name;
      fill_name(name, id);
      permissions unrestricted_security;
      unrestricted_security.set_unrestricted();
      winapi_semaphore_wrapper sem_wrapper;
      bool created;
      sem_wrapper.open_or_create
         (name, (long)initial_count, winapi_semaphore_wrapper::MaxCount, unrestricted_security, created);
      throw_if_error(sem_wrapper.handle());
      return sem_wrapper.release();
   }

   void* open_or_create_mutex(const sync_id &id)
   {
      NameBuf name;
      fill_name(name, id);
      permissions unrestricted_security;
      unrestricted_security.set_unrestricted();
      winapi_mutex_wrapper mtx_wrapper;
      mtx_wrapper.open_or_create(name, unrestricted_security);
      throw_if_error(mtx_wrapper.handle());
      return mtx_wrapper.release();
   }

   public:
   sync_handles()
      : num_handles_()
   {}

   ~sync_handles()
   {
      BOOST_ASSERT(num_handles_ == 0); //Sanity check that handle we don't leak handles
   }

   void *obtain_mutex(const sync_id &id, const void *mapping_address, bool *popen_created = 0)
   {
      id_map_type::value_type v(id, (void*)0);
      scoped_lock<spin_mutex> lock(mtx_);
      id_map_type::iterator it = umap_.insert(v).first;
      void *&hnd_val = it->second;
      if(!hnd_val){
         BOOST_ASSERT(map_.find(mapping_address) == map_.end());
         map_[mapping_address] = id;
         hnd_val = open_or_create_mutex(id);
         if(popen_created) *popen_created = true;
         ++num_handles_;
      }
      else if(popen_created){
         BOOST_ASSERT(map_.find(mapping_address) != map_.end());
         *popen_created = false;
      }

      return hnd_val;
   }

   void *obtain_semaphore(const sync_id &id, const void *mapping_address, unsigned int initial_count, bool *popen_created = 0)
   {
      id_map_type::value_type v(id, (void*)0);
      scoped_lock<spin_mutex> lock(mtx_);
      id_map_type::iterator it = umap_.insert(v).first;
      void *&hnd_val = it->second;
      if(!hnd_val){
         BOOST_ASSERT(map_.find(mapping_address) == map_.end());
         map_[mapping_address] = id;
         hnd_val = open_or_create_semaphore(id, initial_count);
         if(popen_created) *popen_created = true;
         ++num_handles_;
      }
      else if(popen_created){
         BOOST_ASSERT(map_.find(mapping_address) != map_.end());
         *popen_created = false;
      }
      return hnd_val;
   }

   void destroy_handle(const sync_id &id, const void *mapping_address)
   {
      scoped_lock<spin_mutex> lock(mtx_);
      id_map_type::iterator it = umap_.find(id);
      id_map_type::iterator itend = umap_.end();

      if(it != itend){
         winapi::close_handle(it->second);
         --num_handles_;
         std::size_t i = map_.erase(mapping_address);
         (void)i;
         BOOST_ASSERT(i == 1);   //The entry should be there
         umap_.erase(it);
      }
   }

   void destroy_syncs_in_range(const void *addr, std::size_t size)
   {
      const void *low_id(addr);
      const void *hig_id(static_cast<const char*>(addr)+size);
      scoped_lock<spin_mutex> lock(mtx_);
      addr_map_type::iterator itlow(map_.lower_bound(low_id)),
                         ithig(map_.lower_bound(hig_id)),
                         it(itlow);
      for (; it != ithig; ++it){
         sync_id ukey = it->second;
         id_map_type::iterator uit = umap_.find(ukey);
         BOOST_ASSERT(uit != umap_.end());
         void * const hnd = uit->second;
         umap_.erase(ukey);
         int ret = winapi::close_handle(hnd);
         --num_handles_;
         BOOST_ASSERT(ret != 0); (void)ret;  //Sanity check that handle was ok
      }

      map_.erase(itlow, ithig);
   }

   private:
   spin_mutex mtx_;
   id_map_type umap_;
   addr_map_type map_;
   std::size_t num_handles_;
};


}  //namespace ipcdetail {
}  //namespace interprocess {
}  //namespace boost {

#include <boost/interprocess/detail/config_end.hpp>

#endif   //BOOST_INTERPROCESS_DETAIL_SYNC_UTILS_HPP
