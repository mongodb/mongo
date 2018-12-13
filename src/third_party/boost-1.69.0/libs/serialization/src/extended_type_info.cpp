/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// extended_type_info.cpp: implementation for portable version of type_info

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#if (defined _MSC_VER) && (_MSC_VER == 1200)
#  pragma warning (disable : 4786) // too long name, harmless warning
#endif

#include <algorithm>
#include <set>
#include <utility>
#include <boost/assert.hpp>
#include <cstddef> // NULL

#include <cstring>
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{ using ::strcmp; }
#endif

#include <boost/config.hpp> // msvc needs this to suppress warning

#include <boost/core/no_exceptions_support.hpp>

// it marks our code with proper attributes as being exported when
// we're compiling it while marking it import when just the headers
// is being included.
#define BOOST_SERIALIZATION_SOURCE
#include <boost/serialization/config.hpp>
#include <boost/serialization/singleton.hpp>
#include <boost/serialization/force_include.hpp>
#include <boost/serialization/extended_type_info.hpp>

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4511 4512)
#endif

namespace boost { 
namespace serialization {
namespace detail {

struct key_compare
{
    bool
    operator()(
        const extended_type_info * lhs, 
        const extended_type_info * rhs
    ) const {
        // performance shortcut
        if(lhs == rhs)
            return false;
        const char * l = lhs->get_key();
        BOOST_ASSERT(NULL != l);
        const char * r = rhs->get_key();
        BOOST_ASSERT(NULL != r);
        // performance shortcut
        // shortcut to exploit string pooling
        if(l == r)
            return false;
        // for exported types, use the string key so that
        // multiple instances in different translation units
        // can be matched up
        return std::strcmp(l, r) < 0;
    }
};

typedef std::multiset<const extended_type_info *, key_compare> ktmap;

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4511 4512)
#endif

class extended_type_info_arg : public extended_type_info
{
    virtual bool
    is_less_than(const extended_type_info & /*rhs*/) const {
        BOOST_ASSERT(false);
        return false;
    };
    virtual bool
    is_equal(const extended_type_info & /*rhs*/) const {
        BOOST_ASSERT(false);
        return false;
    };
    virtual const char * get_debug_info() const {
        return get_key();
    }
    virtual void * construct(unsigned int /*count*/, ...) const{
        BOOST_ASSERT(false);
        return NULL;
    }
    virtual void destroy(void const * const /*p*/) const {
        BOOST_ASSERT(false);
    }
public:
    extended_type_info_arg(const char * key) :
        extended_type_info(0, key)
    {}

    ~extended_type_info_arg(){
    }
};

#ifdef BOOST_MSVC
#  pragma warning(pop)
#endif

} // namespace detail

BOOST_SERIALIZATION_DECL void  
extended_type_info::key_register() const{
    if(NULL == get_key())
        return;
    singleton<detail::ktmap>::get_mutable_instance().insert(this);
}

BOOST_SERIALIZATION_DECL void  
extended_type_info::key_unregister() const{
    if(NULL == get_key())
        return;
    // note: it's been discovered that at least one platform is not guaranteed
    // to destroy singletons reverse order of construction.  So we can't
    // use a runtime assert here.  Leave this in a reminder not to do this!
    // BOOST_ASSERT(! singleton<detail::ktmap>::is_destroyed());
    if(! singleton<detail::ktmap>::is_destroyed()){
        detail::ktmap & x = singleton<detail::ktmap>::get_mutable_instance();
        detail::ktmap::iterator start = x.lower_bound(this);
        detail::ktmap::iterator end = x.upper_bound(this);
        // remove entry in map which corresponds to this type
        for(;start != end; ++start){
            if(this == *start){
                x.erase(start);
                break;
            }
        }
    }
}

BOOST_SERIALIZATION_DECL const extended_type_info *
extended_type_info::find(const char *key) {
    BOOST_ASSERT(NULL != key);
    const detail::ktmap & k = singleton<detail::ktmap>::get_const_instance();
    const detail::extended_type_info_arg eti_key(key);
    const detail::ktmap::const_iterator it = k.find(& eti_key);
    if(k.end() == it)
        return NULL;
    return *(it);
}

BOOST_SERIALIZATION_DECL
extended_type_info::extended_type_info(
    const unsigned int type_info_key,
    const char * key
) :
    m_type_info_key(type_info_key),
    m_key(key)
{
}

BOOST_SERIALIZATION_DECL 
extended_type_info::~extended_type_info(){
}

BOOST_SERIALIZATION_DECL bool
extended_type_info::operator<(const extended_type_info &rhs) const {
    // short cut for a common cases
    if(this == & rhs)
        return false;
    if(m_type_info_key == rhs.m_type_info_key){
        return is_less_than(rhs);
    }
    if(m_type_info_key < rhs.m_type_info_key)
        return true;
    return false;
}

BOOST_SERIALIZATION_DECL bool
extended_type_info::operator==(const extended_type_info &rhs) const {
    // short cut for a common cases
    if(this == & rhs)
        return true;
    if(m_type_info_key != rhs.m_type_info_key){
        return false;
    }
    return is_equal(rhs);
}

} // namespace serialization
} // namespace boost
