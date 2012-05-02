// Copyright David Abrahams 2003.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef IMPLICIT_CAST_DWA200356_HPP
# define IMPLICIT_CAST_DWA200356_HPP

# include <boost/mpl/identity.hpp>

namespace boost {

// implementation originally suggested by C. Green in
// http://lists.boost.org/MailArchives/boost/msg00886.php

// The use of identity creates a non-deduced form, so that the
// explicit template argument must be supplied
template <typename T>
inline T implicit_cast (typename mpl::identity<T>::type x) {
    return x;
}

// incomplete return type now is here
//template <typename T>
//void implicit_cast (...);

} // namespace boost


#endif // IMPLICIT_CAST_DWA200356_HPP
