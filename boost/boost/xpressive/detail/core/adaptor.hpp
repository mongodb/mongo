///////////////////////////////////////////////////////////////////////////////
// adaptor.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_ADAPTOR_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_ADAPTOR_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/shared_ptr.hpp>
#include <boost/call_traits.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/dynamic/matchable.hpp>
#include <boost/xpressive/detail/core/state.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// xpression_adaptor
//
//   wrap a static xpression in a matchable interface so it can be stored
//   in and invoked from a basic_regex object.
template<typename Xpr, typename BidiIter>
struct xpression_adaptor
  : matchable<BidiIter>
{
    typedef typename iterator_value<BidiIter>::type char_type;

    Xpr xpr_;

    xpression_adaptor(typename call_traits<Xpr>::param_type xpr)
      : xpr_(xpr)
    {
    }

    bool match(state_type<BidiIter> &state) const
    {
        return this->xpr_.match(state);
    }

    std::size_t get_width(state_type<BidiIter> *state) const
    {
        return this->xpr_.get_width(state);
    }

    void link(xpression_linker<char_type> &linker) const
    {
        this->xpr_.link(linker);
    }

    void peek(xpression_peeker<char_type> &peeker) const
    {
        this->xpr_.peek(peeker);
    }

private:
    xpression_adaptor &operator =(xpression_adaptor const &);
};

///////////////////////////////////////////////////////////////////////////////
// make_adaptor
//
template<typename BidiIter, typename Xpr>
inline shared_ptr<matchable<BidiIter> const> make_adaptor(Xpr const &xpr)
{
    return shared_ptr<matchable<BidiIter> const>(new xpression_adaptor<Xpr, BidiIter>(xpr));
}

}}} // namespace boost::xpressive::detail

#endif
