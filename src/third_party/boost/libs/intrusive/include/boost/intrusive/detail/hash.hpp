/////////////////////////////////////////////////////////////////////////////
//
// Copyright 2005-2014 Daniel James.
// Copyright 2021, 2022 Peter Dimov.
// Copyright 2024 Ion Gaztanaga.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt
//
// Based on Peter Dimov's proposal
// http://www.open-std.org/JTC1/SC22/WG21/docs/papers/2005/n1756.pdf
// issue 6.18.
//
// The original C++11 implementation was done by Peter Dimov
// The C++03 porting was done by Ion Gaztanaga.
// 
// The goal of this header is to avoid Intrusive's hard dependency on ContainerHash,
// which adds additional dependencies and the minimum supported C++ standard can
// differ between both libraries. However, a compatibility protocol is used so that
// users compatible with ContainerHash are also compatible with Intrusive:
// 
// - If users define `hash_value` (as required by boost::hash) for their classes
//   are automatically compatible with Intrusive unordered containers.
//
// - If users include boost/container_hash/hash.hpp in their headers, Intrusive
//   unordered containers will take advantage of boost::hash compatibility hash functions
//   (such as hashing functions for range-compatible types, standard containers, etc.)
// 
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_HASH_HASH_HPP
#define BOOST_INTRUSIVE_HASH_HASH_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/hash_integral.hpp>
#include <boost/intrusive/detail/hash_mix.hpp>
#include <boost/intrusive/detail/hash_combine.hpp>
#include <boost/cstdint.hpp>
#include <climits>
#include <cstring>
#include <cfloat>
#include <boost/intrusive/detail/mpl.hpp>

namespace boost {

template<class T>
struct hash;

} //namespace boost

//Fallback function to call boost::hash if scalar type and ADL fail.
//The user must include boost/container_hash/hash.hpp when to make this call work,
//this allows boost::intrusive to be compatible with boost::hash without
//a mandatory physical (header inclusion) dependency
namespace boost_intrusive_adl
{
   template<class T>
   inline std::size_t hash_value(const T& v)
   {
      return boost::hash<T>()(v);
   }
}

namespace boost {
namespace intrusive {
namespace detail {

//ADL-based lookup hash call
template <class T>
inline typename detail::disable_if_c<detail::is_scalar<T>::value, std::size_t>::type
   hash_value_dispatch(const T& v)
{
   //Try ADL lookup, if it fails, boost_intrusive_adl::hash_value will retry with boost::hash
   using boost_intrusive_adl::hash_value;
   return hash_value(v);
}

template <typename T>
typename enable_if_c<is_enum<T>::value, std::size_t>::type
   hash_value( T v )
{
   return static_cast<std::size_t>( v );
}

////////////////////////////////////////////////////////////   
//
//          floating point types
//
////////////////////////////////////////////////////////////

template<class T, std::size_t Bits = sizeof(T) * CHAR_BIT>
struct hash_float_impl;

// float
template<class T> struct hash_float_impl<T, 32>
{
   static std::size_t fn( T v )
   {
         boost::uint32_t w;
         std::memcpy( &w, &v, sizeof( v ) );

         return w;
   }
};

// double
template<class T> struct hash_float_impl<T, 64>
{
   static std::size_t fn( T v )
   {
         boost::uint64_t w;
         std::memcpy( &w, &v, sizeof( v ) );

         return hash_value( w );
   }
};

// 80 bit long double in 12 bytes
template<class T> struct hash_float_impl<T, 96>
{
   static std::size_t fn( T v )
   {
         boost::uint64_t w[ 2 ] = {};
         std::memcpy( &w, &v, 80 / CHAR_BIT );

         std::size_t seed = 0;

         seed = hash_value( w[0] ) + (hash_mix)( seed );
         seed = hash_value( w[1] ) + (hash_mix)( seed );

         return seed;
   }
};

#if (LDBL_MAX_10_EXP == 4932)

// 80 bit long double in 16 bytes
template<class T> struct hash_float_impl<T, 128>
{
   static std::size_t fn( T v )
   {
      boost::uint64_t w[ 2 ] = {};
      std::memcpy( &w, &v, 80 / CHAR_BIT );

      std::size_t seed = 0;

      seed = hash_value( w[0] ) + (hash_mix)( seed );
      seed = hash_value( w[1] ) + (hash_mix)( seed );

      return seed;
   }
};

#elif (LDBL_MAX_10_EXP  > 4932)
// 128 bit long double
template<class T> struct hash_float_impl<T, 128>
{
   static std::size_t fn( T v )
   {
      boost::uint64_t w[ 2 ];
      std::memcpy( &w, &v, sizeof( v ) );

      std::size_t seed = 0;

   #if defined(__FLOAT_WORD_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __FLOAT_WORD_ORDER__ == __ORDER_BIG_ENDIAN__

      seed = hash_value( w[1] ) + (hash_mix)( seed );
      seed = hash_value( w[0] ) + (hash_mix)( seed );

   #else

      seed = hash_value( w[0] ) + (hash_mix)( seed );
      seed = hash_value( w[1] ) + (hash_mix)( seed );

   #endif
      return seed;
   }
};
#endif   //#if (LDBL_MAX_10_EXP  == 4932)

template <typename T>
typename enable_if_c<is_floating_point<T>::value, std::size_t>::type
   hash_value( T v )
{
   return boost::intrusive::detail::hash_float_impl<T>::fn( v + 0 );
}

////////////////////////////////////////////////////////////
//
//          pointer types
//
////////////////////////////////////////////////////////////
// `x + (x >> 3)` adjustment by Alberto Barbati and Dave Harris.
template <class T> std::size_t hash_value( T* const& v )
{
   std::size_t x = reinterpret_cast<std::size_t>( v );
   return hash_value( x + (x >> 3) );
}

////////////////////////////////////////////////////////////
//
//          std::nullptr_t
//
////////////////////////////////////////////////////////////
#if !defined(BOOST_NO_CXX11_NULLPTR)
template <typename T>
typename enable_if_c<is_same<T, std::nullptr_t>::value, std::size_t>::type
   hash_value( T const &)
{
   return (hash_value)( static_cast<void*>( nullptr ) );
}
   #endif

////////////////////////////////////////////////////////////
//
//          Array types
//
////////////////////////////////////////////////////////////

//Forward declaration or internal hash functor, for array iteration
template<class T>
struct internal_hash_functor;

template<class T, std::size_t N>
inline std::size_t hash_value_dispatch( T const (&x)[ N ] )
{
   std::size_t seed = 0;
   for(std::size_t i = 0; i != N; ++i){
      hash_combine_size_t(seed, internal_hash_functor<T>()(x[i]));
   }
   return seed;
}

template<class T, std::size_t N>
inline std::size_t hash_value_dispatch( T (&x)[ N ] )
{
   std::size_t seed = 0;
   for (std::size_t i = 0; i != N; ++i) {
      hash_combine_size_t(seed, internal_hash_functor<T>()(x[i]));
   }
   return seed;
}

////////////////////////////////////////////////////////////
//
//          Scalar types, calls proper overload
//
////////////////////////////////////////////////////////////
template <class T>
inline typename detail::enable_if_c<detail::is_scalar<T>::value, std::size_t>::type
   hash_value_dispatch(const T &v)
{
   return boost::intrusive::detail::hash_value(v);
}

//Internal "anonymous" hash functor, first selects between "built-in" scalar/array types
//and ADL-based lookup

template<class T>
struct internal_hash_functor
{
   inline std::size_t operator()(T const& val) const
   {
      return ::boost::intrusive::detail::hash_value_dispatch(val);
   }
};

} // namespace detail {
} // namespace intrusive {
} // namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif // #ifndef BOOST_INTRUSIVE_HASH_HASH_HPP

