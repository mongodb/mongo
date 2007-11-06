///////////////////////////////////////////////////////////////////////////////
// regex_impl.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_REGEX_IMPL_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_REGEX_IMPL_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <vector>
#include <boost/noncopyable.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/utility/tracking_ptr.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// finder
//
template<typename BidiIter>
struct finder
  : noncopyable
{
    virtual ~finder() {}
    virtual bool operator ()(state_type<BidiIter> &state) const = 0;
};

///////////////////////////////////////////////////////////////////////////////
// regex_impl
//
template<typename BidiIter>
struct regex_impl
  : enable_reference_tracking<regex_impl<BidiIter> >
{
    typedef typename iterator_value<BidiIter>::type char_type;

    regex_impl()
      : enable_reference_tracking<regex_impl<BidiIter> >()
      , xpr_()
      , traits_()
      , finder_()
      , mark_count_(0)
      , hidden_mark_count_(0)
    {
        #ifdef BOOST_XPRESSIVE_DEBUG_CYCLE_TEST
        ++instances;
        #endif
    }

    regex_impl(regex_impl<BidiIter> const &that)
      : enable_reference_tracking<regex_impl<BidiIter> >(that)
      , xpr_(that.xpr_)
      , traits_(that.traits_)
      , finder_(that.finder_)
      , mark_count_(that.mark_count_)
      , hidden_mark_count_(that.hidden_mark_count_)
    {
        #ifdef BOOST_XPRESSIVE_DEBUG_CYCLE_TEST
        ++instances;
        #endif
    }

    ~regex_impl()
    {
        #ifdef BOOST_XPRESSIVE_DEBUG_CYCLE_TEST
        --instances;
        #endif
    }

    void swap(regex_impl<BidiIter> &that)
    {
        enable_reference_tracking<regex_impl<BidiIter> >::swap(that);
        this->xpr_.swap(that.xpr_);
        this->traits_.swap(that.traits_);
        this->finder_.swap(that.finder_);
        std::swap(this->mark_count_, that.mark_count_);
        std::swap(this->hidden_mark_count_, that.hidden_mark_count_);
    }

    shared_ptr<matchable<BidiIter> const>  xpr_;
    shared_ptr<void const> traits_;
    shared_ptr<finder<BidiIter> > finder_;
    std::size_t mark_count_;
    std::size_t hidden_mark_count_;

    #ifdef BOOST_XPRESSIVE_DEBUG_CYCLE_TEST
    static int instances;
    #endif
};

#ifdef BOOST_XPRESSIVE_DEBUG_CYCLE_TEST
template<typename BidiIter>
int regex_impl<BidiIter>::instances = 0;
#endif

}}} // namespace boost::xpressive::detail

#endif
