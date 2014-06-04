// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// shared_ptr_helper.hpp: serialization for boost shared pointern

// (C) Copyright 2004-2009 Robert Ramey, Martin Ecker and Takatoshi Kondo
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <map>
#include <list>
#include <utility>
#include <cstddef> // NULL

#define BOOST_ARCHIVE_SOURCE
// include this to prevent linker errors when the
// same modules are marked export and import.
#define BOOST_SERIALIZATION_SOURCE

#include <boost/serialization/throw_exception.hpp>
#include <boost/serialization/void_cast.hpp>
#include <boost/serialization/extended_type_info.hpp>
#include <boost/archive/shared_ptr_helper.hpp>
#include <boost/archive/archive_exception.hpp>

namespace boost {
namespace archive{
namespace detail {

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// a common class for holding various types of shared pointers

// returns pointer to object and an indicator whether this is a
// new entry (true) or a previous one (false)
BOOST_ARCHIVE_DECL(shared_ptr<void>)
shared_ptr_helper::get_od(
        const void * t,
        const boost::serialization::extended_type_info * true_type, 
        const boost::serialization::extended_type_info * this_type
){
    // get void pointer to the most derived type
    // this uniquely identifies the object referred to
    const void * od = void_downcast(
        *true_type, 
        *this_type, 
        t
    );
    if(NULL == od)
        boost::serialization::throw_exception(
            archive_exception(
                archive_exception::unregistered_cast,
                true_type->get_debug_info(),
                this_type->get_debug_info()
            )
        );

    // make tracking array if necessary
    if(NULL == m_pointers)
        m_pointers = new collection_type;

    //shared_ptr<const void> sp(od, null_deleter()); 
    shared_ptr<const void> sp(od, null_deleter());
    collection_type::iterator i = m_pointers->find(sp);

    if(i == m_pointers->end()){
        shared_ptr<void> np;
        return np;
    }
    od = void_upcast(
        *true_type, 
        *this_type,
        i->get()
    );
    if(NULL == od)
        boost::serialization::throw_exception(
            archive_exception(
                archive_exception::unregistered_cast,
                true_type->get_debug_info(),
                this_type->get_debug_info()
            )
        );

    return shared_ptr<void>(
        const_pointer_cast<void>(*i), 
        const_cast<void *>(od)
    );
}

BOOST_ARCHIVE_DECL(void)
shared_ptr_helper::append(const boost::shared_ptr<const void> &sp){
    // make tracking array if necessary
    if(NULL == m_pointers)
        m_pointers = new collection_type;

    collection_type::iterator i = m_pointers->find(sp);

    if(i == m_pointers->end()){
        std::pair<collection_type::iterator, bool> result;
        result = m_pointers->insert(sp);
        BOOST_ASSERT(result.second);
    }
}

//  #ifdef BOOST_SERIALIZATION_SHARED_PTR_132_HPP
BOOST_ARCHIVE_DECL(void)
shared_ptr_helper::append(const boost_132::shared_ptr<const void> & t){
    if(NULL == m_pointers_132)
        m_pointers_132 = new std::list<boost_132::shared_ptr<const void> >;
    m_pointers_132->push_back(t);
}
//  #endif
BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY())
shared_ptr_helper::shared_ptr_helper() : 
    m_pointers(NULL)
    #ifdef BOOST_SERIALIZATION_SHARED_PTR_132_HPP
        , m_pointers_132(NULL)
    #endif
{}
BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY())
shared_ptr_helper::~shared_ptr_helper(){
    if(NULL != m_pointers)
        delete m_pointers;
    #ifdef BOOST_SERIALIZATION_SHARED_PTR_132_HPP
    if(NULL != m_pointers_132)
        delete m_pointers_132;
    #endif
}

} // namespace detail
} // namespace serialization
} // namespace boost

