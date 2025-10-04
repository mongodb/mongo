/*=============================================================================
    Copyright (c) 2001-2014 Joel de Guzman
    Copyright (c) 2013 Agustin Berge
    Copyright (c) 2024 Nana Sakisaka

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_X3_SKIP_JANUARY_26_2008_0422PM)
#define BOOST_SPIRIT_X3_SKIP_JANUARY_26_2008_0422PM

#include <boost/spirit/home/x3/support/context.hpp>
#include <boost/spirit/home/x3/support/unused.hpp>
#include <boost/spirit/home/x3/support/expectation.hpp>
#include <boost/spirit/home/x3/core/skip_over.hpp>
#include <boost/spirit/home/x3/core/parser.hpp>
#include <boost/utility/enable_if.hpp>

namespace boost { namespace spirit { namespace x3
{
    template <typename Subject>
    struct reskip_directive : unary_parser<Subject, reskip_directive<Subject>>
    {
        typedef unary_parser<Subject, reskip_directive<Subject>> base_type;
        static bool const is_pass_through_unary = true;
        static bool const handles_container = Subject::handles_container;

        constexpr reskip_directive(Subject const& subject)
          : base_type(subject) {}

        template <typename Iterator, typename Context
          , typename RContext, typename Attribute>
        typename disable_if<has_skipper<Context>, bool>::type
        parse(Iterator& first, Iterator const& last
          , Context& context, RContext& rcontext, Attribute& attr) const
        {
            auto const& skipper =
                detail::get_unused_skipper(x3::get<skipper_tag>(context));

            auto const local_ctx = make_context<skipper_tag>(skipper, context);
            bool const r = this->subject.parse(first, last, local_ctx, rcontext, attr);

        #if !BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
            if (has_expectation_failure(local_ctx))
            {
                set_expectation_failure(get_expectation_failure(local_ctx), context);
            }
        #endif

            return r;
        }
        template <typename Iterator, typename Context
          , typename RContext, typename Attribute>
        typename enable_if<has_skipper<Context>, bool>::type
        parse(Iterator& first, Iterator const& last
          , Context const& context, RContext& rcontext, Attribute& attr) const
        {
            return this->subject.parse(first, last, context, rcontext, attr);
        }
    };

    template <typename Subject, typename Skipper>
    struct skip_directive : unary_parser<Subject, skip_directive<Subject, Skipper>>
    {
        typedef unary_parser<Subject, skip_directive<Subject, Skipper>> base_type;
        static bool const is_pass_through_unary = true;
        static bool const handles_container = Subject::handles_container;

        constexpr skip_directive(Subject const& subject, Skipper const& skipper)
          : base_type(subject)
          , skipper(skipper)
        {}

        template <typename Iterator, typename RContext, typename Attribute>
        bool parse(Iterator& first, Iterator const& last
          , unused_type const&, RContext& rcontext, Attribute& attr) const
        {
            // It is perfectly fine to omit the expectation_failure context
            // even in non-throwing mode if and only if the skipper itself
            // is expectation-less.
            //
            // For example:
            //   skip(a > b) [lit('foo')]
            //   skip(c >> d)[lit('foo')]
            //     `a > b`  should require non-`unused_type` context, but
            //     `c >> d` should NOT require non-`unused_type` context
            //
            // However, it's impossible right now to detect whether
            // `this->subject` actually is expectation-less, so we just
            // call the parse function to see what will happen. If the
            // subject turns out to lack the expectation context,
            // static_assert will be engaged in other locations.
            //
            // Anyways, we don't need to repack the expectation context
            // into our brand new skipper context, in contrast to the
            // repacking process done in `x3::skip_over`.
            return this->subject.parse(first, last,
                make_context<skipper_tag>(skipper), rcontext, attr);
        }

        template <typename Iterator, typename Context, typename RContext, typename Attribute>
        bool parse(Iterator& first, Iterator const& last
          , Context const& context, RContext& rcontext, Attribute& attr) const
        {
        #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
            return this->subject.parse(first, last, make_context<skipper_tag>(skipper, context), rcontext, attr);

        #else
            static_assert(
                !std::is_same_v<expectation_failure_t<Context>, unused_type>,
                "Context type was not specified for x3::expectation_failure_tag. "
                "You probably forgot: `x3::with<x3::expectation_failure_tag>(failure)[p]`. "
                "Note that you must also bind the context to your skipper.");

            // This logic is heavily related to the instantiation chain;
            // see `x3::skip_over` for details.
            auto const local_ctx = make_context<skipper_tag>(skipper, context);
            bool const r = this->subject.parse(first, last, local_ctx, rcontext, attr);

            if (has_expectation_failure(local_ctx))
            {
                set_expectation_failure(get_expectation_failure(local_ctx), context);
            }
            return r;
        #endif
        }

        Skipper const skipper;
    };

    struct reskip_gen
    {
        template <typename Skipper>
        struct skip_gen
        {
            constexpr skip_gen(Skipper const& skipper)
              : skipper_(skipper) {}

            template <typename Subject>
            constexpr skip_directive<typename extension::as_parser<Subject>::value_type, Skipper>
            operator[](Subject const& subject) const
            {
                return { as_parser(subject), skipper_ };
            }

            Skipper skipper_;
        };

        template <typename Skipper>
        constexpr skip_gen<Skipper> const operator()(Skipper const& skipper) const
        {
            return { skipper };
        }

        template <typename Subject>
        constexpr reskip_directive<typename extension::as_parser<Subject>::value_type>
        operator[](Subject const& subject) const
        {
            return { as_parser(subject) };
        }
    };

    constexpr auto skip = reskip_gen{};
}}}

#endif
