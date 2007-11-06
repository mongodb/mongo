// Copyright David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef MSVC_TYPEINFO_DWA200222_HPP
# define MSVC_TYPEINFO_DWA200222_HPP

#include <typeinfo>
#include <boost/type.hpp>
#include <boost/type_traits/config.hpp>

//
// Fix for MSVC's broken typeid() implementation which doesn't strip
// decoration. This fix doesn't handle cv-qualified array types. It
// could probably be done, but I haven't figured it out yet.
//

# if defined(BOOST_MSVC) && BOOST_MSVC <= 1300 || defined(BOOST_INTEL_CXX_VERSION) && BOOST_INTEL_CXX_VERSION <= 700

namespace boost { namespace python { namespace detail { 

typedef std::type_info const& typeinfo;

template <class T>
static typeinfo typeid_nonref(T const volatile*) { return typeid(T); }

template <class T>
inline typeinfo typeid_ref_1(T&(*)())
{
    return detail::typeid_nonref((T*)0);
}

// A non-reference
template <class T>
inline typeinfo typeid_ref(type<T>*, T&(*)(type<T>))
{
    return detail::typeid_nonref((T*)0);
}

// A reference
template <class T>
inline typeinfo typeid_ref(type<T>*, ...)
{
    return detail::typeid_ref_1((T(*)())0);
}

template< typename T > T&(* is_ref_tester1(type<T>) )(type<T>) { return 0; }
inline char BOOST_TT_DECL is_ref_tester1(...) { return 0; }

template <class T>
inline typeinfo msvc_typeid(boost::type<T>*)
{
    return detail::typeid_ref(
        (boost::type<T>*)0, detail::is_ref_tester1(type<T>())
        );
}

template <>
inline typeinfo msvc_typeid<void>(boost::type<void>*)
{
    return typeid(void);
}

#  ifndef NDEBUG
inline typeinfo assert_array_typeid_compiles()
{
    return msvc_typeid((boost::type<char const[3]>*)0)
        , msvc_typeid((boost::type<char[3]>*)0);
}
#  endif

}}} // namespace boost::python::detail

# endif // BOOST_MSVC
#endif // MSVC_TYPEINFO_DWA200222_HPP
