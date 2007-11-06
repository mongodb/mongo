// Copyright David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef MODULE_INIT_DWA20020722_HPP
# define MODULE_INIT_DWA20020722_HPP

# include <boost/python/detail/prefix.hpp>

# ifndef BOOST_PYTHON_MODULE_INIT

namespace boost { namespace python { namespace detail {

BOOST_PYTHON_DECL void init_module(char const* name, void(*)());

}}}

#  if (defined(_WIN32) || defined(__CYGWIN__)) && !defined(BOOST_PYTHON_STATIC_MODULE)

#   define BOOST_PYTHON_MODULE_INIT(name)               \
void init_module_##name();                              \
extern "C" __declspec(dllexport) void init##name()      \
{                                                       \
    boost::python::detail::init_module(                 \
        #name,&init_module_##name);                     \
}                                                       \
void init_module_##name()

#  elif BOOST_PYTHON_USE_GCC_SYMBOL_VISIBILITY

#   define BOOST_PYTHON_MODULE_INIT(name)                               \
void init_module_##name();                                              \
extern "C" __attribute__ ((visibility("default"))) void init##name()    \
{                                                                       \
    boost::python::detail::init_module(#name, &init_module_##name);     \
}                                                                       \
void init_module_##name()

#  else

#   define BOOST_PYTHON_MODULE_INIT(name)                               \
void init_module_##name();                                              \
extern "C"  void init##name()                                           \
{                                                                       \
    boost::python::detail::init_module(#name, &init_module_##name);     \
}                                                                       \
void init_module_##name()

#  endif

# endif 

#endif // MODULE_INIT_DWA20020722_HPP
