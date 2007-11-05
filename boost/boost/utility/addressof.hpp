// Copyright (C) 2002 Brad King (brad.king@kitware.com) 
//                    Douglas Gregor (gregod@cs.rpi.edu)
//                    Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// For more information, see http://www.boost.org

#ifndef BOOST_UTILITY_ADDRESSOF_HPP
# define BOOST_UTILITY_ADDRESSOF_HPP

# include <boost/config.hpp>
# include <boost/detail/workaround.hpp>

namespace boost {

// Do not make addressof() inline. Breaks MSVC 7. (Peter Dimov)

// VC7 strips const from nested classes unless we add indirection here
# if BOOST_WORKAROUND(BOOST_MSVC, == 1300)

template<class T> struct _addp
{
    typedef T * type;
};
    
template <typename T> typename _addp<T>::type

# else
template <typename T> T*
# endif
addressof(T& v)
{
  return reinterpret_cast<T*>(
       &const_cast<char&>(reinterpret_cast<const volatile char &>(v)));
}

// Borland doesn't like casting an array reference to a char reference
// but these overloads work around the problem.
# if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
template<typename T,std::size_t N>
T (*addressof(T (&t)[N]))[N]
{
   return reinterpret_cast<T(*)[N]>(&t);
}

template<typename T,std::size_t N>
const T (*addressof(const T (&t)[N]))[N]
{
   return reinterpret_cast<const T(*)[N]>(&t);
}
# endif

}

#endif // BOOST_UTILITY_ADDRESSOF_HPP
