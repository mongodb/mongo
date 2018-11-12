/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// void_cast.cpp: implementation of run-time casting of void pointers

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// <gennadiy.rozental@tfn.com>

//  See http://www.boost.org for updates, documentation, and revision history.

#if (defined _MSC_VER) && (_MSC_VER == 1200)
# pragma warning (disable : 4786) // too long name, harmless warning
#endif

// STL
#include <set>
#include <functional>
#include <algorithm>
#include <cstddef> // NULL
#ifdef BOOST_SERIALIZATION_LOG
#include <iostream>
#endif

// BOOST
#include <boost/config.hpp>
#include <boost/assert.hpp>

#define BOOST_SERIALIZATION_SOURCE
#include <boost/serialization/config.hpp>
// it marks our code with proper attributes as being exported when
// we're compiling it while marking it import when just the headers
// is being included.
#include <boost/serialization/singleton.hpp>
#include <boost/serialization/extended_type_info.hpp>
#include <boost/serialization/void_cast.hpp>

namespace boost { 
namespace serialization {
namespace void_cast_detail {

// note that void_casters are keyed on value of
// member extended type info records - NOT their
// addresses.  This is necessary in order for the
// void cast operations to work across dll and exe
// module boundries.
bool void_caster::operator<(const void_caster & rhs) const {
    // include short cut to save time and eliminate
    // problems when when base class aren't virtual
    if(m_derived != rhs.m_derived){
        if(*m_derived < *rhs.m_derived)
            return true;
        if(*rhs.m_derived < *m_derived)
            return false;
    }
    // m_derived == rhs.m_derived
    if(m_base != rhs.m_base)
        return *m_base < *rhs.m_base;
    else
        return false;
}

struct void_caster_compare {
    bool operator()(const void_caster * lhs, const void_caster * rhs) const {
        return *lhs < *rhs;
    }
};

typedef std::set<const void_caster *, void_caster_compare> set_type;
typedef boost::serialization::singleton<set_type> void_caster_registry;

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4511 4512)
#endif

// implementation of shortcut void caster
class void_caster_shortcut : public void_caster
{
    bool m_includes_virtual_base;

    void const * 
    vbc_upcast(
        void const * const t
    ) const;
    void const *
    vbc_downcast(
        void const * const t
    ) const;
    virtual void const *
    upcast(void const * const t) const{
        if(m_includes_virtual_base)
            return vbc_upcast(t);
        return static_cast<const char *> ( t ) - m_difference;
    }
    virtual void const *
    downcast(void const * const t) const{
        if(m_includes_virtual_base)
            return vbc_downcast(t);
        return static_cast<const char *> ( t ) + m_difference;
    }
    virtual bool is_shortcut() const {
        return true;
    }
    virtual bool has_virtual_base() const {
        return m_includes_virtual_base;
    }
public:
    void_caster_shortcut(
        extended_type_info const * derived,
        extended_type_info const * base,
        std::ptrdiff_t difference,
        bool includes_virtual_base,
        void_caster const * const parent
    ) :
        void_caster(derived, base, difference, parent),
        m_includes_virtual_base(includes_virtual_base)
    {
        recursive_register(includes_virtual_base);
    }
    virtual ~void_caster_shortcut(){
        recursive_unregister();
    }
};

#ifdef BOOST_MSVC
#  pragma warning(pop)
#endif

void const * 
void_caster_shortcut::vbc_downcast(
    void const * const t
) const {
    // try to find a chain that gives us what we want
    const void_cast_detail::set_type & s
        = void_cast_detail::void_caster_registry::get_const_instance();
    void_cast_detail::set_type::const_iterator it;
    for(it = s.begin(); it != s.end(); ++it){
        // if the current candidate casts to the desired target type
        if ((*it)->m_derived == m_derived){
            // and if it's not us
            if ((*it)->m_base != m_base){
                // try to cast from the candidate base to our base
                const void * t_new;
                t_new = void_downcast(*(*it)->m_base, *m_base, t);
                // if we were successful
                if(NULL != t_new){
                    // recast to our derived
                    const void_caster * vc = *it;
                    return vc->downcast(t_new);
                }
            }
        }
    }
    return NULL;
}

void const * 
void_caster_shortcut::vbc_upcast(
    void const * const t
) const {
    // try to find a chain that gives us what we want
    const void_cast_detail::set_type & s
        = void_cast_detail::void_caster_registry::get_const_instance();
    void_cast_detail::set_type::const_iterator it;
    for(it = s.begin(); it != s.end(); ++it){
        // if the current candidate casts from the desired base type
        if((*it)->m_base == m_base){
            // and if it's not us
            if ((*it)->m_derived != m_derived){
                // try to cast from the candidate derived to our our derived
                const void * t_new;
                t_new = void_upcast(*m_derived, *(*it)->m_derived, t);
                if(NULL != t_new)
                    return (*it)->upcast(t_new);
            }
        }
    }
    return NULL;
}

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4511 4512)
#endif

// just used as a search key
class void_caster_argument : public void_caster
{
    virtual void const *
    upcast(void const * const /*t*/) const {
        BOOST_ASSERT(false);
        return NULL;
    }
    virtual void const *
    downcast( void const * const /*t*/) const {
        BOOST_ASSERT(false);
        return NULL;
    }
    virtual bool has_virtual_base() const {
        BOOST_ASSERT(false);
        return false;
    }
public:
    void_caster_argument(
        extended_type_info const * derived,
        extended_type_info const * base
    ) :
        void_caster(derived, base)
    {}
    virtual ~void_caster_argument(){};
};

#ifdef BOOST_MSVC
#  pragma warning(pop)
#endif

// implementation of void caster base class
BOOST_SERIALIZATION_DECL void
void_caster::recursive_register(bool includes_virtual_base) const {
    void_cast_detail::set_type & s
        = void_cast_detail::void_caster_registry::get_mutable_instance();

    #ifdef BOOST_SERIALIZATION_LOG
    std::clog << "recursive_register\n";
    std::clog << m_derived->get_debug_info();
    std::clog << "<-";
    std::clog << m_base->get_debug_info();
    std::clog << "\n";
    #endif

    std::pair<void_cast_detail::set_type::const_iterator, bool> result;
    // comment this out for now.  
    result = s.insert(this);
    //assert(result.second);

    // generate all implied void_casts.
    void_cast_detail::set_type::const_iterator it;
    for(it = s.begin(); it != s.end(); ++it){
        if(* m_derived == * (*it)->m_base){
            const void_caster_argument vca(
                (*it)->m_derived, 
                m_base
            );
            void_cast_detail::set_type::const_iterator i;
            i = s.find(& vca);
            if(i == s.end()){
                new void_caster_shortcut(
                    (*it)->m_derived, 
                    m_base,
                    m_difference + (*it)->m_difference,
                    (*it)->has_virtual_base() || includes_virtual_base,
                    this
                );
            }
        }
        if(* (*it)->m_derived == * m_base){
            const void_caster_argument vca(
                m_derived, 
                (*it)->m_base
            );
            void_cast_detail::set_type::const_iterator i;
            i = s.find(& vca);
            if(i == s.end()){
                new void_caster_shortcut(
                    m_derived, 
                    (*it)->m_base, 
                    m_difference + (*it)->m_difference,
                    (*it)->has_virtual_base() || includes_virtual_base,
                    this
                );
            }
        }
    }
}

BOOST_SERIALIZATION_DECL void
void_caster::recursive_unregister() const {
    BOOST_ASSERT(! void_caster_registry::is_destroyed());
    if(void_caster_registry::is_destroyed())
        return;

    #ifdef BOOST_SERIALIZATION_LOG
    std::clog << "recursive_unregister\n";
    std::clog << m_derived->get_debug_info();
    std::clog << "<-";
    std::clog << m_base->get_debug_info();
    std::clog << "\n";
    #endif

    void_cast_detail::set_type & s 
        = void_caster_registry::get_mutable_instance();

    // delete all shortcuts which use this primitive
    void_cast_detail::set_type::iterator it;
    for(it = s.begin(); it != s.end();){
        const void_caster * vc = *it;
        if(vc == this){
            s.erase(it++);
        }
        else
        if(vc->m_parent == this){
            s.erase(it);
            delete vc;
            it = s.begin();
        }
        else
            it++;
    }
}

} // namespace void_cast_detail

BOOST_SYMBOL_VISIBLE void const *
void_upcast(
    extended_type_info const & derived,
    extended_type_info const & base,
    void const * const t
);

// Given a void *, assume that it really points to an instance of one type
// and alter it so that it would point to an instance of a related type.
// Return the altered pointer. If there exists no sequence of casts that
// can transform from_type to to_type, return a NULL.  
BOOST_SERIALIZATION_DECL void const *
void_upcast(
    extended_type_info const & derived,
    extended_type_info const & base,
    void const * const t
){
    // same types - trivial case
    if (derived == base)
        return t;

    // check to see if base/derived pair is found in the registry
    const void_cast_detail::set_type & s
        = void_cast_detail::void_caster_registry::get_const_instance();
    const void_cast_detail::void_caster_argument ca(& derived, & base);

    void_cast_detail::set_type::const_iterator it;
    it = s.find(& ca);
    if (s.end() != it)
        return (*it)->upcast(t);

    return NULL;
}

BOOST_SYMBOL_VISIBLE void const *
void_downcast(
    extended_type_info const & derived,
    extended_type_info const & base,
    void const * const t
);

BOOST_SERIALIZATION_DECL void const *
void_downcast(
    extended_type_info const & derived,
    extended_type_info const & base,
    void const * const t
){
    // same types - trivial case
    if (derived == base)
        return t;

    // check to see if base/derived pair is found in the registry
    const void_cast_detail::set_type & s
        = void_cast_detail::void_caster_registry::get_const_instance();
    const void_cast_detail::void_caster_argument ca(& derived, & base);

    void_cast_detail::set_type::const_iterator it;
    it = s.find(&ca);
    if (s.end() != it)
        return(*it)->downcast(t);

    return NULL;
}

} // namespace serialization
} // namespace boost
