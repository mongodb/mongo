#ifndef BOOST_STATECHART_IN_STATE_REACTION_HPP_INCLUDED
#define BOOST_STATECHART_IN_STATE_REACTION_HPP_INCLUDED
//////////////////////////////////////////////////////////////////////////////
// Copyright 2005-2006 Andreas Huber Doenni
// Distributed under the Boost Software License, Version 1.0. (See accompany-
// ing file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//////////////////////////////////////////////////////////////////////////////



#include <boost/statechart/result.hpp>

#include <boost/mpl/if.hpp>

#include <boost/cast.hpp> // boost::polymorphic_downcast
#include <boost/type_traits/is_same.hpp>



namespace boost
{
namespace statechart
{



class event_base;

//////////////////////////////////////////////////////////////////////////////
template< class Event, 
          class ReactionContext,
          void ( ReactionContext::*pAction )( const Event & ) >
class in_state_reaction
{
  private:
    //////////////////////////////////////////////////////////////////////////
    struct react_base
    {
      template< class State, class EventBase, class IdType >
      static detail::reaction_result react(
        State & stt, const EventBase & evt, const IdType & )
      {
        ( stt.template context< ReactionContext >().*pAction )( evt );
        return detail::do_discard_event;
      }
    };

    struct react_derived
    {
      template< class State, class EventBase, class IdType >
      static detail::reaction_result react(
        State & stt, const EventBase & evt, const IdType & eventType )
      {
        if ( eventType == Event::static_type() )
        {
          ( stt.template context< ReactionContext >().*pAction )(
            *polymorphic_downcast< const Event * >( &evt ) );
          return detail::do_discard_event;
        }
        else
        {
          return detail::no_reaction;
        }
      }
    };

  public:
    //////////////////////////////////////////////////////////////////////////
    // The following declarations should be private.
    // They are only public because many compilers lack template friends.
    //////////////////////////////////////////////////////////////////////////
    template< class State, class EventBase, class IdType >
    static detail::reaction_result react(
      State & stt, const EventBase & evt, const IdType & eventType )
    {
      typedef typename mpl::if_<
        is_same< Event, event_base >, react_base, react_derived
      >::type impl;

      return impl::react( stt, evt, eventType );
    }
};



} // namespace statechart
} // namespace boost



#endif
