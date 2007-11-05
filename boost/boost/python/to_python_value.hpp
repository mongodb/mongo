// Copyright David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef TO_PYTHON_VALUE_DWA200221_HPP
# define TO_PYTHON_VALUE_DWA200221_HPP

# include <boost/python/detail/prefix.hpp>

# include <boost/python/refcount.hpp>
# include <boost/python/tag.hpp>

# include <boost/python/converter/registry.hpp>
# include <boost/python/converter/registered.hpp>
# include <boost/python/converter/builtin_converters.hpp>
# include <boost/python/converter/object_manager.hpp>
# include <boost/python/converter/object_manager.hpp>
# include <boost/python/converter/shared_ptr_to_python.hpp>

# include <boost/python/detail/value_is_shared_ptr.hpp>
# include <boost/python/detail/value_arg.hpp>

# include <boost/type_traits/transform_traits.hpp>

# include <boost/mpl/if.hpp>
# include <boost/mpl/or.hpp>

namespace boost { namespace python { 

namespace detail
{
  template <class T>
  struct object_manager_to_python_value
  {
      typedef typename value_arg<T>::type argument_type;
    
      PyObject* operator()(argument_type) const;

      // This information helps make_getter() decide whether to try to
      // return an internal reference or not. I don't like it much,
      // but it will have to serve for now.
      BOOST_STATIC_CONSTANT(bool, uses_registry = false);
  };

  
  template <class T>
  struct registry_to_python_value
  {
      typedef typename value_arg<T>::type argument_type;
    
      PyObject* operator()(argument_type) const;

      // This information helps make_getter() decide whether to try to
      // return an internal reference or not. I don't like it much,
      // but it will have to serve for now.
      BOOST_STATIC_CONSTANT(bool, uses_registry = true);
  };

  template <class T>
  struct shared_ptr_to_python_value
  {
      typedef typename value_arg<T>::type argument_type;
    
      PyObject* operator()(argument_type) const;

      // This information helps make_getter() decide whether to try to
      // return an internal reference or not. I don't like it much,
      // but it will have to serve for now.
      BOOST_STATIC_CONSTANT(bool, uses_registry = false);
  };
}

template <class T>
struct to_python_value
    : mpl::if_<
          detail::value_is_shared_ptr<T>
        , detail::shared_ptr_to_python_value<T>
        , typename mpl::if_<
              mpl::or_<
                  converter::is_object_manager<T>
                , converter::is_reference_to_object_manager<T>
              >
            , detail::object_manager_to_python_value<T>
            , detail::registry_to_python_value<T>
          >::type
      >::type
{
};

//
// implementation 
//
namespace detail
{
  template <class T>
  inline PyObject* registry_to_python_value<T>::operator()(argument_type x) const
  {
      typedef converter::registered<argument_type> r;
# if BOOST_WORKAROUND(__GNUC__, < 3)
      // suppresses an ICE, somehow
      (void)r::converters;
# endif 
      return converter::registered<argument_type>::converters.to_python(&x);
  }

  template <class T>
  inline PyObject* object_manager_to_python_value<T>::operator()(argument_type x) const
  {
      return python::upcast<PyObject>(
          python::xincref(
              get_managed_object(x, tag))
          );
  }

  template <class T>
  inline PyObject* shared_ptr_to_python_value<T>::operator()(argument_type x) const
  {
      return converter::shared_ptr_to_python(x);
  }
}

}} // namespace boost::python

#endif // TO_PYTHON_VALUE_DWA200221_HPP
