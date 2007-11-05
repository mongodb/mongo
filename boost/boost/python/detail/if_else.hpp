// Copyright David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef IF_ELSE_DWA2002322_HPP
# define IF_ELSE_DWA2002322_HPP
# include <boost/config.hpp>

namespace boost { namespace python { namespace detail { 

template <class T> struct elif_selected;

template <class T>
struct if_selected
{
    template <bool b>
    struct elif : elif_selected<T>
    {
    };

    template <class U>
    struct else_
    {
        typedef T type;
    };
};

# if defined(BOOST_MSVC) && (BOOST_MSVC == 1300)
namespace msvc70_aux {

template< bool > struct inherit_from
{
    template< typename T > struct result
    {
        typedef T type;
    };
};

template<> struct inherit_from<true>
{
    template< typename T > struct result
    {
        struct type {};
    };
};

template< typename T >
struct never_true
{
    BOOST_STATIC_CONSTANT(bool, value = false);
};

} // namespace msvc70_aux

#endif // # if defined(BOOST_MSVC) && (BOOST_MSVC == 1300)

template <class T>
struct elif_selected
{
# if !(defined(BOOST_MSVC) && BOOST_MSVC <= 1300 || defined(__MWERKS__) && __MWERKS__ <= 0x2407)
    template <class U> class then;
# elif defined(BOOST_MSVC) && (BOOST_MSVC == 1300)
    template <class U>
    struct then : msvc70_aux::inherit_from< msvc70_aux::never_true<U>::value >
        ::template result< if_selected<T> >::type
    {
    };
# else
    template <class U>
    struct then : if_selected<T>
    {
    };
# endif 
};

# if !(defined(BOOST_MSVC) && BOOST_MSVC <= 1300 || defined(__MWERKS__) && __MWERKS__ <= 0x2407)
template <class T>
template <class U>
class elif_selected<T>::then : public if_selected<T>
{
};
# endif 

template <bool b> struct if_
{
    template <class T>
    struct then : if_selected<T>
    {
    };
};

struct if_unselected
{
    template <bool b> struct elif : if_<b>
    {
    };

    template <class U>
    struct else_
    {
        typedef U type;
    };
};

template <>
struct if_<false>
{
    template <class T>
    struct then : if_unselected
    {
    };
};

}}} // namespace boost::python::detail

#endif // IF_ELSE_DWA2002322_HPP
