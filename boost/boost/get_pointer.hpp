// Copyright Peter Dimov and David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef GET_POINTER_DWA20021219_HPP
# define GET_POINTER_DWA20021219_HPP

# include <memory>

namespace boost { 

// get_pointer(p) extracts a ->* capable pointer from p

template<class T> T * get_pointer(T * p)
{
    return p;
}

// get_pointer(shared_ptr<T> const & p) has been moved to shared_ptr.hpp

template<class T> T * get_pointer(std::auto_ptr<T> const& p)
{
    return p.get();
}


} // namespace boost

#endif // GET_POINTER_DWA20021219_HPP
