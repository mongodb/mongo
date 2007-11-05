/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt
    Copyright (c) 2005 Eric Niebler

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_CONS_HPP)
#define FUSION_SEQUENCE_CONS_HPP

#include <boost/call_traits.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/spirit/fusion/iterator/cons_iterator.hpp>
#include <boost/spirit/fusion/sequence/detail/cons_begin_end_traits.hpp>
#include <boost/spirit/fusion/sequence/as_fusion_sequence.hpp>

namespace boost { namespace fusion
{
    struct void_t;

    struct cons_tag;

    struct nil : sequence_base<nil>
    {
        typedef cons_tag tag;
        typedef void_t car_type;
        typedef void_t cdr_type;
    };

    template <typename Car, typename Cdr = nil>
    struct cons : sequence_base<cons<Car,Cdr> >
    {
        typedef cons_tag tag;
        typedef typename call_traits<Car>::value_type car_type;
        typedef Cdr cdr_type;

        cons()
          : car(), cdr() {}

        explicit cons(
            typename call_traits<Car>::param_type car_
          , typename call_traits<Cdr>::param_type cdr_ = Cdr())
          : car(car_), cdr(cdr_) {}

        car_type car;
        cdr_type cdr;
    };

    template <typename Car>
    inline cons<Car>
    make_cons(Car const& car)
    {
        return cons<Car>(car);
    }

    template <typename Car, typename Cdr>
    inline cons<Car, Cdr>
    make_cons(Car const& car, Cdr const& cdr)
    {
        return cons<Car, Cdr>(car, cdr);
    }
}}

#endif

