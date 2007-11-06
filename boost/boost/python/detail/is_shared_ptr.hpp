// Copyright David Abrahams 2003.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef IS_SHARED_PTR_DWA2003224_HPP
# define IS_SHARED_PTR_DWA2003224_HPP

# include <boost/python/detail/is_xxx.hpp>
# include <boost/shared_ptr.hpp>

namespace boost { namespace python { namespace detail { 

BOOST_PYTHON_IS_XXX_DEF(shared_ptr, shared_ptr, 1)
    
}}} // namespace boost::python::detail

#endif // IS_SHARED_PTR_DWA2003224_HPP
