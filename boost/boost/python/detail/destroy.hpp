// Copyright David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef DESTROY_DWA2002221_HPP
# define DESTROY_DWA2002221_HPP

# include <boost/type_traits/is_array.hpp>
# include <boost/detail/workaround.hpp>
# if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
#  include <boost/type_traits/is_enum.hpp>
# endif 
namespace boost { namespace python { namespace detail { 

template <
    bool array
# if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
  , bool enum_  // vc7 has a problem destroying enums
# endif 
    > struct value_destroyer;
    
template <>
struct value_destroyer<
    false
# if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
  , false
# endif 
    >
{
    template <class T>
    static void execute(T const volatile* p)
    {
        p->T::~T();
    }
};

template <>
struct value_destroyer<
    true
# if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
  , false
# endif 
    >
{
    template <class A, class T>
    static void execute(A*, T const volatile* const first)
    {
        for (T const volatile* p = first; p != first + sizeof(A)/sizeof(T); ++p)
        {
            value_destroyer<
                boost::is_array<T>::value
# if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
              , boost::is_enum<T>::value
# endif 
            >::execute(p);
        }
    }
    
    template <class T>
    static void execute(T const volatile* p)
    {
        execute(p, *p);
    }
};

# if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
template <>
struct value_destroyer<true,true>
{
    template <class T>
    static void execute(T const volatile*)
    {
    }
};

template <>
struct value_destroyer<false,true>
{
    template <class T>
    static void execute(T const volatile*)
    {
    }
};
# endif 
template <class T>
inline void destroy_referent_impl(void* p, T& (*)())
{
    // note: cv-qualification needed for MSVC6
    // must come *before* T for metrowerks
    value_destroyer<
         (boost::is_array<T>::value)
# if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
       , (boost::is_enum<T>::value)
# endif 
    >::execute((const volatile T*)p);
}

template <class T>
inline void destroy_referent(void* p, T(*)() = 0)
{
    destroy_referent_impl(p, (T(*)())0);
}

}}} // namespace boost::python::detail

#endif // DESTROY_DWA2002221_HPP
