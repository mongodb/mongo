/*=============================================================================
    Copyright (c) 2001-2014 Joel de Guzman
    Copyright (c) 2017 wanghan02
    Copyright (c) 2024 Nana Sakisaka

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_X3_GUARD_FERBRUARY_02_2013_0649PM)
#define BOOST_SPIRIT_X3_GUARD_FERBRUARY_02_2013_0649PM

#include <boost/spirit/home/x3/support/context.hpp>
#include <boost/spirit/home/x3/support/expectation.hpp>
#include <boost/spirit/home/x3/core/parser.hpp>

namespace boost { namespace spirit { namespace x3
{
    enum class error_handler_result
    {
        fail
      , retry
      , accept
      , rethrow // see BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE for alternative behaviors
    };

    template <typename Subject, typename Handler>
    struct guard : unary_parser<Subject, guard<Subject, Handler>>
    {
        typedef unary_parser<Subject, guard<Subject, Handler>> base_type;
        static bool const is_pass_through_unary = true;

        constexpr guard(Subject const& subject, Handler handler)
          : base_type(subject), handler(handler) {}

        template <typename Iterator, typename Context
          , typename RuleContext, typename Attribute>
        bool parse(Iterator& first, Iterator const& last
          , Context const& context, RuleContext& rcontext, Attribute& attr) const
        {
            for (;;)
            {
                Iterator i = first;

            #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
                try
            #endif
                {
                    if (this->subject.parse(i, last, context, rcontext, attr))
                    {
                        first = i;
                        return true;
                    }
                }

            #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
                catch (expectation_failure<Iterator> const& x) {
            #else
                if (has_expectation_failure(context)) {
                    auto& x = get_expectation_failure(context);
            #endif
                    // X3 developer note: don't forget to sync this implementation with x3::detail::rule_parser
                    switch (handler(first, last, x, context))
                    {
                        case error_handler_result::fail:
                            clear_expectation_failure(context);
                            return false;

                        case error_handler_result::retry:
                            continue;

                        case error_handler_result::accept:
                            return true;

                        case error_handler_result::rethrow:
                        #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
                            throw;
                        #else
                            return false; // TODO: design decision required
                        #endif
                    }
                }
                return false;
            }
        }

        Handler handler;
    };
}}}

#endif
