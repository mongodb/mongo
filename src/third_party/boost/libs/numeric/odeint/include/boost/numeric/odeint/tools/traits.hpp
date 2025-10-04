//  Copyright Matt Borland 2021 - 2023.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_NUMERIC_ODEINT_TOOLS_TRAITS
#define BOOST_NUMERIC_ODEINT_TOOLS_TRAITS

#include <type_traits>

namespace boost {
namespace numeric {
namespace odeint {
namespace detail {

#define BOOST_NUMERIC_ODEINT_HAS_NAMED_TRAIT(trait, name)               \
template <typename T>                                                   \
class trait                                                             \
{                                                                       \
private:                                                                \
   using yes = char;                                                    \
   struct no { char x[2]; };                                            \
                                                                        \
   template <typename U>                                                \
   static yes test(typename U::name* = nullptr);                        \
                                                                        \
   template <typename U>                                                \
   static no test(...);                                                 \
                                                                        \
public:                                                                 \
   static constexpr bool value = (sizeof(test<T>(0)) == sizeof(char));  \
};

} //namespace detail
} //namespace odeint
} //namespace numeric
} //namespace boost

#endif //BOOST_NUMERIC_ODEINT_TOOLS_TRAITS
