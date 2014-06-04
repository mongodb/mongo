 //////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2011-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_DETAIL_WINAPI_MUTEX_WRAPPER_HPP
#define BOOST_INTERPROCESS_DETAIL_WINAPI_MUTEX_WRAPPER_HPP

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

class winapi_mutex_functions
{
   /// @cond

   //Non-copyable
   winapi_mutex_functions(const winapi_mutex_functions &);
   winapi_mutex_functions &operator=(const winapi_mutex_functions &);
   /// @endcond

   public:
   winapi_mutex_functions(void *mtx_hnd)
      : m_mtx_hnd(mtx_hnd)
   {}

   void unlock()
   {
      winapi::release_mutex(m_mtx_hnd);
   }

   void lock()
   {
      if(winapi::wait_for_single_object(m_mtx_hnd, winapi::infinite_time) != winapi::wait_object_0){
         error_info err = system_error_code();
         throw interprocess_exception(err);
      }
   }

   bool try_lock()
   {
      unsigned long ret = winapi::wait_for_single_object(m_mtx_hnd, 0);
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

   bool timed_lock(const boost::posix_time::ptime &abs_time)
   {
      if(abs_time == boost::posix_time::pos_infin){
         this->lock();
         return true;
      }

      unsigned long ret = winapi::wait_for_single_object
         (m_mtx_hnd, (abs_time - microsec_clock::universal_time()).total_milliseconds());
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

   /// @cond
   protected:
   void *m_mtx_hnd;
   /// @endcond
};

//Swappable mutex wrapper
class winapi_mutex_wrapper
   : public winapi_mutex_functions
{
   /// @cond

   //Non-copyable
   winapi_mutex_wrapper(const winapi_mutex_wrapper &);
   winapi_mutex_wrapper &operator=(const winapi_mutex_wrapper &);
   /// @endcond

   public:
   winapi_mutex_wrapper(void *mtx_hnd = winapi::invalid_handle_value)
      : winapi_mutex_functions(mtx_hnd)
   {}

   ~winapi_mutex_wrapper()
   {  this->close(); }

   void *release()
   {
      void *hnd = m_mtx_hnd;
      m_mtx_hnd = winapi::invalid_handle_value;
      return hnd;
   }

   void *handle() const
   {  return m_mtx_hnd; }

   bool open_or_create(const char *name, const permissions &perm)
   {
      if(m_mtx_hnd == winapi::invalid_handle_value){
         m_mtx_hnd = winapi::open_or_create_mutex
            ( name
            , false
            , (winapi::interprocess_security_attributes*)perm.get_permissions()
            );
         return m_mtx_hnd != winapi::invalid_handle_value;
      }
      else{
         return false;
      }
   }  

   void close()
   {
      if(m_mtx_hnd != winapi::invalid_handle_value){
         winapi::close_handle(m_mtx_hnd);
         m_mtx_hnd = winapi::invalid_handle_value;
      }
   }

   void swap(winapi_mutex_wrapper &other)
   {  void *tmp = m_mtx_hnd; m_mtx_hnd = other.m_mtx_hnd; other.m_mtx_hnd = tmp;   }
};

}  //namespace ipcdetail {
}  //namespace interprocess {
}  //namespace boost {

#include <boost/interprocess/detail/config_end.hpp>

#endif   //BOOST_INTERPROCESS_DETAIL_WINAPI_MUTEX_WRAPPER_HPP
