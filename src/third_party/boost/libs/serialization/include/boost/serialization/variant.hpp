#ifndef BOOST_SERIALIZATION_VARIANT_HPP
#define BOOST_SERIALIZATION_VARIANT_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// variant.hpp - non-intrusive serialization of variant types
//
// copyright (c) 2005
// troy d. straszheim <troy@resophonic.com>
// http://www.resophonic.com
//
// copyright (c) 2019 Samuel Debionne, ESRF
//
// copyright (c) 2023
// Robert Ramey <ramey@rrsd.com>
// http://www.rrsd.com
//
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for updates, documentation, and revision history.
//
// thanks to Robert Ramey, Peter Dimov, and Richard Crossley.
//

#include <boost/mpl/front.hpp>
#include <boost/mpl/pop_front.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/mpl/size.hpp>
#include <boost/mpl/empty.hpp>

#include <boost/serialization/throw_exception.hpp>

// Boost Variant supports all C++ versions back to C++03
#include <boost/variant/variant.hpp>
#include <boost/variant/get.hpp>

// Boost Variant2 supports all C++ versions back to C++11
#if BOOST_CXX_VERSION >= 201103L
#include <boost/variant2/variant.hpp>
#include <type_traits>
#endif

// Boost Variant2 supports all C++ versions back to C++11
#ifndef BOOST_NO_CXX17_HDR_VARIANT
#include <variant>
//#include <type_traits>
#endif

#include <boost/archive/archive_exception.hpp>

#include <boost/serialization/split_free.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/nvp.hpp>

// use visitor from boost::variant
template<class Visitor, BOOST_VARIANT_ENUM_PARAMS(class T)>
typename Visitor::result_type visit(
    Visitor visitor,
    const boost::variant<BOOST_VARIANT_ENUM_PARAMS(T)> & t
){
    return boost::apply_visitor(visitor, t);
}
template<class Visitor, BOOST_VARIANT_ENUM_PARAMS(class T)>
typename Visitor::result_type visit(
    Visitor visitor,
    const boost::variant<BOOST_VARIANT_ENUM_PARAMS(T)> & t,
    const boost::variant<BOOST_VARIANT_ENUM_PARAMS(T)> & u
){
    return boost::apply_visitor(visitor, t, u);
}

namespace boost {
namespace serialization {

template<class Archive>
struct variant_save_visitor :
    boost::static_visitor<void>
{
    variant_save_visitor(Archive& ar) :
        m_ar(ar)
    {}
    template<class T>
    void operator()(T const & value) const {
        m_ar << BOOST_SERIALIZATION_NVP(value);
    }
private:
    Archive & m_ar;
};

template<class Archive, BOOST_VARIANT_ENUM_PARAMS(/* typename */ class T)>
void save(
    Archive & ar,
    boost::variant<BOOST_VARIANT_ENUM_PARAMS(T)> const & v,
    unsigned int /*version*/
){
    int which = v.which();
    ar << BOOST_SERIALIZATION_NVP(which);
    variant_save_visitor<Archive> visitor(ar);
    visit(visitor, v);
}

#if BOOST_CXX_VERSION >= 201103L
template<class Archive, class ...Types>
void save(
    Archive & ar,
    boost::variant2::variant<Types...> const & v,
    unsigned int /*version*/
){
    int which = v.index();
    ar << BOOST_SERIALIZATION_NVP(which);
    const variant_save_visitor<Archive> visitor(ar);
    visit(visitor, v);
}
#endif

#ifndef BOOST_NO_CXX17_HDR_VARIANT
template<class Archive, class ...Types>
void save(
    Archive & ar,
    std::variant<Types...> const & v,
    unsigned int /*version*/
){
    int which = v.index();
    ar << BOOST_SERIALIZATION_NVP(which);
    const variant_save_visitor<Archive> visitor(ar);
    visit(visitor, v);
}
#endif

template<class S>
struct variant_impl {

    struct load_null {
        template<class Archive, class V>
        static void invoke(
            Archive & /*ar*/,
            std::size_t /*which*/,
            V & /*v*/,
            const unsigned int /*version*/
        ){}
    };

    struct load_member {
        template<class Archive, class V>
        static void invoke(
            Archive & ar,
            std::size_t which,
            V & v,
            const unsigned int version
        ){
            if(which == 0){
                // note: A non-intrusive implementation (such as this one)
                // necessary has to copy the value.  This wouldn't be necessary
                // with an implementation that de-serialized to the address of the
                // aligned storage included in the variant.
                typedef typename mpl::front<S>::type head_type;
                head_type value;
                ar >> BOOST_SERIALIZATION_NVP(value);
                v = std::move(value);;
                head_type * new_address = & get<head_type>(v);
                ar.reset_object_address(new_address, & value);
                return;
            }
            typedef typename mpl::pop_front<S>::type type;
            variant_impl<type>::load_impl(ar, which - 1, v, version);
        }
    };

    template<class Archive, class V>
    static void load_impl(
        Archive & ar,
        std::size_t which,
        V & v,
        const unsigned int version
    ){
        typedef typename mpl::eval_if<mpl::empty<S>,
            mpl::identity<load_null>,
            mpl::identity<load_member>
        >::type typex;
        typex::invoke(ar, which, v, version);
    }
}; // variant_impl

template<class Archive, BOOST_VARIANT_ENUM_PARAMS(/* typename */ class T)>
void load(
    Archive & ar,
    boost::variant<BOOST_VARIANT_ENUM_PARAMS(T)>& v,
    const unsigned int version
){
    int which;
    typedef typename boost::variant<BOOST_VARIANT_ENUM_PARAMS(T)>::types types;
    ar >> BOOST_SERIALIZATION_NVP(which);
    if(which >=  mpl::size<types>::value){
        // this might happen if a type was removed from the list of variant types
        boost::serialization::throw_exception(
            boost::archive::archive_exception(
                boost::archive::archive_exception::unsupported_version
            )
        );
    }
    variant_impl<types>::load_impl(ar, which, v, version);
}

#if BOOST_CXX_VERSION >= 201103L
template<class Archive, class ... Types>
void load(
    Archive & ar,
    boost::variant2::variant<Types...> & v,
    const unsigned int version
){
    int which;
    typedef typename boost::variant<Types...>::types types;
    ar >> BOOST_SERIALIZATION_NVP(which);
    if(which >=  sizeof...(Types)){
        // this might happen if a type was removed from the list of variant types
        boost::serialization::throw_exception(
            boost::archive::archive_exception(
                boost::archive::archive_exception::unsupported_version
            )
        );
    }
    variant_impl<types>::load_impl(ar, which, v, version);
}
#endif

#ifndef BOOST_NO_CXX17_HDR_VARIANT
template<class Archive, class ... Types>
void load(
    Archive & ar,
    std::variant<Types...> & v,
    const unsigned int version
){
    int which;
    typedef typename boost::variant<Types...>::types types;
    ar >> BOOST_SERIALIZATION_NVP(which);
    if(which >=  sizeof...(Types)){
        // this might happen if a type was removed from the list of variant types
        boost::serialization::throw_exception(
            boost::archive::archive_exception(
                boost::archive::archive_exception::unsupported_version
            )
        );
    }
    variant_impl<types>::load_impl(ar, which, v, version);
}
#endif

template<class Archive,BOOST_VARIANT_ENUM_PARAMS(/* typename */ class T)>
inline void serialize(
    Archive & ar,
    boost::variant<BOOST_VARIANT_ENUM_PARAMS(T)> & v,
    const unsigned int file_version
){
    boost::serialization::split_free(ar,v,file_version);
}

#if BOOST_CXX_VERSION >= 201103L
template<class Archive, class ... Types>
inline void serialize(
    Archive & ar,
    boost::variant2::variant<Types...> & v,
    const unsigned int file_version
){
    boost::serialization::split_free(ar,v,file_version);
}
#endif

#ifndef BOOST_NO_CXX17_HDR_VARIANT
template<class Archive, class ... Types>
inline void serialize(
    Archive & ar,
    std::variant<Types...> & v,
    const unsigned int file_version
){
    boost::serialization::split_free(ar,v,file_version);
}
#endif

} // namespace serialization
} // namespace boost

#include <boost/serialization/tracking.hpp>

namespace boost {
namespace serialization {

template<BOOST_VARIANT_ENUM_PARAMS(/* typename */ class T)>
struct tracking_level<
    variant<BOOST_VARIANT_ENUM_PARAMS(T)>
>{
    typedef mpl::integral_c_tag tag;
    typedef mpl::int_< ::boost::serialization::track_always> type;
    BOOST_STATIC_CONSTANT(int, value = type::value);
};

#ifndef BOOST_NO_CXX17_HDR_VARIANT
template<class... Types>
struct tracking_level<
    std::variant<Types...>
>{
    typedef mpl::integral_c_tag tag;
    typedef mpl::int_< ::boost::serialization::track_always> type;
    BOOST_STATIC_CONSTANT(int, value = type::value);
};
#endif

} // namespace serialization
} // namespace boost

#endif //BOOST_SERIALIZATION_VARIANT_HPP
