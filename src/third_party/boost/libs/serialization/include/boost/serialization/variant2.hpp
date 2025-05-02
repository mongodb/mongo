#ifndef BOOST_SERIALIZATION_VARIANT2_HPP
#define BOOST_SERIALIZATION_VARIANT2_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// variant.hpp - non-intrusive serialization of variant types
//
// copyright (c) 2019 Samuel Debionne, ESRF
//
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for updates, documentation, and revision history.
//
// Widely inspired form boost::variant serialization
//

#include <boost/serialization/throw_exception.hpp>

#include <variant>

#include <boost/archive/archive_exception.hpp>

#include <boost/mp11/list.hpp>

#include <boost/serialization/split_free.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/nvp.hpp>

namespace boost {
namespace serialization {

template<class Archive>
struct variant2_save_visitor
{
    variant2_save_visitor(Archive& ar) :
        m_ar(ar)
    {}
    template<class T>
    void operator()(T const & value) const
    {
        m_ar << BOOST_SERIALIZATION_NVP(value);
    }
private:
    Archive & m_ar;
};


template<class Archive>
struct variant2_load_visitor
{
    variant2_load_visitor(Archive& ar) :
        m_ar(ar)
    {}
    template<class T>
    void operator()(T & value) const
    {
        m_ar >> BOOST_SERIALIZATION_NVP(value);
    }
private:
    Archive & m_ar;
};

template<class Archive, class ...Types>
void save(
    Archive & ar,
    variant2::variant<Types...> const & v,
    unsigned int /*version*/
){
    const std::size_t which = v.index();
    ar << BOOST_SERIALIZATION_NVP(which);
    variant2_save_visitor<Archive> visitor(ar);
    std::visit(visitor, v);
}

template<class Seq>
struct variant_impl
{
    template<class Archive, class V>
    static void load (
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
            using type = mp11::mp_front<Seq>;
            type value;
            ar >> BOOST_SERIALIZATION_NVP(value);
            v = std::move(value);
            type * new_address = & variant2::get<type>(v);
            ar.reset_object_address(new_address, & value);
            return;
        }
        //typedef typename mpl::pop_front<S>::type type;
        using types = mp11::mp_pop_front<Seq>;
        variant_impl<types>::load(ar, which - 1, v, version);
    }
};

template<class Seq>
struct variant_impl<Seq>
{
    template<class Archive, class V>
    static void load (
        Archive & /*ar*/,
        std::size_t /*which*/,
        V & /*v*/,
        const unsigned int /*version*/
    ){}
};

template<class Archive, class... Types>
void load(
    Archive & ar, 
    variant2::variant<Types...> & v,
    const unsigned int version
){
    std::size_t which;
    ar >> BOOST_SERIALIZATION_NVP(which);
    if(which >=  sizeof...(Types))
        // this might happen if a type was removed from the list of variant types
        boost::serialization::throw_exception(
            boost::archive::archive_exception(
                boost::archive::archive_exception::unsupported_version
            )
        );
    variant_impl<sizeof...(Types), mp11::mp_list<Types...>>::load(ar, which, v, version);
}

template<class Archive,class... Types>
inline void serialize(
    Archive & ar,
    variant2::variant<Types...> & v,
    const unsigned int file_version
){
    split_free(ar,v,file_version);
}

// Specialization for std::monostate
template<class Archive>
void serialize(Archive &ar, variant2::monostate &, const unsigned int /*version*/)
{}

} // namespace serialization
} // namespace boost

//template<typename T0_, BOOST_VARIANT_ENUM_SHIFTED_PARAMS(typename T)>

#include <boost/serialization/tracking.hpp>

namespace boost {
    namespace serialization {
        
template<class... Types>
struct tracking_level<
    variant2::variant<Types...>
>{
    typedef mpl::integral_c_tag tag;
    typedef mpl::int_< ::boost::serialization::track_always> type;
    BOOST_STATIC_CONSTANT(int, value = type::value);
};

} // namespace serialization
} // namespace boost

#endif //BOOST_SERIALIZATION_VARIANT2_HPP
