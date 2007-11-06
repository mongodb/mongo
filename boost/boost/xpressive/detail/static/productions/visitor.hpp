///////////////////////////////////////////////////////////////////////////////
// visitor.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_VISITOR_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_VISITOR_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/mpl/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/regex_impl.hpp>
#include <boost/xpressive/detail/static/productions/transmogrify.hpp>
#include <boost/xpressive/detail/core/matcher/mark_begin_matcher.hpp>

namespace boost { namespace xpressive { namespace detail
{
    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename BidiIter>
    struct xpression_visitor_base
    {
        explicit xpression_visitor_base(shared_ptr<regex_impl<BidiIter> > const &self)
          : impl_()
          , self_(self)
        {
        }

        void swap(xpression_visitor_base &that)
        {
            this->impl_.swap(that.impl_);
            this->self_.swap(that.self_);
        }

        int get_hidden_mark()
        {
            return -(int)(++this->impl_.hidden_mark_count_);
        }

        void mark_number(int mark_number)
        {
            if(0 < mark_number)
            {
                this->impl_.mark_count_ =
                    (std::max)(this->impl_.mark_count_, (std::size_t)mark_number);
            }
        }

        shared_ptr<regex_impl<BidiIter> > &self()
        {
            return this->self_;
        }

        regex_impl<BidiIter> &impl()
        {
            return this->impl_;
        }

    protected:

        template<typename Matcher>
        void visit_(Matcher const &)
        {
            // no-op
        }

        template<bool ByRef>
        void visit_(regex_placeholder<BidiIter, ByRef> const &rex)
        {
            // when visiting an embedded regex, track the references
            this->impl_.track_reference(rex.impl_);
        }

        void visit_(mark_placeholder const &backref)
        {
            // keep track of the largest mark number found
            this->mark_number(backref.mark_number_);
        }

        void visit_(mark_begin_matcher const &mark_begin)
        {
            // keep track of the largest mark number found
            this->mark_number(mark_begin.mark_number_);
        }

    private:

        regex_impl<BidiIter> impl_;
        shared_ptr<regex_impl<BidiIter> > self_;
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename BidiIter, typename ICase, typename Traits>
    struct xpression_visitor
      : xpression_visitor_base<BidiIter>
    {
        typedef BidiIter iterator_type;
        typedef ICase icase_type;
        typedef Traits traits_type;
        typedef typename boost::iterator_value<BidiIter>::type char_type;

        explicit xpression_visitor(Traits const &tr, shared_ptr<regex_impl<BidiIter> > const &self)
          : xpression_visitor_base<BidiIter>(self)
          , traits_(tr)
        {
        }

        template<typename Matcher>
        struct apply
        {
            typedef typename transmogrify<BidiIter, ICase, Traits, Matcher>::type type;
        };

        template<typename Matcher>
        typename apply<Matcher>::type
        call(Matcher const &matcher)
        {
            this->visit_(matcher);
            return transmogrify<BidiIter, ICase, Traits, Matcher>::call(matcher, *this);
        }

        Traits const &traits() const
        {
            return this->traits_;
        }

    private:

        Traits traits_;
    };

}}}

#endif
