////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2023-2024. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_TIMED_UTILS_HPP
#define BOOST_INTERPROCESS_TIMED_UTILS_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif
#
#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>
#include <boost/interprocess/detail/timed_utils.hpp>


//!\file
//!Describes some simple time-related utilities that can be used to call synchronization primitive and ipc methods that required
//!waiting until the resource is signalled or a timeout expires.
//!
//! These utilities are provided for those users that want to avoid dependence on std::chrono or boost::chrono or boost::date_time
//! and just want to implement simple portable waits.

namespace boost {
namespace interprocess {

//!Describes a simple duration type with microsecond resolution that can be used with the ustime time-point utility to call timed functions
//! of Boost.Interprocess' synchronization classes that expect a time-point (timed_wait, wait_until, timed_lock, lock_until...)
class ustime;

//!Describes a simple duration type with microsecond resolution that can be used with the ustime time-point utility to call timed functions
//! of Boost.Interprocess' synchronization classes that expect a duration type (wait_for, lock_for...)
class usduration
{
   public:
   friend class ustime;

   //!Constructs a duration type that stores microseconds from
   //!the passed count
   explicit usduration(boost::uint64_t microsecs = 0u)
      : m_microsecs(microsecs)
   {}

   //!Returns the stored microsecond
   //!count
   boost::uint64_t get_microsecs() const
   {  return m_microsecs;  }

   bool operator < (const usduration &other) const
   {  return m_microsecs < other.m_microsecs; }

   bool operator > (const usduration &other) const
   {  return m_microsecs > other.m_microsecs; }

   bool operator <= (const usduration &other) const
   {  return m_microsecs <= other.m_microsecs; }

   bool operator >= (const usduration &other) const
   {  return m_microsecs >= other.m_microsecs; }

   private:
   boost::uint64_t m_microsecs;
};

class ustime
{
   public:
   //!Constructs a time point that is "microsecs" duration away
   //!from the epoch of the system
   explicit ustime(boost::uint64_t microsecs = 0u)
      : m_microsecs(microsecs)
   {}

   ustime &operator += (const usduration &other)
   {  m_microsecs += other.m_microsecs; return *this; }

   ustime operator + (const usduration &other)
   {  ustime r(*this); r += other; return r; }

   ustime &operator -= (const usduration &other)
   {  m_microsecs -= other.m_microsecs; return *this; }

   ustime operator - (const usduration &other)
   {  ustime r(*this); r -= other; return r; }

   friend usduration operator - (const ustime &l, const ustime &r)
   {  return usduration(l.m_microsecs - r.m_microsecs); }

   bool operator < (const ustime &other) const
   {  return m_microsecs < other.m_microsecs; }

   bool operator > (const ustime &other) const
   {  return m_microsecs > other.m_microsecs; }

   bool operator <= (const ustime &other) const
   {  return m_microsecs <= other.m_microsecs; }

   bool operator >= (const ustime &other) const
   {  return m_microsecs >= other.m_microsecs; }

   //!Returns the stored count
   //!that represents microseconds from epoch
   boost::uint64_t get_microsecs() const
   {  return m_microsecs;  }

   private:
   boost::uint64_t m_microsecs;
};

//!Utility that returns a duration from
//!a seconds count
inline usduration usduration_from_seconds(boost::uint64_t sec)
{  return usduration(sec*uint64_t(1000000u));   }

//!Utility that returns a duration from
//!a milliseconds count
inline usduration usduration_from_milliseconds(boost::uint64_t millisec)
{  return usduration(millisec*1000u);   }

//!Utility that returns a time_point in the future that is "msecs"
//!milliseconds in the future from now.
inline ustime ustime_delay_milliseconds(unsigned msecs)
{
   return ustime(ipcdetail::universal_time_u64_us()) + usduration(msecs*1000u);
}

#if !defined(BOOST_INTERPROCESS_DOXYGEN_INVOKED)

namespace ipcdetail {

template<>
class microsec_clock<ustime>
{
   public:
   typedef ustime time_point;

   static ustime universal_time()
   {  return ustime(universal_time_u64_us());   }
};

// duration_to_usduration

template<class Duration>
inline usduration duration_to_usduration(const Duration &d, typename enable_if_ptime_duration<Duration>::type* = 0)
{
   return usduration(static_cast<boost::uint64_t>(d.total_microseconds()));
}

template<class Duration>
inline usduration duration_to_usduration(const Duration &d, typename enable_if_duration<Duration>::type* = 0)
{
   const double factor = double(Duration::period::num)*1000000.0/double(Duration::period::den);
   return usduration(static_cast<boost::uint64_t>(double(d.count())*factor));
}

inline usduration duration_to_usduration(const usduration &d)
{
   return d;
}

// duration_to_ustime

template<class Duration>
inline ustime duration_to_ustime(const Duration &d)
{
   return microsec_clock<ustime>::universal_time() + (duration_to_usduration)(d);
}


}  //namespace ipcdetail {

#endif   //#if !defined(BOOST_INTERPROCESS_DOXYGEN_INVOKED)

}  //namespace interprocess {
}  //namespace boost {

#include <boost/interprocess/detail/config_end.hpp>

#endif   //BOOST_INTERPROCESS_TIMED_UTILS_HPP
