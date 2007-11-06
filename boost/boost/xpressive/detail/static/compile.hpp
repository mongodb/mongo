///////////////////////////////////////////////////////////////////////////////
// compile.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_COMPILE_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_COMPILE_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/mpl/bool.hpp>
#include <boost/iterator/iterator_traits.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/regex_traits.hpp>
#include <boost/xpressive/detail/core/regex_impl.hpp>
#include <boost/xpressive/detail/core/linker.hpp>
#include <boost/xpressive/detail/core/optimize.hpp>
#include <boost/xpressive/detail/core/adaptor.hpp>
#include <boost/xpressive/detail/core/matcher/end_matcher.hpp>
#include <boost/xpressive/detail/static/static.hpp>
#include <boost/xpressive/detail/static/productions/visitor.hpp>
#include <boost/xpressive/detail/static/productions/domain_tags.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // static_compile_impl2
    template<typename Xpr, typename BidiIter, typename Traits>
    void static_compile_impl2(Xpr const &xpr, regex_impl<BidiIter> &impl, Traits const &traits)
    {
        typedef typename iterator_value<BidiIter>::type char_type;
        // "compile" the regex and wrap it in an xpression_adaptor
        xpression_visitor<BidiIter, mpl::false_, Traits> visitor(traits, impl.shared_from_this());
        visitor.impl().traits_.reset(new Traits(visitor.traits()));
        visitor.impl().xpr_ = make_adaptor<BidiIter>(
            proto::compile(xpr, end_xpression(), visitor, seq_tag()));

        // "link" the regex
        xpression_linker<char_type> linker(visitor.traits());
        visitor.impl().xpr_->link(linker);

        // optimization: get the peek chars OR the boyer-moore search string
        optimize_regex(visitor.impl(), visitor.traits(), is_random<BidiIter>());

        // copy the implementation
        impl.tracking_copy(visitor.impl());
    }

    ///////////////////////////////////////////////////////////////////////////////
    // static_compile_impl1
    template<typename Xpr, typename BidiIter>
    void static_compile_impl1(Xpr const &xpr, regex_impl<BidiIter> &impl)
    {
        // use default traits
        typedef typename iterator_value<BidiIter>::type char_type;
        typedef typename default_regex_traits<char_type>::type traits_type;
        traits_type traits;
        static_compile_impl2(xpr, impl, traits);
    }

    ///////////////////////////////////////////////////////////////////////////////
    // static_compile_impl1
    template<typename Locale, typename Xpr, typename BidiIter>
    void static_compile_impl1
    (
        proto::binary_op<locale_modifier<Locale>, Xpr, modifier_tag> const &xpr
      , regex_impl<BidiIter> &impl
    )
    {
        // use specified traits
        typedef typename regex_traits_type<Locale, BidiIter>::type traits_type;
        static_compile_impl2(proto::right(xpr), impl, traits_type(proto::left(xpr).getloc()));
    }

    ///////////////////////////////////////////////////////////////////////////////
    // static_compile
    template<typename Xpr, typename BidiIter>
    void static_compile(Xpr const &xpr, regex_impl<BidiIter> &impl)
    {
        static_compile_impl1(xpr, impl);
    }

}}} // namespace boost::xpressive::detail

#endif
