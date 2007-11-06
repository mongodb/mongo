#ifndef BOOST_SERIALIZATION_SHARED_PTR_HPP
#define BOOST_SERIALIZATION_SHARED_PTR_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// shared_ptr.hpp: serialization for boost shared pointer

// (C) Copyright 2004 Robert Ramey and Martin Ecker
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <map>

#include <boost/config.hpp>
#include <boost/mpl/integral_c.hpp>
#include <boost/mpl/integral_c_tag.hpp>

#include <boost/detail/workaround.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/throw_exception.hpp>

#include <boost/archive/archive_exception.hpp>

#include <boost/serialization/type_info_implementation.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/version.hpp>
#include <boost/serialization/tracking.hpp>
#include <boost/static_assert.hpp>

#include <boost/serialization/void_cast_fwd.hpp>

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// shared_ptr serialization traits
// version 1 to distinguish from boost 1.32 version. Note: we can only do this
// for a template when the compiler supports partial template specialization

#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
    namespace boost {
    namespace serialization{
        template<class T>
        struct version< ::boost::shared_ptr<T> > {
            typedef mpl::integral_c_tag tag;
#if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3206))
            typedef BOOST_DEDUCED_TYPENAME mpl::int_<1> type;
#else
            typedef mpl::int_<1> type;
#endif
#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x570))
            BOOST_STATIC_CONSTANT(unsigned int, value = 1);
#else
            BOOST_STATIC_CONSTANT(unsigned int, value = type::value);
#endif
        };
        // don't track shared pointers
        template<class T>
        struct tracking_level< ::boost::shared_ptr<T> > { 
            typedef mpl::integral_c_tag tag;
#if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3206))
            typedef BOOST_DEDUCED_TYPENAME mpl::int_< ::boost::serialization::track_never> type;
#else
            typedef mpl::int_< ::boost::serialization::track_never> type;
#endif
#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x570))
            BOOST_STATIC_CONSTANT(int, value = ::boost::serialization::track_never);
#else
            BOOST_STATIC_CONSTANT(int, value = type::value);
#endif
        };
    }}
    #define BOOST_SERIALIZATION_SHARED_PTR(T)
#else
    // define macro to let users of these compilers do this
    #define BOOST_SERIALIZATION_SHARED_PTR(T)                         \
    BOOST_CLASS_VERSION(                                              \
        ::boost::shared_ptr< T >,                                     \
        1                                                             \
    )                                                                 \
    BOOST_CLASS_TRACKING(                                             \
        ::boost::shared_ptr< T >,                                     \
        ::boost::serialization::track_never                           \
    )                                                                 \
    /**/
#endif

namespace boost {
namespace serialization{

class extended_type_info;

namespace detail {

struct null_deleter {
    void operator()(void const *) const {}
};

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// a common class for holding various types of shared pointers

class shared_ptr_helper {
    typedef std::map<void*, shared_ptr<void> > collection_type;
    typedef collection_type::const_iterator iterator_type;
    // list of shared_pointers create accessable by raw pointer. This
    // is used to "match up" shared pointers loaded at diferent
    // points in the archive
    collection_type m_pointers;
    // return a void pointer to the most derived type
    template<class T>
    void * object_identifier(T * t) const {
        const extended_type_info * true_type 
            = type_info_implementation<T>::type::get_derived_extended_type_info(*t);
        // note:if this exception is thrown, be sure that derived pointer
        // is either regsitered or exported.
        if(NULL == true_type)
            boost::throw_exception(
                boost::archive::archive_exception(
                    boost::archive::archive_exception::unregistered_class
                )
            );
        const boost::serialization::extended_type_info * this_type
            = boost::serialization::type_info_implementation<T>::type::get_instance();
        void * vp = void_downcast(*true_type, *this_type, t);
        return vp;
    }
public:
    template<class T>
    void reset(shared_ptr<T> & s, T * r){
        if(NULL == r){
            s.reset();
            return;
        }
        // get pointer to the most derived object.  This is effectively
        // the object identifer
        void * od = object_identifier(r);

        iterator_type it = m_pointers.find(od);

        if(it == m_pointers.end()){
            s.reset(r);
            m_pointers.insert(collection_type::value_type(od,s));
        }
        else{
            s = static_pointer_cast<T>((*it).second);
        }
    }
    virtual ~shared_ptr_helper(){}
};

} // namespace detail

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// utility function for creating/getting a helper - could be useful in general
// but shared_ptr is the only class (so far that needs it) and I don't have a
// convenient header to place it into.
template<class Archive, class H>
H &
get_helper(Archive & ar){
    extended_type_info * eti = type_info_implementation<H>::type::get_instance();
    shared_ptr<void> sph;
    ar.lookup_helper(eti, sph);
    if(NULL == sph.get()){
        sph = shared_ptr<H>(new H);
        ar.insert_helper(eti, sph);
    }
    return * static_cast<H *>(sph.get());
}

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// serialization for shared_ptr

template<class Archive, class T>
inline void save(
    Archive & ar,
    const boost::shared_ptr<T> &t,
    const unsigned int /* file_version */
){
    // The most common cause of trapping here would be serializing
    // something like shared_ptr<int>.  This occurs because int
    // is never tracked by default.  Wrap int in a trackable type
    BOOST_STATIC_ASSERT((tracking_level<T>::value != track_never));
    const T * t_ptr = t.get();
    ar << boost::serialization::make_nvp("px", t_ptr);
}

template<class Archive, class T>
inline void load(
    Archive & ar,
    boost::shared_ptr<T> &t,
    const unsigned int file_version
){
    // The most common cause of trapping here would be serializing
    // something like shared_ptr<int>.  This occurs because int
    // is never tracked by default.  Wrap int in a trackable type
    BOOST_STATIC_ASSERT((tracking_level<T>::value != track_never));
    T* r;
    #ifdef BOOST_SERIALIZATION_SHARED_PTR_132_HPP
    if(file_version < 1){
        ar.register_type(static_cast<
            boost_132::detail::sp_counted_base_impl<T *, boost::checked_deleter<T> > *
        >(NULL));
        boost_132::shared_ptr<T> sp;
        ar >> boost::serialization::make_nvp("px", sp.px);
        ar >> boost::serialization::make_nvp("pn", sp.pn);
        // got to keep the sps around so the sp.pns don't disappear
        get_helper<Archive, boost_132::serialization::detail::shared_ptr_helper>(ar).append(sp);
        r = sp.get();
    }
    else    
    #endif
    {
        ar >> boost::serialization::make_nvp("px", r);
    }
    get_helper<Archive, detail::shared_ptr_helper >(ar).reset(t,r);
}

template<class Archive, class T>
inline void serialize(
    Archive & ar,
    boost::shared_ptr<T> &t,
    const unsigned int file_version
){
    // correct shared_ptr serialization depends upon object tracking
    // being used.
    BOOST_STATIC_ASSERT(
        boost::serialization::tracking_level<T>::value
        != boost::serialization::track_never
    );
    boost::serialization::split_free(ar, t, file_version);
}

} // namespace serialization
} // namespace boost

#endif // BOOST_SERIALIZATION_SHARED_PTR_HPP
