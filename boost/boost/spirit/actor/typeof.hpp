/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_ACTOR_TYPEOF_HPP)
#define BOOST_SPIRIT_ACTOR_TYPEOF_HPP

#include <boost/typeof/typeof.hpp>

namespace boost { namespace spirit {

    template<typename T, typename ActionT> class ref_actor;

    template<typename T, typename ActionT> class ref_value_actor;

    template<typename T, typename ValueT, typename ActionT> 

    class ref_const_ref_actor;
    template<typename T, typename ValueT, typename ActionT> 

    class ref_const_ref_value_actor;
    template<typename T, typename Value1T, typename Value2T, typename ActionT> 

    class ref_const_ref_const_ref_actor;

    struct assign_action; 
    struct clear_action;
    struct increment_action;
    struct decrement_action;
    struct push_back_action;
    struct push_front_action;
    struct insert_key_action;
    struct insert_at_action;
    struct assign_key_action;
    
    template<typename T> class swap_actor;

}} // namespace boost::spirit


#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()


BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ref_actor,2)
#if !defined(BOOST_SPIRIT_CORE_TYPEOF_HPP)
// this part also lives in the core master header and is deprecated there...
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ref_value_actor,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ref_const_ref_actor,3)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::assign_action)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::push_back_action)
#endif
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ref_const_ref_value_actor,3)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ref_const_ref_const_ref_actor,4)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::clear_action)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::increment_action)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::decrement_action)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::push_front_action)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::insert_key_action)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::insert_at_action)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::assign_key_action)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::swap_actor,1)

#endif

