#ifndef BOOST_STATECHART_TRANSITION_HPP_INCLUDED
#define BOOST_STATECHART_TRANSITION_HPP_INCLUDED
//////////////////////////////////////////////////////////////////////////////
// Copyright 2002-2006 Andreas Huber Doenni
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
namespace detail
{



//////////////////////////////////////////////////////////////////////////////
template< class Event >
struct no_context
{
  void no_function( const Event & );
};



} // namespace detail



class event_base;

//////////////////////////////////////////////////////////////////////////////
template< class Event, class Destination,
          class TransitionContext = detail::no_context< Event >,
          void ( TransitionContext::*pTransitionAction )( const Event & ) =
            &detail::no_context< Event >::no_function >
class transition
{
  private:
    //////////////////////////////////////////////////////////////////////////
    struct react_without_transition_action_impl
    {
      template< class State, class EventBase >
      static detail::reaction_result react( State & stt, const EventBase & )
      {
        return detail::result_utility::get_result(
          stt.template transit< Destination >() );
      }
    };

    struct react_base_with_transition_action_impl
    {
      template< class State, class EventBase >
      static detail::reaction_result react(
        State & stt, const EventBase & toEvent )
      {
        return detail::result_utility::get_result(
          stt.template transit< Destination >( pTransitionAction, toEvent ) );
      }
    };

    struct react_base
    {
      template< class State, class EventBase, class IdType >
      static detail::reaction_result react(
        State & stt, const EventBase & evt, const IdType & )
      {
        typedef typename mpl::if_<
          is_same< TransitionContext, detail::no_context< Event > >,
          react_without_transition_action_impl,
          react_base_with_transition_action_impl
        >::type impl;
        return impl::react( stt, evt );
      }
    };

    struct react_derived_with_transition_action_impl
    {
      template< class State, class EventBase >
      static detail::reaction_result react(
        State & stt, const EventBase & toEvent )
      {
        return detail::result_utility::get_result(
          stt.template transit< Destination >(
            pTransitionAction,
            *polymorphic_downcast< const Event * >( &toEvent ) ) );
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
          typedef typename mpl::if_<
            is_same< TransitionContext, detail::no_context< Event > >,
            react_without_transition_action_impl,
            react_derived_with_transition_action_impl
          >::type impl;
          return impl::react( stt, evt );
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
