/*=============================================================================
    Copyright (c) 2001-2014 Joel de Guzman
    Copyright (c) 2017 wanghan02
    Copyright (c) 2024 Nana Sakisaka

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_X3_EXPECT_MARCH_16_2012_1024PM)
#define BOOST_SPIRIT_X3_EXPECT_MARCH_16_2012_1024PM

#include <boost/spirit/home/x3/support/context.hpp>
#include <boost/spirit/home/x3/support/expectation.hpp>
#include <boost/spirit/home/x3/core/parser.hpp>
#include <boost/spirit/home/x3/core/detail/parse_into_container.hpp>

namespace boost { namespace spirit { namespace x3
{
    template <typename Subject>
    struct expect_directive : unary_parser<Subject, expect_directive<Subject>>
    {
        typedef unary_parser<Subject, expect_directive<Subject> > base_type;
        static bool const is_pass_through_unary = true;

        constexpr expect_directive(Subject const& subject)
          : base_type(subject) {}

        template <typename Iterator, typename Context
          , typename RContext, typename Attribute>
        bool parse(Iterator& first, Iterator const& last
          , Context const& context, RContext& rcontext, Attribute& attr) const
        {
            bool const r = this->subject.parse(first, last, context, rcontext, attr);

            if (!r)
            {
            #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
                boost::throw_exception(
                    expectation_failure<Iterator>(
                        first, what(this->subject)));
            #else
                if (!has_expectation_failure(context))
                {
                    set_expectation_failure(first, this->subject, context);
                }
            #endif
            }
            return r;
        }
    };

    struct expect_gen
    {
        template <typename Subject>
        constexpr expect_directive<typename extension::as_parser<Subject>::value_type>
        operator[](Subject const& subject) const
        {
            return { as_parser(subject) };
        }
    };

    constexpr auto expect = expect_gen{};
}}}

namespace boost { namespace spirit { namespace x3 { namespace detail
{
    // Special case handling for expect expressions.
    template <typename Subject, typename Context, typename RContext>
    struct parse_into_container_impl<expect_directive<Subject>, Context, RContext>
    {
        template <typename Iterator, typename Attribute>
        static bool call(
            expect_directive<Subject> const& parser
          , Iterator& first, Iterator const& last
          , Context const& context, RContext& rcontext, Attribute& attr)
        {
            bool const r = parse_into_container(
                parser.subject, first, last, context, rcontext, attr);

            if (!r)
            {
            #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
                boost::throw_exception(
                    expectation_failure<Iterator>(
                        first, what(parser.subject)));
            #else
                if (!has_expectation_failure(context))
                {
                    set_expectation_failure(first, parser.subject, context);
                }
            #endif
            }
            return r;
        }
    };
}}}}

#endif
