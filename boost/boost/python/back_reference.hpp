// Copyright David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BACK_REFERENCE_DWA2002510_HPP
# define BACK_REFERENCE_DWA2002510_HPP

# include <boost/python/detail/prefix.hpp>

# include <boost/python/object_fwd.hpp>
# include <boost/python/detail/dependent.hpp>
# include <boost/python/detail/raw_pyobject.hpp>

namespace boost { namespace python { 

template <class T>
struct back_reference
{
 private: // types
    typedef typename detail::dependent<object,T>::type source_t;
 public:
    typedef T type;
    
    back_reference(PyObject*, T);
    source_t const& source() const;
    T get() const;
 private:
    source_t m_source;
    T m_value;
};

# ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
template<typename T>
class is_back_reference
{
 public:
    BOOST_STATIC_CONSTANT(bool, value = false); 
};

template<typename T>
class is_back_reference<back_reference<T> >
{
 public:
    BOOST_STATIC_CONSTANT(bool, value = true);
};

# else // no partial specialization

}} // namespace boost::python

#include <boost/type.hpp>

namespace boost { namespace python {

namespace detail
{
  typedef char (&yes_back_reference_t)[1];
  typedef char (&no_back_reference_t)[2];
      
  no_back_reference_t is_back_reference_test(...);

  template<typename T>
  yes_back_reference_t is_back_reference_test(boost::type< back_reference<T> >);
}

template<typename T>
class is_back_reference
{
 public:
    BOOST_STATIC_CONSTANT(
        bool, value = (
            sizeof(detail::is_back_reference_test(boost::type<T>()))
            == sizeof(detail::yes_back_reference_t)));
};

# endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

//
// implementations
//
template <class T>
back_reference<T>::back_reference(PyObject* p, T x)
    : m_source(detail::borrowed_reference(p))
      , m_value(x)
{
}

template <class T>
typename back_reference<T>::source_t const& back_reference<T>::source() const
{
    return m_source;
}

template <class T>
T back_reference<T>::get() const
{
    return m_value;
}

}} // namespace boost::python

#endif // BACK_REFERENCE_DWA2002510_HPP
