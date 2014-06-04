/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_oarchive.cpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <boost/config.hpp> // msvc 6.0 needs this for warning suppression

#include <boost/assert.hpp>
#include <set>
#include <cstddef> // NULL

#include <boost/limits.hpp>
#include <boost/serialization/state_saver.hpp>
#include <boost/serialization/throw_exception.hpp>

// including this here to work around an ICC in intel 7.0
// normally this would be part of basic_oarchive.hpp below.
#define BOOST_ARCHIVE_SOURCE
// include this to prevent linker errors when the
// same modules are marked export and import.
#define BOOST_SERIALIZATION_SOURCE

#include <boost/archive/detail/decl.hpp>
#include <boost/archive/basic_archive.hpp>
#include <boost/archive/detail/basic_oserializer.hpp>
#include <boost/archive/detail/basic_pointer_oserializer.hpp>
#include <boost/archive/detail/basic_oarchive.hpp>
#include <boost/archive/archive_exception.hpp>
#include <boost/serialization/extended_type_info.hpp>

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4251 4231 4660 4275)
#endif

using namespace boost::serialization;

namespace boost {
namespace archive {
namespace detail {

class basic_oarchive_impl {
    friend class basic_oarchive;
    unsigned int m_flags;

    //////////////////////////////////////////////////////////////////////
    // information about each serialized object saved
    // keyed on address, class_id
    struct aobject
    {
        const void * address;
        class_id_type class_id;
        object_id_type object_id;

        bool operator<(const aobject &rhs) const
        {
            BOOST_ASSERT(NULL != address);
            BOOST_ASSERT(NULL != rhs.address);
            if( address < rhs.address )
                return true;
            if( address > rhs.address )
                return false;
            return class_id < rhs.class_id;
        }
        aobject & operator=(const aobject & rhs)
        {
            address = rhs.address;
            class_id = rhs.class_id;
            object_id = rhs.object_id;
            return *this;
        }
        aobject(
            const void *a,
            class_id_type class_id_,
            object_id_type object_id_
        ) :
            address(a),
            class_id(class_id_),
            object_id(object_id_)
        {}
        aobject() : address(NULL){}
    };
    // keyed on class_id, address
    typedef std::set<aobject> object_set_type;
    object_set_type object_set;

    //////////////////////////////////////////////////////////////////////
    // information about each serialized class saved
    // keyed on type_info
    struct cobject_type
    {
        const basic_oserializer * m_bos_ptr;
        const class_id_type m_class_id;
        bool m_initialized;
        cobject_type(
            std::size_t class_id,
            const basic_oserializer & bos
        ) :
            m_bos_ptr(& bos),
            m_class_id(class_id),
            m_initialized(false)
        {}
        cobject_type(const basic_oserializer & bos)
            : m_bos_ptr(& bos)
        {}
        cobject_type(
            const cobject_type & rhs
        ) :
            m_bos_ptr(rhs.m_bos_ptr),
            m_class_id(rhs.m_class_id),
            m_initialized(rhs.m_initialized)
        {}
        // the following cannot be defined because of the const
        // member.  This will generate a link error if an attempt
        // is made to assign.  This should never be necessary
        // use this only for lookup argument 
        cobject_type & operator=(const cobject_type &rhs);
        bool operator<(const cobject_type &rhs) const {
            return *m_bos_ptr < *(rhs.m_bos_ptr);
        }
    };
    // keyed on type_info
    typedef std::set<cobject_type> cobject_info_set_type;
    cobject_info_set_type cobject_info_set;

    // list of objects initially stored as pointers - used to detect errors
    // keyed on object id
    std::set<object_id_type> stored_pointers;

    // address of the most recent object serialized as a poiner
    // whose data itself is now pending serialization
    const void * pending_object;
    const basic_oserializer * pending_bos;

    basic_oarchive_impl(unsigned int flags) :
        m_flags(flags),
        pending_object(NULL),
        pending_bos(NULL)
    {}

    const cobject_type &
    find(const basic_oserializer & bos);
    const basic_oserializer *  
    find(const serialization::extended_type_info &ti) const;

//public:
    const cobject_type &
    register_type(const basic_oserializer & bos);
    void save_object(
        basic_oarchive & ar,
        const void *t,
        const basic_oserializer & bos
    );
    void save_pointer(
        basic_oarchive & ar,
        const void * t, 
        const basic_pointer_oserializer * bpos
    );
};

//////////////////////////////////////////////////////////////////////
// basic_oarchive implementation functions

// given a type_info - find its bos
// return NULL if not found
inline const basic_oserializer *
basic_oarchive_impl::find(const serialization::extended_type_info & ti) const {
    #ifdef BOOST_MSVC
    #  pragma warning(push)
    #  pragma warning(disable : 4511 4512)
    #endif
    class bosarg : 
        public basic_oserializer
    {
        bool class_info() const {
            BOOST_ASSERT(false); 
            return false;
        }
        // returns true if objects should be tracked
        bool tracking(const unsigned int) const {
            BOOST_ASSERT(false);
            return false;
        }
        // returns class version
        version_type version() const {
            BOOST_ASSERT(false);
            return version_type(0);
        }
        // returns true if this class is polymorphic
        bool is_polymorphic() const{
            BOOST_ASSERT(false);
            return false;
        }
        void save_object_data(      
            basic_oarchive & /*ar*/, const void * /*x*/
        ) const {
            BOOST_ASSERT(false);
        }
    public:
        bosarg(const serialization::extended_type_info & eti) :
          boost::archive::detail::basic_oserializer(eti)
        {}
    };
    #ifdef BOOST_MSVC
    #pragma warning(pop)
    #endif
    bosarg bos(ti);
    cobject_info_set_type::const_iterator cit 
        = cobject_info_set.find(cobject_type(bos));
    // it should already have been "registered" - see below
    if(cit == cobject_info_set.end()){
        // if an entry is not found in the table it is because a pointer
        // of a derived class has been serialized through its base class
        // but the derived class hasn't been "registered" 
        return NULL;
    }
    // return pointer to the real class
    return cit->m_bos_ptr;
}

inline const basic_oarchive_impl::cobject_type &
basic_oarchive_impl::find(const basic_oserializer & bos)
{
    std::pair<cobject_info_set_type::iterator, bool> cresult = 
        cobject_info_set.insert(cobject_type(cobject_info_set.size(), bos));
    return *(cresult.first);
}

inline const basic_oarchive_impl::cobject_type &
basic_oarchive_impl::register_type(
    const basic_oserializer & bos
){
    cobject_type co(cobject_info_set.size(), bos);
    std::pair<cobject_info_set_type::const_iterator, bool>
        result = cobject_info_set.insert(co);
    return *(result.first);
}

inline void
basic_oarchive_impl::save_object(
    basic_oarchive & ar,
    const void *t,
    const basic_oserializer & bos
){
    // if its been serialized through a pointer and the preamble's been done
    if(t == pending_object && pending_bos == & bos){
        // just save the object data
        ar.end_preamble();
        (bos.save_object_data)(ar, t);
        return;
    }

    // get class information for this object
    const cobject_type & co = register_type(bos);
    if(bos.class_info()){
        if( ! co.m_initialized){
            ar.vsave(class_id_optional_type(co.m_class_id));
            ar.vsave(tracking_type(bos.tracking(m_flags)));
            ar.vsave(version_type(bos.version()));
            (const_cast<cobject_type &>(co)).m_initialized = true;
        }
    }

    // we're not tracking this type of object
    if(! bos.tracking(m_flags)){
        // just windup the preamble - no object id to write
        ar.end_preamble();
        // and save the data
        (bos.save_object_data)(ar, t);
        return;
    }

    // look for an existing object id
    object_id_type oid(object_set.size());
    // lookup to see if this object has already been written to the archive
    basic_oarchive_impl::aobject ao(t, co.m_class_id, oid);
    std::pair<basic_oarchive_impl::object_set_type::const_iterator, bool>
        aresult = object_set.insert(ao);
    oid = aresult.first->object_id;

    // if its a new object
    if(aresult.second){
        // write out the object id
        ar.vsave(oid);
        ar.end_preamble();
        // and data
        (bos.save_object_data)(ar, t);
        return;
    }

    // check that it wasn't originally stored through a pointer
    if(stored_pointers.end() != stored_pointers.find(oid)){
        // this has to be a user error.  loading such an archive
        // would create duplicate objects
        boost::serialization::throw_exception(
            archive_exception(archive_exception::pointer_conflict)
        );
    }
    // just save the object id
    ar.vsave(object_reference_type(oid));
    ar.end_preamble();
    return;
}

// save a pointer to an object instance
inline void
basic_oarchive_impl::save_pointer(
    basic_oarchive & ar,
    const void * t, 
    const basic_pointer_oserializer * bpos_ptr
){
    const basic_oserializer & bos = bpos_ptr->get_basic_serializer();
    std::size_t original_count = cobject_info_set.size();
    const cobject_type & co = register_type(bos);
    if(! co.m_initialized){
        ar.vsave(co.m_class_id);
        // if its a previously unregistered class 
        if((cobject_info_set.size() > original_count)){
            if(bos.is_polymorphic()){
                const serialization::extended_type_info *eti = & bos.get_eti();
                const char * key = NULL;
                if(NULL != eti)
                    key = eti->get_key();
                if(NULL != key){
                    // the following is required by IBM C++ compiler which
                    // makes a copy when passing a non-const to a const.  This
                    // is permitted by the standard but rarely seen in practice
                    const class_name_type cn(key);
                    // write out the external class identifier
                    ar.vsave(cn);
                }
                else
                    // without an external class name
                    // we won't be able to de-serialize it so bail now
                    boost::serialization::throw_exception(
                        archive_exception(archive_exception::unregistered_class)
                    );
            }
        }
        if(bos.class_info()){
            ar.vsave(tracking_type(bos.tracking(m_flags)));
            ar.vsave(version_type(bos.version()));
        }
        (const_cast<cobject_type &>(co)).m_initialized = true;
    }
    else{
        ar.vsave(class_id_reference_type(co.m_class_id));
    }

    // if we're not tracking
    if(! bos.tracking(m_flags)){
        // just save the data itself
        ar.end_preamble();
        serialization::state_saver<const void *> x(pending_object);
        serialization::state_saver<const basic_oserializer *> y(pending_bos);
        pending_object = t;
        pending_bos = & bpos_ptr->get_basic_serializer();
        bpos_ptr->save_object_ptr(ar, t);
        return;
    }

    object_id_type oid(object_set.size());
    // lookup to see if this object has already been written to the archive
    basic_oarchive_impl::aobject ao(t, co.m_class_id, oid);
    std::pair<basic_oarchive_impl::object_set_type::const_iterator, bool>
        aresult = object_set.insert(ao);
    oid = aresult.first->object_id;
    // if the saved object already exists
    if(! aresult.second){
        // append the object id to he preamble
        ar.vsave(object_reference_type(oid));
        // and windup.
        ar.end_preamble();
        return;
    }

    // append id of this object to preamble
    ar.vsave(oid);
    ar.end_preamble();

    // and save the object itself
    serialization::state_saver<const void *> x(pending_object);
    serialization::state_saver<const basic_oserializer *> y(pending_bos);
    pending_object = t;
    pending_bos = & bpos_ptr->get_basic_serializer();
    bpos_ptr->save_object_ptr(ar, t);
    // add to the set of object initially stored through pointers
    stored_pointers.insert(oid);
}

} // namespace detail
} // namespace archive
} // namespace boost

//////////////////////////////////////////////////////////////////////
// implementation of basic_oarchive functions

namespace boost {
namespace archive {
namespace detail {

BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) 
basic_oarchive::basic_oarchive(unsigned int flags)
    : pimpl(new basic_oarchive_impl(flags))
{}

BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) 
basic_oarchive::~basic_oarchive()
{
    delete pimpl;
}

BOOST_ARCHIVE_DECL(void) 
basic_oarchive::save_object(
    const void *x, 
    const basic_oserializer & bos
){
    pimpl->save_object(*this, x, bos);
}

BOOST_ARCHIVE_DECL(void) 
basic_oarchive::save_pointer(
    const void * t, 
    const basic_pointer_oserializer * bpos_ptr
){
    pimpl->save_pointer(*this, t, bpos_ptr);
}

BOOST_ARCHIVE_DECL(void) 
basic_oarchive::register_basic_serializer(const basic_oserializer & bos){
    pimpl->register_type(bos);
}

BOOST_ARCHIVE_DECL(library_version_type)
basic_oarchive::get_library_version() const{
    return BOOST_ARCHIVE_VERSION();
}

BOOST_ARCHIVE_DECL(unsigned int)
basic_oarchive::get_flags() const{
    return pimpl->m_flags;
}

BOOST_ARCHIVE_DECL(void) 
basic_oarchive::end_preamble(){
}

} // namespace detail
} // namespace archive
} // namespace boost

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif
