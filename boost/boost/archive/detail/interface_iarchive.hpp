#ifndef BOOST_ARCHIVE_DETAIL_INTERFACE_IARCHIVE_HPP
#define BOOST_ARCHIVE_DETAIL_INTERFACE_IARCHIVE_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// interface_iarchive.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.
#include <string>
#include <boost/cstdint.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/archive/detail/auto_link_archive.hpp>
#include <boost/archive/detail/iserializer.hpp>
#include <boost/archive/detail/abi_prefix.hpp> // must be the last header

namespace boost {
template<class T>
class shared_ptr;
namespace serialization {
    class extended_type_info;
} // namespace serialization
namespace archive {
namespace detail {

class BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) basic_pointer_iserializer;

template<class Archive>
class interface_iarchive 
{
protected:
    interface_iarchive(){};
public:
    /////////////////////////////////////////////////////////
    // archive public interface
    typedef mpl::bool_<true> is_loading;
    typedef mpl::bool_<false> is_saving;

    // return a pointer to the most derived class
    Archive * This(){
        return static_cast<Archive *>(this);
    }

    template<class T>
    const basic_pointer_iserializer * register_type(T * = NULL){
        const basic_pointer_iserializer & bpis =
            instantiate_pointer_iserializer(
                static_cast<Archive *>(NULL),
                static_cast<T *>(NULL)
            );
        this->This()->register_basic_serializer(bpis.get_basic_serializer());
        return & bpis;
    }
    void lookup_helper(
        const boost::serialization::extended_type_info * const eti,
        boost::shared_ptr<void> & sph
    ){
        this->This()->lookup_basic_helper(eti, sph);
    }

    void insert_helper(
        const boost::serialization::extended_type_info * const eti,
        shared_ptr<void> & sph
    ){
        this->This()->insert_basic_helper(eti, sph);
    }
    template<class T>
    Archive & operator>>(T & t){
        this->This()->load_override(t, 0);
        return * this->This();
    }

    // the & operator 
    template<class T>
    Archive & operator&(T & t){
        return *(this->This()) >> t;
    }
};

} // namespace detail
} // namespace archive
} // namespace boost

#include <boost/archive/detail/abi_suffix.hpp> // pops abi_suffix.hpp pragmas

#endif // BOOST_ARCHIVE_DETAIL_INTERFACE_IARCHIVE_HPP
