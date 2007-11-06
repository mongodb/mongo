/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_SCANNER_FWD_HPP)
#define BOOST_SPIRIT_SCANNER_FWD_HPP

namespace boost { namespace spirit 
{

    ///////////////////////////////////////////////////////////////////////////
    //
    //  policy classes
    //
    ///////////////////////////////////////////////////////////////////////////
    struct iteration_policy;
    struct action_policy;
    struct match_policy;

    ///////////////////////////////////////////////////////////////////////////
    //
    //  scanner_policies class
    //
    ///////////////////////////////////////////////////////////////////////////
    template <
        typename IterationPolicyT   = iteration_policy,
        typename MatchPolicyT       = match_policy,
        typename ActionPolicyT      = action_policy>
    struct scanner_policies;

    ///////////////////////////////////////////////////////////////////////////
    //
    //  scanner class
    //
    ///////////////////////////////////////////////////////////////////////////
    template <
        typename IteratorT  = char const*,
        typename PoliciesT  = scanner_policies<> >
    class scanner;

}} // namespace boost::spirit

#endif

