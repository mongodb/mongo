///////////////////////////////////////////////////////////////////////////////
// modifier.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_MODIFIER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_MODIFIER_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
# pragma warning(push)
# pragma warning(disable : 4510) // default constructor could not be generated
# pragma warning(disable : 4610) // user defined constructor required
#endif

#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/regex_constants.hpp>
#include <boost/xpressive/detail/static/as_xpr.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // modifier
    template<typename Modifier>
    struct modifier_op
    {
        typedef regex_constants::syntax_option_type opt_type;

        template<typename Xpr>
        struct apply
        {
            typedef proto::binary_op<Modifier, typename as_xpr_type<Xpr>::type, modifier_tag> type;
        };

        template<typename Xpr>
        typename apply<Xpr>::type    
        operator ()(Xpr const &xpr) const
        {
            return proto::make_op<modifier_tag>(this->mod_, as_xpr(xpr));
        }

        operator opt_type() const
        {
            return this->opt_;
        }

        Modifier mod_;
        opt_type opt_;
    };

}}}

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma warning(pop)
#endif

#endif
