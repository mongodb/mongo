// Copyright David Abrahams 2003.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef VALUE_IS_XXX_DWA2003224_HPP
# define VALUE_IS_XXX_DWA2003224_HPP

# include <boost/config.hpp>
# include <boost/mpl/bool.hpp>
# include <boost/preprocessor/enum_params.hpp>

# if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
# include <boost/type_traits/is_reference.hpp>
# include <boost/type_traits/add_reference.hpp>

#  define BOOST_PYTHON_VALUE_IS_XXX_DEF(name, qualified_name, nargs)    \
template <class X_>                                                     \
struct value_is_##name                                                  \
{                                                                       \
    typedef char yes;                                                   \
    typedef char (&no)[2];                                              \
                                                                        \
    static typename add_reference<X_>::type dummy;                      \
                                                                        \
    template < BOOST_PP_ENUM_PARAMS_Z(1, nargs, class U) >              \
    static yes test(                                                    \
       qualified_name< BOOST_PP_ENUM_PARAMS_Z(1, nargs, U) > const&, int \
    );                                                                  \
                                                                        \
    template <class U>                                                  \
    static no test(U&, ...);                                            \
                                                                        \
    BOOST_STATIC_CONSTANT(                                              \
        bool, value                                                     \
        = (sizeof(test(dummy, 0)) == sizeof(yes)));                     \
                                                                        \
    typedef mpl::bool_<value> type;                                    \
};

# else

#  include <boost/type_traits/remove_reference.hpp>
#  include <boost/type_traits/remove_cv.hpp>
#  include <boost/python/detail/is_xxx.hpp>

#  define BOOST_PYTHON_VALUE_IS_XXX_DEF(name, qualified_name, nargs)    \
template <class X_>                                                     \
struct value_is_##name                                                  \
{                                                                       \
    BOOST_PYTHON_IS_XXX_DEF(name,qualified_name,nargs)                  \
    BOOST_STATIC_CONSTANT(bool, value = is_##name<                      \
                               typename remove_cv<                      \
                                  typename remove_reference<X_>::type   \
                               >::type                                  \
                           >::value);                                   \
    typedef mpl::bool_<value> type;                                    \
                                                                        \
};                                                              

# endif

#endif // VALUE_IS_XXX_DWA2003224_HPP
