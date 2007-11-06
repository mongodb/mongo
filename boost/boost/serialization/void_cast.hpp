#ifndef  BOOST_SERIALIZATION_VOID_CAST_HPP
#define BOOST_SERIALIZATION_VOID_CAST_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// void_cast.hpp:   interface for run-time casting of void pointers.

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// gennadiy.rozental@tfn.com

//  See http://www.boost.org for updates, documentation, and revision history.

#include <boost/smart_cast.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/identity.hpp>

#include <boost/serialization/config.hpp>
#include <boost/serialization/force_include.hpp>
#include <boost/serialization/type_info_implementation.hpp>

#include <boost/config/abi_prefix.hpp> // must be the last header

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4251 4231 4660 4275)
#endif

namespace boost { 
namespace serialization { 

class BOOST_SERIALIZATION_DECL(BOOST_PP_EMPTY()) extended_type_info;

// Given a void *, assume that it really points to an instance of one type
// and alter it so that it would point to an instance of a related type.
// Return the altered pointer. If there exists no sequence of casts that
// can transform from_type to to_type, return a NULL.  

BOOST_SERIALIZATION_DECL(void const *)
void_upcast(
    extended_type_info const & derived_type,  
    extended_type_info const & base_type, 
    void const * const t,
    bool top = true
);

inline void *
void_upcast(
    extended_type_info const & derived_type_,
    extended_type_info const & base_type_,
    void * const t 
){
    return const_cast<void*>(void_upcast(
        derived_type_, 
        base_type_, 
        const_cast<void const *>(t)
    ));
}

BOOST_SERIALIZATION_DECL(void const *)
void_downcast(
    extended_type_info const & derived_type,  
    extended_type_info const & base_type, 
    void const * const t,
    bool top = true
);

inline void *
void_downcast(
    extended_type_info const & derived_type_,
    extended_type_info const & base_type_,
    void * const t 
){
    return const_cast<void*>(void_downcast(
        derived_type_, 
        base_type_, 
        const_cast<void const *>(t)
    ));
}

namespace void_cast_detail {

// note: can't be abstract because an instance is used as a search argument
class BOOST_SERIALIZATION_DECL(BOOST_PP_EMPTY()) void_caster
{
    friend struct void_caster_compare ;
    friend 
    BOOST_SERIALIZATION_DECL(void const *)  
    boost::serialization::void_upcast(
        const extended_type_info & derived_type,
        const extended_type_info & base_type,
        const void * t,
        bool top
    );
    friend 
    BOOST_SERIALIZATION_DECL(void const *)  
    boost::serialization::void_downcast(
        const extended_type_info & derived_type,
        const extended_type_info & base_type,
        const void * t,
        bool top
    );
    // each derived class must re-implement these;
    virtual void const * upcast(void const * t) const = 0;
    virtual void const * downcast(void const * t) const = 0;
    // Data members
    extended_type_info const & m_derived_type;
    extended_type_info const & m_base_type;
protected:
    static void static_register(const void_caster *);
public:
    // Constructor
    void_caster(
        extended_type_info const & derived_type_,
        extended_type_info const & base_type_ 
    );
    // predicate used to determine if this void caster includes
    // a particular eti *
    bool includes(const extended_type_info * eti) const;
    virtual ~void_caster();
private:
    // cw 8.3 requires this!!
    void_caster& operator=(void_caster const&);
};

template <class Derived, class Base>
class void_caster_primitive : 
    public void_caster
{
    virtual void const* downcast( void const * t ) const {
        Derived * d = boost::smart_cast<const Derived *, const Base *>(
            static_cast<const Base *>(t)
        );
        return d;
    }
    virtual void const* upcast(void const * t) const {
        Base * b = boost::smart_cast<const Base *, const Derived *>(
            static_cast<const Derived *>(t)
        );
        return b;
    }

public:
    static const void_caster_primitive instance;
    void_caster_primitive() BOOST_USED;
};

template <class Derived, class Base>
void_caster_primitive<Derived, Base>::void_caster_primitive() :
    void_caster( 
        * type_info_implementation<Derived>::type::get_instance(), 
        * type_info_implementation<Base>::type::get_instance() 
    )
{
    this->static_register(& instance);
}

// the purpose of this class is to create to->from and from->to instances
// of void_caster_primitive for each related pair of types.  This has to be
// done a pre-execution time - hence the usage of static variable.
template<class Derived, class Base>
const void_caster_primitive<Derived, Base>
    void_caster_primitive<Derived, Base>::instance;

} // void_cast_detail 

// Register a base/derived pair.  This indicates that it is possible
// to upcast a void pointer from Derived to Base and downcast a
// void pointer from Base to Derived.  Note bogus arguments to workaround
// bug in msvc 6.0
template<class Derived, class Base>
BOOST_DLLEXPORT 
inline const void_cast_detail::void_caster & void_cast_register(
    const Derived * dnull, 
    const Base * bnull
) BOOST_USED;
template<class Derived, class Base>
BOOST_DLLEXPORT 
inline const void_cast_detail::void_caster & void_cast_register(
    const Derived * /* dnull = NULL */, 
    const Base * /* bnull = NULL */
){
    return void_cast_detail::void_caster_primitive<
        const Derived, 
        const Base
    >::instance;
}

} // namespace serialization
} // namespace boost

#include <boost/config/abi_suffix.hpp> // pops abi_suffix.hpp pragmas

#endif // BOOST_SERIALIZATION_VOID_CAST_HPP
