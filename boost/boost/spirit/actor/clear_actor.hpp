/*=============================================================================
    Copyright (c) 2003 Jonathan de Halleux (dehalleux@pelikhan.com)
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#ifndef BOOST_SPIRIT_ACTOR_CLEAR_ACTOR_HPP
#define BOOST_SPIRIT_ACTOR_CLEAR_ACTOR_HPP

#include <boost/spirit/actor/ref_actor.hpp>

namespace boost { namespace spirit {

    ///////////////////////////////////////////////////////////////////////////
    //  Summary:
    //  A semantic action policy that calls clear method.
    //  (This doc uses convention available in actors.hpp)
    //
    //  Actions (what it does):
    //      ref.clear();
    //
    //  Policy name:
    //      clear_action
    //
    //  Policy holder, corresponding helper method:
    //      ref_actor, clear_a( ref );
    //
    //  () operators: both.
    //
    //  See also ref_actor for more details.
    ///////////////////////////////////////////////////////////////////////////
    struct clear_action
    {
        template<
            typename T
        >
        void act(T& ref_) const
        {
            ref_.clear();
        }
    };

    ///////////////////////////////////////////////////////////////////////////
    // helper method that creates a and_assign_actor.
    ///////////////////////////////////////////////////////////////////////////
    template<typename T>
    inline ref_actor<T,clear_action> clear_a(T& ref_)
    {
        return ref_actor<T,clear_action>(ref_);
    }


}}

#endif

