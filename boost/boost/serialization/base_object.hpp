#ifndef BOOST_SERIALIZATION_BASE_OBJECT_HPP
#define BOOST_SERIALIZATION_BASE_OBJECT_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// base_object.hpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// if no archive headers have been included this is a no op
// this is to permit BOOST_EXPORT etc to be included in a 
// file declaration header

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/int.hpp>
#include <boost/type_traits/is_base_and_derived.hpp>
#include <boost/type_traits/is_pointer.hpp>
#include <boost/type_traits/is_const.hpp>

#include <boost/static_assert.hpp>
#include <boost/serialization/type_info_implementation.hpp>
#include <boost/serialization/force_include.hpp>
#include <boost/serialization/void_cast_fwd.hpp>

namespace boost {
namespace serialization {

namespace detail {
    // metrowerks CodeWarrior
    #if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3206)) 
        // only register void casts if the types are polymorphic
        template<class Base, class Derived>
        struct base_register{
            struct nothing {
                static const void_cast_detail::void_caster & invoke(){
                    return static_cast<const void_cast_detail::void_caster &>(
                        * static_cast<const void_cast_detail::void_caster *>(NULL)
                    );
                }
            };

            // hold a reference to the void_cast_register and void_caster in the hope of 
            // ensuring code instantiation for some compilers with over-zealous link time 
            // optimiser. The compiler that demanded this was CW
            struct reg{
                typedef const void_cast_detail::void_caster & (* t_vcr)(
                    const Derived *,
                    const Base *
                );
                t_vcr m_vcr;
                static const void_cast_detail::void_caster & invoke(){
                    return  void_cast_register<const Derived, const Base>(
                        static_cast<const Derived *>(NULL),
                        static_cast<const Base *>(NULL)
                    );
                }
                reg() :
                    m_vcr(static_cast<t_vcr>(void_cast_register))
                {
                }
            } m_reg;

            static const void_cast_detail::void_caster & invoke(){
                typedef BOOST_DEDUCED_TYPENAME mpl::eval_if<
                    BOOST_DEDUCED_TYPENAME type_info_implementation<Base>::type::is_polymorphic,
                    mpl::identity<reg>,
                    mpl::identity<nothing>
                >::type typex;
                return typex::invoke();
            }

            const void_cast_detail::void_caster & m_vc;
            Derived & m_d;

            base_register(Derived & d) :
                m_vc(invoke()),
                m_d(d)
            {}
            Base & get_base() const {
                return m_d;
            }
        };
    #else
        // only register void casts if the types are polymorphic
        template<class Base, class Derived>
        struct base_register{
            struct nothing {
                static void invoke(){}
            };
            struct reg{
                static void invoke(){
                    void_cast_register<const Derived, const Base>(
                        static_cast<const Derived *>(NULL),
                        static_cast<const Base *>(NULL)
                    );
                }
            };
            static void invoke(){
                typedef BOOST_DEDUCED_TYPENAME mpl::eval_if<
                    BOOST_DEDUCED_TYPENAME type_info_implementation<Base>::type::is_polymorphic,
                    mpl::identity<reg>,
                    mpl::identity<nothing>
                >::type typex;
                typex::invoke();
            }
        };
    #endif
    // get the base type for a given derived type
    // preserving the const-ness
    template<class B, class D>
    struct base_cast
    {
        typedef BOOST_DEDUCED_TYPENAME
            mpl::if_<
                is_const<D>,
                const B,
                B
        >::type type;
        BOOST_STATIC_ASSERT(is_const<type>::value == is_const<D>::value);
    };
} // namespace detail

// metrowerks CodeWarrior
#if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3206)) 
template<class Base, class Derived>
BOOST_DEDUCED_TYPENAME detail::base_cast<Base, Derived>::type & 
base_object(Derived &d)
{
    BOOST_STATIC_ASSERT(( is_base_and_derived<Base,Derived>::value));
    BOOST_STATIC_ASSERT(! is_pointer<Derived>::value);
    typedef BOOST_DEDUCED_TYPENAME detail::base_cast<Base, Derived>::type type;
    return detail::base_register<type, Derived>(d).get_base();
}
// BORLAND
#elif BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x560))
template<class Base, class Derived>
const Base & 
base_object(const Derived & d)
{
    BOOST_STATIC_ASSERT(! is_pointer<Derived>::value);
    detail::base_register<Base, Derived>::invoke();
    return static_cast<const Base &>(d);
}
#else
template<class Base, class Derived>
BOOST_DEDUCED_TYPENAME detail::base_cast<Base, Derived>::type & 
base_object(Derived &d)
{
    BOOST_STATIC_ASSERT(( is_base_and_derived<Base,Derived>::value));
    BOOST_STATIC_ASSERT(! is_pointer<Derived>::value);
    typedef BOOST_DEDUCED_TYPENAME detail::base_cast<Base, Derived>::type type;
    detail::base_register<type, Derived>::invoke();
    return static_cast<type &>(d);
}
#endif

} // namespace serialization
} // namespace boost

#endif // BOOST_SERIALIZATION_BASE_OBJECT_HPP
