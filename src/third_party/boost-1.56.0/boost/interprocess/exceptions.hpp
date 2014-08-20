//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_EXCEPTIONS_HPP
#define BOOST_INTERPROCESS_EXCEPTIONS_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>
#include <boost/interprocess/errors.hpp>
#include <stdexcept>
#include <new>

//!\file
//!Describes exceptions thrown by interprocess classes

namespace boost {

namespace interprocess {

//!This class is the base class of all exceptions
//!thrown by boost::interprocess
class interprocess_exception : public std::exception
{
   public:
   interprocess_exception(const char *err/*error_code_t ec = other_error*/)
      :  m_err(other_error)
   {
//      try   {  m_str = "boost::interprocess_exception::library_error"; }
      try   {  m_str = err; }
      catch (...) {}
   }
/*
   interprocess_exception(native_error_t sys_err_code)
      :  m_err(sys_err_code)
   {
      try   {  fill_system_message(m_err.get_native_error(), m_str); }
      catch (...) {}
   }*/

   interprocess_exception(const error_info &err_info, const char *str = 0)
      :  m_err(err_info)
   {
      try{
         if(m_err.get_native_error() != 0){
            fill_system_message(m_err.get_native_error(), m_str);
         }
         else if(str){
            m_str = str;
         }
         else{
            m_str = "boost::interprocess_exception::library_error";
         }
      }
      catch(...){}
   }

   virtual ~interprocess_exception() throw(){}

   virtual const char * what() const throw()
   {  return m_str.c_str();  }

   native_error_t get_native_error()const { return m_err.get_native_error(); }

   // Note: a value of other_error implies a library (rather than system) error
   error_code_t   get_error_code()  const { return m_err.get_error_code(); }

   /// @cond
   private:
   error_info        m_err;
   std::string       m_str;
   /// @endcond
};

//!This is the exception thrown by shared interprocess_mutex family when a deadlock situation
//!is detected or when using a interprocess_condition the interprocess_mutex is not locked
class lock_exception : public interprocess_exception
{
   public:
   lock_exception()
      :  interprocess_exception(lock_error)
   {}

   virtual const char* what() const throw()
   {  return "boost::interprocess::lock_exception";  }
};

//!This is the exception thrown by named interprocess_semaphore when a deadlock situation
//!is detected or when an error is detected in the post/wait operation
/*
class sem_exception : public interprocess_exception
{
   public:
   sem_exception()
      :  interprocess_exception(lock_error)
   {}

   virtual const char* what() const throw()
   {  return "boost::interprocess::sem_exception";  }
};
*/
//!This is the exception thrown by synchronization objects when there is
//!an error in a wait() function
/*
class wait_exception : public interprocess_exception
{
   public:
   virtual const char* what() const throw()
   {  return "boost::interprocess::wait_exception";  }
};
*/

//!This exception is thrown when a named object is created
//!in "open_only" mode and the resource was not already created
/*
class not_previously_created : public interprocess_exception
{
 public:
    virtual const char* what() const throw()
      {  return "boost::interprocess::not_previously_created";  }
};
*/

//!This exception is thrown when a memory request can't be
//!fulfilled.
class bad_alloc : public interprocess_exception
{
 public:
   bad_alloc() : interprocess_exception("::boost::interprocess::bad_alloc"){}
   virtual const char* what() const throw()
      {  return "boost::interprocess::bad_alloc";  }
};

}  // namespace interprocess {

}  // namespace boost

#include <boost/interprocess/detail/config_end.hpp>

#endif // BOOST_INTERPROCESS_EXCEPTIONS_HPP
