 //////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2011-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_DETAIL_WINAPI_SEMAPHORE_WRAPPER_HPP
#define BOOST_INTERPROCESS_DETAIL_WINAPI_SEMAPHORE_WRAPPER_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>
#include <boost/interprocess/creation_tags.hpp>
#include <boost/interprocess/permissions.hpp>
#include <boost/interprocess/detail/win32_api.hpp>
#include <boost/interprocess/detail/posix_time_types_wrk.hpp>
#include <boost/interprocess/errors.hpp>
#include <boost/interprocess/exceptions.hpp>
#include <limits>

namespace boost {
namespace interprocess {
namespace ipcdetail {

class winapi_semaphore_functions
{
   /// @cond

   //Non-copyable
   winapi_semaphore_functions(const winapi_semaphore_functions &);
   winapi_semaphore_functions &operator=(const winapi_semaphore_functions &);
   /// @endcond

   public:
   winapi_semaphore_functions(void *hnd)
      : m_sem_hnd(hnd)
   {}

   void post(long count = 1)
   {
      long prev_count;
      winapi::release_semaphore(m_sem_hnd, count, &prev_count);
   }

   void wait()
   {
      if(winapi::wait_for_single_object(m_sem_hnd, winapi::infinite_time) != winapi::wait_object_0){
         error_info err = system_error_code();
         throw interprocess_exception(err);
      }
   }

   bool try_wait()
   {
      unsigned long ret = winapi::wait_for_single_object(m_sem_hnd, 0);
      if(ret == winapi::wait_object_0){
         return true;
      }
      else if(ret == winapi::wait_timeout){
         return false;
      }
      else{
         error_info err = system_error_code();
         throw interprocess_exception(err);
      }
   }

   bool timed_wait(const boost::posix_time::ptime &abs_time)
   {
      if(abs_time == boost::posix_time::pos_infin){
         this->wait();
         return true;
      }

      unsigned long ret = winapi::wait_for_single_object
         (m_sem_hnd, (abs_time - microsec_clock::universal_time()).total_milliseconds());
      if(ret == winapi::wait_object_0){
         return true;
      }
      else if(ret == winapi::wait_timeout){
         return false;
      }
      else{
         error_info err = system_error_code();
         throw interprocess_exception(err);
      }
   }

   long value() const
   {
      long l_count, l_limit;
      if(!winapi::get_semaphore_info(m_sem_hnd, l_count, l_limit))
         return 0;
      return l_count;
   }

   long limit() const
   {
      long l_count, l_limit;
      if(!winapi::get_semaphore_info(m_sem_hnd, l_count, l_limit))
         return 0;
      return l_limit;
   }

   /// @cond
   protected:
   void *m_sem_hnd;
   /// @endcond
};


//Swappable semaphore wrapper
class winapi_semaphore_wrapper
   : public winapi_semaphore_functions
{
   winapi_semaphore_wrapper(const winapi_semaphore_wrapper &);
   winapi_semaphore_wrapper &operator=(const winapi_semaphore_wrapper &);

   public:

   //Long is 32 bits in windows
   static const long MaxCount = long(0x7FFFFFFF);

   winapi_semaphore_wrapper(void *hnd = winapi::invalid_handle_value)
      : winapi_semaphore_functions(hnd)
   {}

   ~winapi_semaphore_wrapper()
   {  this->close(); }

   void *release()
   {
      void *hnd = m_sem_hnd;
      m_sem_hnd = winapi::invalid_handle_value;
      return hnd;
   }

   void *handle() const
   {  return m_sem_hnd; }

   bool open_or_create( const char *name
                      , long sem_count
                      , long max_count
                      , const permissions &perm
                      , bool &created)
   {
      if(m_sem_hnd == winapi::invalid_handle_value){
         m_sem_hnd = winapi::open_or_create_semaphore
            ( name
            , sem_count
            , max_count
            , (winapi::interprocess_security_attributes*)perm.get_permissions()
            );
         created = winapi::get_last_error() != winapi::error_already_exists;
         return m_sem_hnd != winapi::invalid_handle_value;
      }
      else{
         return false;
      }
   }

   bool open_semaphore(const char *name)
   {
      if(m_sem_hnd == winapi::invalid_handle_value){
         m_sem_hnd = winapi::open_semaphore(name);
         return m_sem_hnd != winapi::invalid_handle_value;
      }
      else{
         return false;
      }
   }

   void close()
   {
      if(m_sem_hnd != winapi::invalid_handle_value){
         winapi::close_handle(m_sem_hnd);
         m_sem_hnd = winapi::invalid_handle_value;
      }
   }

   void swap(winapi_semaphore_wrapper &other)
   {  void *tmp = m_sem_hnd; m_sem_hnd = other.m_sem_hnd; other.m_sem_hnd = tmp;   }
};

}  //namespace ipcdetail {
}  //namespace interprocess {
}  //namespace boost {

#include <boost/interprocess/detail/config_end.hpp>

#endif   //BOOST_INTERPROCESS_DETAIL_WINAPI_SEMAPHORE_WRAPPER_HPP
