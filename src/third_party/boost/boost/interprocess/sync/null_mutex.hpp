//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_NULL_MUTEX_HPP
#define BOOST_INTERPROCESS_NULL_MUTEX_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif
#
#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>


//!\file
//!Describes null_mutex classes

namespace boost {
namespace interprocess {

//!Implements a mutex that simulates a mutex without doing any operation and
//!simulates a successful operation.
class null_mutex
{
   #if !defined(BOOST_INTERPROCESS_DOXYGEN_INVOKED)
   null_mutex(const null_mutex&);
   null_mutex &operator= (const null_mutex&);
   #endif   //#ifndef BOOST_INTERPROCESS_DOXYGEN_INVOKED
   public:

   //!Constructor.
   //!Empty.
   null_mutex() BOOST_NOEXCEPT {}

   //!Destructor.
   //!Empty.
   ~null_mutex(){}

   //!Simulates a mutex lock() operation. Empty function.
   void lock(){}

   //!Simulates a mutex try_lock() operation.
   //!Equivalent to "return true;"
   bool try_lock()
   {  return true;   }

   //!Simulates a mutex timed_lock() operation.
   //!Equivalent to "return true;"
   template<class TimePoint>
   bool timed_lock(const TimePoint &)
   {  return true;   }

   //!Same as `timed_lock`, but this function is modeled after the
   //!standard library interface.
   template<class TimePoint>
   bool try_lock_until(const TimePoint &)
   {  return true;  }

   //!Same as `timed_lock`, but this function is modeled after the
   //!standard library interface.
   template<class Duration> 
   bool try_lock_for(const Duration &)
   {  return true; }

   //!Simulates a mutex unlock() operation.
   //!Empty function.
   void unlock(){}

   //!Simulates a mutex lock_sharable() operation.
   //!Empty function.
   void lock_sharable(){}

   //!Same as `lock_sharable` but with a std-compatible interface
   //! 
   void lock_shared()
   {  this->lock_sharable();  }

   //!Simulates a mutex try_lock_sharable() operation.
   //!Equivalent to "return true;"
   bool try_lock_sharable()
   {  return true;   }

   //!Same as `try_lock_sharable` but with a std-compatible interface
   //! 
   bool try_lock_shared()
   {  return this->try_lock_sharable();  }

   //!Simulates a mutex timed_lock_sharable() operation.
   //!Equivalent to "return true;"
   template<class TimePoint>
   bool timed_lock_sharable(const TimePoint &)
   {  return true;   }

   //!Simulates a mutex unlock_sharable() operation.
   //!Empty function.
   void unlock_sharable(){}

   //!Same as `unlock_sharable` but with a std-compatible interface
   //! 
   void unlock_shared()
   {  this->unlock_sharable();  }

   //!Simulates a mutex lock_upgradable() operation.
   //!Empty function.
   void lock_upgradable(){}

   //!Simulates a mutex try_lock_upgradable() operation.
   //!Equivalent to "return true;"
   bool try_lock_upgradable()
   {  return true;   }

   //!Simulates a mutex timed_lock_upgradable() operation.
   //!Equivalent to "return true;"
   template<class TimePoint>
   bool timed_lock_upgradable(const TimePoint &)
   {  return true;   }

   //!Simulates a mutex unlock_upgradable() operation.
   //!Empty function.
   void unlock_upgradable(){}

   //!Simulates unlock_and_lock_upgradable().
   //!Empty function.
   void unlock_and_lock_upgradable(){}

   //!Simulates unlock_and_lock_sharable().
   //!Empty function.
   void unlock_and_lock_sharable(){}

   //!Simulates unlock_upgradable_and_lock_sharable().
   //!Empty function.
   void unlock_upgradable_and_lock_sharable(){}

   //Promotions

   //!Simulates unlock_upgradable_and_lock().
   //!Empty function.
   void unlock_upgradable_and_lock(){}

   //!Simulates try_unlock_upgradable_and_lock().
   //!Equivalent to "return true;"
   bool try_unlock_upgradable_and_lock()
   {  return true;   }

   //!Simulates timed_unlock_upgradable_and_lock().
   //!Equivalent to "return true;"
   template<class TimePoint>
   bool timed_unlock_upgradable_and_lock(const TimePoint &)
   {  return true;   }

   //!Simulates try_unlock_sharable_and_lock().
   //!Equivalent to "return true;"
   bool try_unlock_sharable_and_lock()
   {  return true;   }

   //!Simulates try_unlock_sharable_and_lock_upgradable().
   //!Equivalent to "return true;"
   bool try_unlock_sharable_and_lock_upgradable()
   {  return true;   }
};

}  //namespace interprocess {
}  //namespace boost {

#include <boost/interprocess/detail/config_end.hpp>

#endif   //BOOST_INTERPROCESS_NULL_MUTEX_HPP
