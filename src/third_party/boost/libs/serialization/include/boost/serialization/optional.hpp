/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8

// (C) Copyright 2002-4 Pavel Vozenilek .
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Provides non-intrusive serialization for boost::optional.

#ifndef BOOST_SERIALIZATION_OPTIONAL_HPP
#define BOOST_SERIALIZATION_OPTIONAL_HPP

#if defined(_MSC_VER)
# pragma once
#endif

#include <boost/config.hpp>
#include <boost/optional.hpp>
#ifndef BOOST_NO_CXX17_HDR_OPTIONAL
#include <optional>
#endif

#include <boost/serialization/item_version_type.hpp>
#include <boost/serialization/library_version_type.hpp>
#include <boost/serialization/version.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/type_traits/is_pointer.hpp>
#include <boost/serialization/detail/is_default_constructible.hpp>

// function specializations must be defined in the appropriate
// namespace - boost::serialization
namespace boost {
namespace serialization {
namespace detail {

// OT is of the form optional<T>
template<class Archive, class OT>
void save_impl(
    Archive & ar,
    const OT & ot
){
    // It is an inherent limitation to the serialization of optional.hpp
    // that the underlying type must be either a pointer or must have a
    // default constructor.  It's possible that this could change sometime
    // in the future, but for now, one will have to work around it.  This can
    // be done by serialization the optional<T> as optional<T *>
    #ifndef BOOST_NO_CXX11_HDR_TYPE_TRAITS
        BOOST_STATIC_ASSERT(
            boost::serialization::detail::is_default_constructible<typename OT::value_type>::value
            || boost::is_pointer<typename OT::value_type>::value
        );
    #endif
    const bool tflag(ot);
    ar << boost::serialization::make_nvp("initialized", tflag);
    if (tflag){
        ar << boost::serialization::make_nvp("value", *ot);
    }
}

// OT is of the form optional<T>
template<class Archive, class OT>
void load_impl(
    Archive & ar,
    OT & ot,
    const unsigned int version
){
    bool tflag;
    ar >> boost::serialization::make_nvp("initialized", tflag);
    if(! tflag){
        ot.reset();
        return;
    }

    if(0 == version){
        boost::serialization::item_version_type item_version(0);
        boost::serialization::library_version_type library_version(
            ar.get_library_version()
        );
        if(boost::serialization::library_version_type(3) < library_version){
            ar >> BOOST_SERIALIZATION_NVP(item_version);
        }
    }
    typename OT::value_type t;
    ar >> boost::serialization::make_nvp("value",t);
    ot = t;
}

} // detail

template<class Archive, class T>
void save(
    Archive & ar,
    const boost::optional< T > & ot,
    const unsigned int /*version*/
){
    detail::save_impl(ar, ot);
}

#ifndef BOOST_NO_CXX17_HDR_OPTIONAL
template<class Archive, class T>
void save(
    Archive & ar,
    const std::optional< T > & ot,
    const unsigned int /*version*/
){
    detail::save_impl(ar, ot);
}
#endif

template<class Archive, class T>
void load(
    Archive & ar,
    boost::optional< T > & ot,
    const unsigned int version
){
    detail::load_impl(ar, ot, version);
}

#ifndef BOOST_NO_CXX17_HDR_OPTIONAL
template<class Archive, class T>
void load(
    Archive & ar,
    std::optional< T >  & ot,
    const unsigned int version
){
    detail::load_impl(ar, ot, version);
}
#endif

template<class Archive, class T>
void serialize(
    Archive & ar,
    boost::optional< T > & ot,
    const unsigned int version
){
    boost::serialization::split_free(ar, ot, version);
}

#ifndef BOOST_NO_CXX17_HDR_OPTIONAL
template<class Archive, class T>
void serialize(
    Archive & ar,
    std::optional< T > & ot,
    const unsigned int version
){
    boost::serialization::split_free(ar, ot, version);
}
#endif

template<class T>
struct version<boost::optional<T> >{
    BOOST_STATIC_CONSTANT(int, value = 1);
};

#ifndef BOOST_NO_CXX17_HDR_OPTIONAL
template<class T>
struct version<std::optional<T> >{
    BOOST_STATIC_CONSTANT(int, value = 1);
};
#endif

} // serialization
} // boost

#endif // BOOST_SERIALIZATION_OPTIONAL_HPP
