#ifndef BOOST_SERIALIZATION_VERSION_HPP
#define BOOST_SERIALIZATION_VERSION_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// version.hpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <boost/config.hpp>
#include <boost/mpl/int.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/mpl/integral_c_tag.hpp>

#include <boost/type_traits/is_base_and_derived.hpp>
//#include <boost/serialization/traits.hpp>

namespace boost { 
namespace serialization {

struct basic_traits;

// default version number is 0. Override with higher version
// when class definition changes.
template<class T>
struct version
{
    template<class U>
    struct traits_class_version {
        typedef BOOST_DEDUCED_TYPENAME U::version type;
    };

    typedef mpl::integral_c_tag tag;
    // note: at least one compiler complained w/o the full qualification
    // on basic traits below
    typedef
        BOOST_DEDUCED_TYPENAME mpl::eval_if<
            is_base_and_derived<boost::serialization::basic_traits,T>,
            traits_class_version<T>,
            mpl::int_<0>
        >::type type;
    BOOST_STATIC_CONSTANT(unsigned int, value = version::type::value);
};

} // namespace serialization
} // namespace boost

// specify the current version number for the class
#define BOOST_CLASS_VERSION(T, N)                                      \
namespace boost {                                                      \
namespace serialization {                                              \
template<>                                                             \
struct version<T >                                                     \
{                                                                      \
    typedef mpl::int_<N> type;                                         \
    typedef mpl::integral_c_tag tag;                                   \
    BOOST_STATIC_CONSTANT(unsigned int, value = version::type::value); \
    /* require that class info saved when versioning is used */        \
    /*                                                                 \
    BOOST_STATIC_ASSERT((                                              \
        mpl::or_<                                                      \
            mpl::equal_to<                                             \
                mpl::int_<0>,                                          \
                mpl::int_<N>                                           \
            >,                                                         \
            mpl::equal_to<                                             \
                implementation_level<T>,                               \
                mpl::int_<object_class_info>                           \
            >                                                          \
        >::value                                                       \
    ));                                                                \
    */                                                                 \
};                                                                     \
}                                                                      \
}

#endif // BOOST_SERIALIZATION_VERSION_HPP
