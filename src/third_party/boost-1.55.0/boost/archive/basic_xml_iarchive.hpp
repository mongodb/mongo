#ifndef BOOST_ARCHIVE_BASIC_XML_IARCHIVE_HPP
#define BOOST_ARCHIVE_BASIC_XML_IARCHIVE_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_xml_iarchive.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com .
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <boost/config.hpp>
#include <boost/serialization/pfto.hpp>
#include <boost/detail/workaround.hpp>

#include <boost/archive/detail/common_iarchive.hpp>

#include <boost/serialization/nvp.hpp>
#include <boost/serialization/string.hpp>

#include <boost/mpl/assert.hpp>

#include <boost/archive/detail/abi_prefix.hpp> // must be the last header

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4511 4512)
#endif

namespace boost {
namespace archive {

/////////////////////////////////////////////////////////////////////////
// class xml_iarchive - read serialized objects from a input text stream
template<class Archive>
class basic_xml_iarchive :
    public detail::common_iarchive<Archive>
{
protected:
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
public:
#elif defined(BOOST_MSVC)
    // for some inexplicable reason insertion of "class" generates compile erro
    // on msvc 7.1
    friend detail::interface_oarchive<Archive>;
#else
    friend class detail::interface_oarchive<Archive>;
#endif
    unsigned int depth;
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load_start(const char *name);
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load_end(const char *name);

    // Anything not an attribute and not a name-value pair is an
    // should be trapped here.
    template<class T>
    void load_override(T & t,  BOOST_PFTO int)
    {
        // If your program fails to compile here, its most likely due to
        // not specifying an nvp wrapper around the variable to
        // be serialized.
        BOOST_MPL_ASSERT((serialization::is_wrapper< T >));
        this->detail_common_iarchive::load_override(t, 0);
    }

    // Anything not an attribute - see below - should be a name value
    // pair and be processed here
    typedef detail::common_iarchive<Archive> detail_common_iarchive;
    template<class T>
    void load_override(
        #ifndef BOOST_NO_FUNCTION_TEMPLATE_ORDERING
        const
        #endif
        boost::serialization::nvp< T > & t,
        int
    ){
        this->This()->load_start(t.name());
        this->detail_common_iarchive::load_override(t.value(), 0);
        this->This()->load_end(t.name());
    }

    // specific overrides for attributes - handle as
    // primitives. These are not name-value pairs
    // so they have to be intercepted here and passed on to load.
    // although the class_id is included in the xml text file in order
    // to make the file self describing, it isn't used when loading
    // an xml archive.  So we can skip it here.  Note: we MUST override
    // it otherwise it will be loaded as a normal primitive w/o tag and
    // leaving the archive in an undetermined state
    void load_override(class_id_optional_type & /* t */, int){}
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load_override(object_id_type & t, int);
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load_override(version_type & t, int);
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load_override(class_id_type & t, int);
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load_override(tracking_type & t, int);
    // class_name_type can't be handled here as it depends upon the
    // char type used by the stream.  So require the derived implementation
    // handle this.
    // void load_override(class_name_type & t, int);

    BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
    basic_xml_iarchive(unsigned int flags);
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
    ~basic_xml_iarchive();
};

} // namespace archive
} // namespace boost

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

#include <boost/archive/detail/abi_suffix.hpp> // pops abi_suffix.hpp pragmas

#endif // BOOST_ARCHIVE_BASIC_XML_IARCHIVE_HPP
