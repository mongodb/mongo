// Copyright David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BASES_DWA2002321_HPP
# define BASES_DWA2002321_HPP

# include <boost/python/detail/prefix.hpp>
# include <boost/type_traits/object_traits.hpp>
# include <boost/python/detail/type_list.hpp>
# include <boost/mpl/if.hpp>
# include <boost/mpl/bool.hpp>
# include <boost/preprocessor/enum_params_with_a_default.hpp>
# include <boost/preprocessor/enum_params.hpp>

namespace boost { namespace python { 

# define BOOST_PYTHON_BASE_PARAMS BOOST_PP_ENUM_PARAMS_Z(1, BOOST_PYTHON_MAX_BASES, Base)

  // A type list for specifying bases
  template < BOOST_PP_ENUM_PARAMS_WITH_A_DEFAULT(BOOST_PYTHON_MAX_BASES, typename Base, mpl::void_) >
  struct bases : detail::type_list< BOOST_PYTHON_BASE_PARAMS >::type
  {};

  namespace detail
  {
# ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
    template <class T> struct specifies_bases
        : mpl::false_
    {
    };
    
    template < BOOST_PP_ENUM_PARAMS_Z(1, BOOST_PYTHON_MAX_BASES, class Base) >
    struct specifies_bases< bases< BOOST_PYTHON_BASE_PARAMS > >
        : mpl::true_
    {
    };
# else
    template < BOOST_PP_ENUM_PARAMS(BOOST_PYTHON_MAX_BASES, class Base) >
    static char is_bases_helper(bases< BOOST_PYTHON_BASE_PARAMS > const&);
    
    static char (& is_bases_helper(...) )[256];

    template <class T>
    struct specifies_bases
    {
     private:
        static typename add_reference<T>::type make();
        BOOST_STATIC_CONSTANT(bool, non_ref = !is_reference<T>::value);
     public:
        BOOST_STATIC_CONSTANT(bool, value = non_ref & (sizeof(is_bases_helper(make())) == 1));
        typedef mpl::bool_<value> type;
    };
# endif
    template <class T, class Prev = bases<> >
    struct select_bases
        : mpl::if_<
                specifies_bases<T>
                , T
                , Prev
          >
    {
    };
  }
# undef BOOST_PYTHON_BASE_PARAMS
}} // namespace boost::python

#endif // BASES_DWA2002321_HPP
