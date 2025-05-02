//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2006-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_MANAGED_OPEN_OR_CREATE_IMPL
#define BOOST_INTERPROCESS_MANAGED_OPEN_OR_CREATE_IMPL

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif
#
#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/os_thread_functions.hpp>
#include <boost/interprocess/detail/os_file_functions.hpp>
#include <boost/interprocess/creation_tags.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/detail/utilities.hpp>
#include <boost/interprocess/detail/type_traits.hpp>
#include <boost/interprocess/detail/atomic.hpp>
#include <boost/interprocess/detail/interprocess_tester.hpp>
#include <boost/interprocess/anonymous_shared_memory.hpp>
#include <boost/interprocess/creation_tags.hpp>
#include <boost/interprocess/detail/mpl.hpp>
#include <boost/interprocess/permissions.hpp>
#include <boost/container/detail/type_traits.hpp>  //alignment_of, aligned_storage
#include <boost/interprocess/sync/spin/wait.hpp>
#include <boost/interprocess/timed_utils.hpp>
#include <boost/move/move.hpp>
#include <boost/cstdint.hpp>

namespace boost {
namespace interprocess {
namespace ipcdetail {

template <bool StoreDevice, class DeviceAbstraction>
class managed_open_or_create_impl_device_holder
{
   public:
   DeviceAbstraction &get_device()
   {  static DeviceAbstraction dev; return dev; }

   const DeviceAbstraction &get_device() const
   {  static DeviceAbstraction dev; return dev; }
};

template <class DeviceAbstraction>
class managed_open_or_create_impl_device_holder<true, DeviceAbstraction>
{
   public:
   DeviceAbstraction &get_device()
   {  return dev; }

   const DeviceAbstraction &get_device() const
   {  return dev; }

   private:
   DeviceAbstraction dev;
};

template<class DeviceAbstraction, std::size_t MemAlignment, bool FileBased, bool StoreDevice>
class managed_open_or_create_impl
   : public managed_open_or_create_impl_device_holder<StoreDevice, DeviceAbstraction>
{
   //Non-copyable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(managed_open_or_create_impl)
   typedef bool_<FileBased> file_like_t;

   static const unsigned MaxCreateOrOpenTries = BOOST_INTERPROCESS_MANAGED_OPEN_OR_CREATE_INITIALIZE_MAX_TRIES;
   static const unsigned MaxInitializeTimeSec = BOOST_INTERPROCESS_MANAGED_OPEN_OR_CREATE_INITIALIZE_TIMEOUT_SEC;

   typedef managed_open_or_create_impl_device_holder<StoreDevice, DeviceAbstraction> DevHolder;
   enum
   {
      UninitializedSegment,
      InitializingSegment,
      InitializedSegment,
      CorruptedSegment
   };

   static const std::size_t RequiredAlignment =
      MemAlignment ? MemAlignment
                   : boost::container::dtl::alignment_of< boost::container::dtl::max_align_t >::value
                   ;

   public:
   static const std::size_t ManagedOpenOrCreateUserOffset =
      ct_rounded_size<sizeof(boost::uint32_t), RequiredAlignment>::value;

   managed_open_or_create_impl()
   {}

   template <class DeviceId>
   managed_open_or_create_impl(create_only_t,
                 const DeviceId & id,
                 std::size_t size,
                 mode_t mode,
                 const void *addr,
                 const permissions &perm)
   {
      priv_open_or_create
         ( DoCreate
         , id
         , size
         , mode
         , addr
         , perm
         , null_mapped_region_function());
   }

   template <class DeviceId>
   managed_open_or_create_impl(open_only_t,
                 const DeviceId & id,
                 mode_t mode,
                 const void *addr)
   {
      priv_open_or_create
         ( DoOpen
         , id
         , 0
         , mode
         , addr
         , permissions()
         , null_mapped_region_function());
   }

   template <class DeviceId>
   managed_open_or_create_impl(open_or_create_t,
                 const DeviceId & id,
                 std::size_t size,
                 mode_t mode,
                 const void *addr,
                 const permissions &perm)
   {
      priv_open_or_create
         ( DoOpenOrCreate
         , id
         , size
         , mode
         , addr
         , perm
         , null_mapped_region_function());
   }

   template <class DeviceId, class ConstructFunc>
   managed_open_or_create_impl(create_only_t,
                 const DeviceId & id,
                 std::size_t size,
                 mode_t mode,
                 const void *addr,
                 const ConstructFunc &construct_func,
                 const permissions &perm)
   {
      priv_open_or_create
         (DoCreate
         , id
         , size
         , mode
         , addr
         , perm
         , construct_func);
   }

   template <class DeviceId, class ConstructFunc>
   managed_open_or_create_impl(open_only_t,
                 const DeviceId & id,
                 mode_t mode,
                 const void *addr,
                 const ConstructFunc &construct_func)
   {
      priv_open_or_create
         ( DoOpen
         , id
         , 0
         , mode
         , addr
         , permissions()
         , construct_func);
   }

   template <class DeviceId, class ConstructFunc>
   managed_open_or_create_impl(open_or_create_t,
                 const DeviceId & id,
                 std::size_t size,
                 mode_t mode,
                 const void *addr,
                 const ConstructFunc &construct_func,
                 const permissions &perm)
   {
      priv_open_or_create
         ( DoOpenOrCreate
         , id
         , size
         , mode
         , addr
         , perm
         , construct_func);
   }

   template <class ConstructFunc>
   managed_open_or_create_impl(std::size_t size, void *addr, const ConstructFunc &construct_func)
   {
      priv_map_anonymous
         ( size
         , addr
         , construct_func);
   }

   managed_open_or_create_impl(BOOST_RV_REF(managed_open_or_create_impl) moved)
   {  this->swap(moved);   }

   managed_open_or_create_impl &operator=(BOOST_RV_REF(managed_open_or_create_impl) moved)
   {
      managed_open_or_create_impl tmp(boost::move(moved));
      this->swap(tmp);
      return *this;
   }

   ~managed_open_or_create_impl()
   {}

   std::size_t get_user_size()  const
   {  return m_mapped_region.get_size() - ManagedOpenOrCreateUserOffset; }

   void *get_user_address()  const
   {  return static_cast<char*>(m_mapped_region.get_address()) + ManagedOpenOrCreateUserOffset;  }

   std::size_t get_real_size()  const
   {  return m_mapped_region.get_size(); }

   void *get_real_address()  const
   {  return m_mapped_region.get_address();  }

   void swap(managed_open_or_create_impl &other)
   {
      this->m_mapped_region.swap(other.m_mapped_region);
   }

   bool flush()
   {  return m_mapped_region.flush();  }

   const mapped_region &get_mapped_region() const
   {  return m_mapped_region;  }

   DeviceAbstraction &get_device()
   {  return this->DevHolder::get_device(); }

   const DeviceAbstraction &get_device() const
   {  return this->DevHolder::get_device(); }

   private:

   //These are templatized to allow explicit instantiations
   template<bool dummy>
   static void truncate_device(DeviceAbstraction &, offset_t, false_)
   {} //Empty

   template<bool dummy>
   static void truncate_device(DeviceAbstraction &dev, offset_t size, true_)
   {  dev.truncate(size);  }


   template<bool dummy>
   static bool check_offset_t_size(std::size_t , false_)
   { return true; } //Empty

   template<bool dummy>
   static bool check_offset_t_size(std::size_t size, true_)
   { return size == std::size_t(offset_t(size)); }

   //These are templatized to allow explicit instantiations
   template<bool dummy, class DeviceId>
   static void create_device(DeviceAbstraction &dev, const DeviceId & id, std::size_t size, const permissions &perm, false_ /*file_like*/)
   {
      DeviceAbstraction tmp(create_only, id, read_write, size, perm);
      tmp.swap(dev);
   }

   template<bool dummy, class DeviceId>
   static void create_device(DeviceAbstraction &dev, const DeviceId & id, std::size_t, const permissions &perm, true_ /*file_like*/)
   {
      DeviceAbstraction tmp(create_only, id, read_write, perm);
      tmp.swap(dev);
   }

   template <class DeviceId>
   static bool do_create_else_open(DeviceAbstraction &dev, const DeviceId & id, std::size_t size, const permissions &perm)
   {
      //This loop is very ugly, but brute force is sometimes better
      //than diplomacy. In POSIX file-based resources we can' know if we
      //effectively created the file or not (there is no ERROR_ALREADY_EXISTS equivalent),
      //so we try to create exclusively and fallback to open if already exists, with
      //some retries if opening also fails because the file does not exist
      //(there is a race, the creator just removed the file after creating it).
      //
      //We'll put a maximum retry limit just to avoid possible deadlocks, we don't
      //want to support pathological use cases.
      spin_wait swait;
      unsigned tries = 0;
      while(1){
         BOOST_INTERPROCESS_TRY{
            create_device<FileBased>(dev, id, size, perm, file_like_t());
            return true;
         }
         BOOST_INTERPROCESS_CATCH(interprocess_exception &ex){
            #ifndef BOOST_NO_EXCEPTIONS
            if(ex.get_error_code() != already_exists_error){
               BOOST_INTERPROCESS_RETHROW
            }
            else if (++tries == MaxCreateOrOpenTries) {
               //File existing when trying to create, but non-existing when
               //trying to open, and tried MaxCreateOrOpenTries times. Something fishy
               //is happening here and we can't solve it
               throw interprocess_exception(error_info(corrupted_error));
            }
            else{
               BOOST_INTERPROCESS_TRY{
                  DeviceAbstraction tmp(open_only, id, read_write);
                  dev.swap(tmp);
                  return false;
               }
               BOOST_INTERPROCESS_CATCH(interprocess_exception &e){
                  if(e.get_error_code() != not_found_error){
                     BOOST_INTERPROCESS_RETHROW
                  }
               }
               BOOST_INTERPROCESS_CATCH(...){
                  BOOST_INTERPROCESS_RETHROW
               } BOOST_INTERPROCESS_CATCH_END
            }
            #endif   //#ifndef BOOST_NO_EXCEPTIONS
         }
         BOOST_INTERPROCESS_CATCH(...){
            BOOST_INTERPROCESS_RETHROW
         } BOOST_INTERPROCESS_CATCH_END
         swait.yield();
      }
      return false;
   }

   template <class ConstructFunc>
   static void do_map_after_create
      (DeviceAbstraction &dev, mapped_region &final_region,
       std::size_t size, const void *addr, ConstructFunc construct_func)
   {
      BOOST_INTERPROCESS_TRY{
         //If this throws, we are lost
         truncate_device<FileBased>(dev, static_cast<offset_t>(size), file_like_t());

         //If the following throws, we will truncate the file to 1
         mapped_region region(dev, read_write, 0, 0, addr);
         boost::uint32_t *patomic_word = 0;  //avoid gcc warning
         patomic_word = static_cast<boost::uint32_t*>(region.get_address());
         boost::uint32_t previous = atomic_cas32(patomic_word, InitializingSegment, UninitializedSegment);

         if(previous == UninitializedSegment){
            BOOST_INTERPROCESS_TRY{
               construct_func( static_cast<char*>(region.get_address()) + ManagedOpenOrCreateUserOffset
                              , size - ManagedOpenOrCreateUserOffset, true);
               //All ok, just move resources to the external mapped region
               final_region.swap(region);
            }
            BOOST_INTERPROCESS_CATCH(...){
               atomic_write32(patomic_word, CorruptedSegment);
               BOOST_INTERPROCESS_RETHROW
            } BOOST_INTERPROCESS_CATCH_END
            atomic_write32(patomic_word, InitializedSegment);
         }
         else{
            atomic_write32(patomic_word, CorruptedSegment);
            throw interprocess_exception(error_info(corrupted_error));
         }
      }
      BOOST_INTERPROCESS_CATCH(...){
         BOOST_INTERPROCESS_TRY{
            truncate_device<FileBased>(dev, 1u, file_like_t());
         }
         BOOST_INTERPROCESS_CATCH(...){
         }
         BOOST_INTERPROCESS_CATCH_END
         BOOST_INTERPROCESS_RETHROW
      }
      BOOST_INTERPROCESS_CATCH_END
   }

   template <class ConstructFunc>
   static void do_map_after_open
      ( DeviceAbstraction &dev, mapped_region &final_region
      , const void *addr, ConstructFunc construct_func
      , bool ronly, bool cow)
   {
      const usduration TimeoutSec(usduration_from_seconds(MaxInitializeTimeSec));

      if(FileBased){
         offset_t filesize = 0;
         spin_wait swait;

         //If a file device was used, the creator might be truncating the device, so wait
         //until the file size is enough to map the initial word
         ustime ustime_start = microsec_clock<ustime>::universal_time();

         while(1){
            if(!get_file_size(file_handle_from_mapping_handle(dev.get_mapping_handle()), filesize)){
               error_info err = system_error_code();
               throw interprocess_exception(err);
            }
            if (filesize != 0)
               break;
            else {
               //More than MaxZeroTruncateTimeSec seconds waiting to the creator
               //to minimally increase the size of the file: something bad has happened
               const usduration elapsed(microsec_clock<ustime>::universal_time() - ustime_start);
               if (elapsed > TimeoutSec){
                  throw interprocess_exception(error_info(corrupted_error));
               }
               swait.yield();
            }
         }
         //The creator detected an error creating the file and signalled it with size 1
         if(filesize == 1){
            throw interprocess_exception(error_info(corrupted_error));
         }
      }

      mapped_region  region(dev, ronly ? read_only : (cow ? copy_on_write : read_write), 0, 0, addr);

      boost::uint32_t *patomic_word = static_cast<boost::uint32_t*>(region.get_address());
      boost::uint32_t value = atomic_read32(patomic_word);

      if (value != InitializedSegment){
         ustime ustime_start = microsec_clock<ustime>::universal_time();
         spin_wait swait;
         while ((value = atomic_read32(patomic_word)) != InitializedSegment){
            if(value == CorruptedSegment){
               throw interprocess_exception(error_info(corrupted_error));
            }
            //More than MaxZeroTruncateTimeSec seconds waiting to the creator
            //to minimally increase the size of the file: something bad has happened
            const usduration elapsed(microsec_clock<ustime>::universal_time() - ustime_start);
            if (elapsed > TimeoutSec){
               throw interprocess_exception(error_info(corrupted_error));
            }
            swait.yield();
         }
         //The size of the file might have grown while Uninitialized -> Initializing, so remap
         {
            mapped_region null_map;
            region.swap(null_map);
         }
         mapped_region  final_size_map(dev, ronly ? read_only : (cow ? copy_on_write : read_write), 0, 0, addr);
         final_size_map.swap(region);
      }
      construct_func( static_cast<char*>(region.get_address()) + ManagedOpenOrCreateUserOffset
                     , region.get_size() - ManagedOpenOrCreateUserOffset
                     , false);
      //All ok, just move resources to the external mapped region
      final_region.swap(region);
   }

   template <class DeviceId, class ConstructFunc> inline
   void priv_open_or_create
      (create_enum_t type,
       const DeviceId & id,
       std::size_t size,
       mode_t mode, const void *addr,
       const permissions &perm,
       ConstructFunc construct_func)
   {
      if(type != DoOpen){
         //Check if the requested size is enough to build the managed metadata
         const std::size_t func_min_size = construct_func.get_min_size();
         if( (std::size_t(-1) - ManagedOpenOrCreateUserOffset) < func_min_size ||
             size < (func_min_size + ManagedOpenOrCreateUserOffset) ){
            throw interprocess_exception(error_info(size_error));
         }
         //Check size can be represented by offset_t (used by truncate)
         if (!check_offset_t_size<FileBased>(size, file_like_t())){
           throw interprocess_exception(error_info(size_error));
         }
      }

      //Now create the device (file, shm file, etc.)
      DeviceAbstraction dev;
      (void)mode;
      bool created = false;
      bool ronly   = false;
      bool cow     = false;
      if(type == DoOpen){
         DeviceAbstraction tmp(open_only, id, mode == read_write ? read_write : read_only);
         tmp.swap(dev);
         ronly = mode == read_only;
         cow = mode == copy_on_write;
      }
      else if(type == DoCreate){
         create_device<FileBased>(dev, id, size, perm, file_like_t());
         created = true;
      }
      else { //DoOpenOrCreate
         created = this->do_create_else_open(dev, id, size, perm);
      }

      if(created){
         this->do_map_after_create(dev, m_mapped_region, size, addr, construct_func);
      }
      else{
         this->do_map_after_open(dev, m_mapped_region, addr, construct_func, ronly, cow);
      }

      if(StoreDevice){
         this->DevHolder::get_device() = boost::move(dev);
      }
   }

   template <class ConstructFunc> inline
   void priv_map_anonymous
      (std::size_t size,
       void *addr,
       ConstructFunc construct_func)
   {
      mapped_region region = anonymous_shared_memory(size, addr);

      boost::uint32_t *patomic_word = 0;  //avoid gcc warning
      patomic_word = static_cast<boost::uint32_t*>(region.get_address());
      boost::uint32_t previous = atomic_cas32(patomic_word, InitializingSegment, UninitializedSegment);

      if(previous == UninitializedSegment){
         BOOST_INTERPROCESS_TRY{
            construct_func( static_cast<char*>(region.get_address()) + ManagedOpenOrCreateUserOffset
                           , size - ManagedOpenOrCreateUserOffset, true);
            //All ok, just move resources to the external mapped region
            m_mapped_region.swap(region);
         }
         BOOST_INTERPROCESS_CATCH(...){
            atomic_write32(patomic_word, CorruptedSegment);
            BOOST_INTERPROCESS_RETHROW
         } BOOST_INTERPROCESS_CATCH_END
         atomic_write32(patomic_word, InitializedSegment);
      }
      else{
         atomic_write32(patomic_word, CorruptedSegment);
         throw interprocess_exception(error_info(corrupted_error));
      }
   }

   friend void swap(managed_open_or_create_impl &left, managed_open_or_create_impl &right)
   {
      left.swap(right);
   }

   private:
   friend class interprocess_tester;
   void dont_close_on_destruction()
   {  interprocess_tester::dont_close_on_destruction(m_mapped_region);  }

   mapped_region     m_mapped_region;
};

}  //namespace ipcdetail {

}  //namespace interprocess {
}  //namespace boost {

#include <boost/interprocess/detail/config_end.hpp>

#endif   //#ifndef BOOST_INTERPROCESS_MANAGED_OPEN_OR_CREATE_IMPL
