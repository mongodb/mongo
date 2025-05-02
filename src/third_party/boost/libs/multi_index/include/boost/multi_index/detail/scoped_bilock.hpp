/* Copyright 2003-2022 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_SCOPED_BILOCK_HPP
#define BOOST_MULTI_INDEX_DETAIL_SCOPED_BILOCK_HPP

#if defined(_MSC_VER)
#pragma once
#endif

#include <boost/core/noncopyable.hpp>
#include <boost/type_traits/aligned_storage.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <functional>
#include <new>

namespace boost{

namespace multi_index{

namespace detail{

/* Locks/unlocks two RAII-lockable mutexes taking care that locking is done in
 * a deadlock-avoiding global order and no double locking happens when the two
 * mutexes are the same.
 */

template<typename Mutex>
class scoped_bilock:private noncopyable
{
public:
  scoped_bilock(Mutex& mutex1,Mutex& mutex2):mutex_eq(&mutex1==&mutex2)
  {
    bool mutex_lt=std::less<Mutex*>()(&mutex1,&mutex2);

    ::new (static_cast<void*>(&lock1)) scoped_lock(mutex_lt?mutex1:mutex2);
    if(!mutex_eq)
      ::new (static_cast<void*>(&lock2)) scoped_lock(mutex_lt?mutex2:mutex1);
  }

  ~scoped_bilock()
  {
    reinterpret_cast<scoped_lock*>(&lock1)->~scoped_lock();
    if(!mutex_eq)
      reinterpret_cast<scoped_lock*>(&lock2)->~scoped_lock();
  }

private:
  typedef typename Mutex::scoped_lock scoped_lock;
  typedef typename aligned_storage<
    sizeof(scoped_lock),
    alignment_of<scoped_lock>::value
  >::type                             scoped_lock_space;

  bool              mutex_eq;
  scoped_lock_space lock1,lock2;
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif
