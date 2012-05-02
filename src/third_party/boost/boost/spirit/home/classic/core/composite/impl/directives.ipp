/*=============================================================================
    Copyright (c) 1998-2003 Joel de Guzman
    Copyright (c) 2001 Daniel Nuffer
    Copyright (c) 2001 Bruce Florman
    Copyright (c) 2002 Raghavendra Satish
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_DIRECTIVES_IPP)
#define BOOST_SPIRIT_DIRECTIVES_IPP

///////////////////////////////////////////////////////////////////////////////
#include <boost/spirit/home/classic/core/scanner/skipper.hpp>

namespace boost { namespace spirit {

BOOST_SPIRIT_CLASSIC_NAMESPACE_BEGIN

    template <typename BaseT>
    struct no_skipper_iteration_policy;

    template <typename BaseT>
    struct inhibit_case_iteration_policy;

    template <typename A, typename B>
    struct alternative;

    template <typename A, typename B>
    struct longest_alternative;

    template <typename A, typename B>
    struct shortest_alternative;

    namespace impl
    {
        template <typename RT, typename ST, typename ScannerT, typename BaseT>
        inline RT
        contiguous_parser_parse(
            ST const& s,
            ScannerT const& scan,
            skipper_iteration_policy<BaseT> const&)
        {
            typedef scanner_policies<
                no_skipper_iteration_policy<
                    BOOST_DEDUCED_TYPENAME ScannerT::iteration_policy_t>,
                BOOST_DEDUCED_TYPENAME ScannerT::match_policy_t,
                BOOST_DEDUCED_TYPENAME ScannerT::action_policy_t
            > policies_t;

            scan.skip(scan);
            RT hit = s.parse(scan.change_policies(policies_t(scan)));
            // We will not do a post skip!!!
            return hit;
        }

        template <typename RT, typename ST, typename ScannerT, typename BaseT>
        inline RT
        contiguous_parser_parse(
            ST const& s,
            ScannerT const& scan,
            no_skipper_iteration_policy<BaseT> const&)
        {
            return s.parse(scan);
        }

        template <typename RT, typename ST, typename ScannerT>
        inline RT
        contiguous_parser_parse(
            ST const& s,
            ScannerT const& scan,
            iteration_policy const&)
        {
            return s.parse(scan);
        }

        template <
            typename RT,
            typename ParserT,
            typename ScannerT,
            typename BaseT>
        inline RT
        implicit_lexeme_parse(
            ParserT const& p,
            ScannerT const& scan,
            skipper_iteration_policy<BaseT> const&)
        {
            typedef scanner_policies<
                no_skipper_iteration_policy<
                    BOOST_DEDUCED_TYPENAME ScannerT::iteration_policy_t>,
                BOOST_DEDUCED_TYPENAME ScannerT::match_policy_t,
                BOOST_DEDUCED_TYPENAME ScannerT::action_policy_t
            > policies_t;

            scan.skip(scan);
            RT hit = p.parse_main(scan.change_policies(policies_t(scan)));
            // We will not do a post skip!!!
            return hit;
        }

        template <
            typename RT,
            typename ParserT,
            typename ScannerT,
            typename BaseT>
        inline RT
        implicit_lexeme_parse(
            ParserT const& p,
            ScannerT const& scan,
            no_skipper_iteration_policy<BaseT> const&)
        {
            return p.parse_main(scan);
        }

        template <typename RT, typename ParserT, typename ScannerT>
        inline RT
        implicit_lexeme_parse(
            ParserT const& p,
            ScannerT const& scan,
            iteration_policy const&)
        {
            return p.parse_main(scan);
        }

        template <typename RT, typename ST, typename ScannerT>
        inline RT
        inhibit_case_parser_parse(
            ST const& s,
            ScannerT const& scan,
            iteration_policy const&)
        {
            typedef scanner_policies<
                inhibit_case_iteration_policy<
                    BOOST_DEDUCED_TYPENAME ScannerT::iteration_policy_t>,
                BOOST_DEDUCED_TYPENAME ScannerT::match_policy_t,
                BOOST_DEDUCED_TYPENAME ScannerT::action_policy_t
            > policies_t;

            return s.parse(scan.change_policies(policies_t(scan)));
        }

        template <typename RT, typename ST, typename ScannerT, typename BaseT>
        inline RT
        inhibit_case_parser_parse(
            ST const& s,
            ScannerT const& scan,
            inhibit_case_iteration_policy<BaseT> const&)
        {
            return s.parse(scan);
        }

#if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)

        ///////////////////////////////////////////////////////////////////////
        //
        //  from spirit 1.1 (copyright (c) 2001 Bruce Florman)
        //  various workarounds to support longest and shortest directives
        //
        ///////////////////////////////////////////////////////////////////////
        template <typename T>
        struct is_alternative
        {
        //  Determine at compile time (without partial specialization)
        //  whether a given type is an instance of the alternative<A,B>

            static T t();
            template <typename A, typename B>
            static char test_(alternative<A, B> const&);    // no implementation
            static int  test_(...);                         // no implementation
            enum { r = sizeof(char) == sizeof(test_(t())) };
            typedef mpl::bool_<r> value;
        };

        template <typename T> struct select_to_longest;

        template <typename T>
        struct to_longest_alternative
        {
            typedef typename select_to_longest<T>::result_t result_t;
            typedef typename select_to_longest<T>::plain_t  plain_t;
            typedef typename select_to_longest<T>::choose_t choose_t;
            static result_t convert(T const& a);
        };

        template <typename T>
        struct to_longest_generic
        {
            typedef T const&        result_t;
            typedef T               plain_t;
            typedef mpl::false_    choose_t;
        };

        template <typename T>
        inline T const&
        to_longest_convert(T const& a, mpl::false_)
        { return a; }

        template <typename T>
        struct to_longest_recursive
        {
            typedef typename to_longest_alternative<
                typename T::left_t>::plain_t    a_t;
            typedef typename to_longest_alternative<
                typename T::right_t>::plain_t   b_t;

            typedef longest_alternative<a_t, b_t>   result_t;

            typedef result_t    plain_t;
            typedef mpl::true_ choose_t;
        };

        template <typename A, typename B>
        inline typename to_longest_alternative<alternative<A, B> >::result_t
        to_longest_convert(alternative<A, B> const& alt, mpl::true_)
        {
            typedef typename to_longest_alternative<
                alternative<A, B> >::result_t result_t;
            return result_t(
                to_longest_alternative<A>::convert(alt.left()),
                to_longest_alternative<B>::convert(alt.right()));
        }

        template <typename T>
        inline typename to_longest_alternative<T>::result_t
        to_longest_alternative<T>::convert(T const& a)
        {
            return to_longest_convert(
                a, to_longest_alternative<T>::choose_t());
        }

        template <typename T>
        struct select_to_longest
        {
            typedef typename mpl::if_<
                is_alternative<T>           //  IF
                , to_longest_recursive<T>   //  THEN
                , to_longest_generic<T>     //  ELSE
            >::type type;

            typedef typename select_to_longest::type::result_t result_t;
            typedef typename select_to_longest::type::plain_t  plain_t;
            typedef typename select_to_longest::type::choose_t choose_t;
        };

        template <typename T> struct select_to_shortest;

        template <typename T>
        struct to_shortest_alternative
        {
            typedef typename select_to_shortest<T>::result_t    result_t;
            typedef typename select_to_shortest<T>::plain_t     plain_t;
            typedef typename select_to_shortest<T>::choose_t    choose_t;
            static result_t convert(T const& a);
        };

        template <typename T>
        struct to_shortest_generic
        {
            typedef T const&        result_t;
            typedef T               plain_t;
            typedef mpl::false_    choose_t;
        };

        template <typename T>
        inline T const&
        to_shortest_convert(T const& a, mpl::false_) { return a; }

        template <typename T>
        struct to_shortest_recursive
        {
            typedef typename to_shortest_alternative<
                typename T::left_t>::plain_t    a_t;
            typedef typename to_shortest_alternative<
                typename T::right_t>::plain_t   b_t;

            typedef shortest_alternative<a_t, b_t>  result_t;

            typedef result_t        plain_t;
            typedef mpl::true_     choose_t;
        };

        template <typename A, typename B>
        inline typename to_shortest_alternative<alternative<A, B> >::result_t
        to_shortest_convert(alternative<A, B> const& alt, mpl::true_)
        {
            typedef typename to_shortest_alternative<
                alternative<A, B> >::result_t result_t;
            return result_t(
                to_shortest_alternative<A>::convert(alt.left()),
                to_shortest_alternative<B>::convert(alt.right()));
        }

        template <typename T>
        inline typename to_shortest_alternative<T>::result_t
        to_shortest_alternative<T>::convert(T const& a)
        {
            return to_shortest_convert(
                a, to_shortest_alternative<T>::choose_t());
        }

        template <typename T>
        struct select_to_shortest
        {
            typedef typename mpl::if_<
                is_alternative<T>           //  IF
                , to_shortest_recursive<T>  //  THEN
                , to_shortest_generic<T>    //  ELSE
            >::type type;

            typedef typename select_to_shortest::type::result_t result_t;
            typedef typename select_to_shortest::type::plain_t  plain_t;
            typedef typename select_to_shortest::type::choose_t choose_t;
        };
#else
        template <typename T>
        struct to_longest_alternative
        {
            typedef T result_t;
            static result_t const&
            convert(T const& a)  //  Special (end) case
            { return a; }
        };

        template <typename A, typename B>
        struct to_longest_alternative<alternative<A, B> >
        {
            typedef typename to_longest_alternative<A>::result_t    a_t;
            typedef typename to_longest_alternative<B>::result_t    b_t;
            typedef longest_alternative<a_t, b_t>                   result_t;

            static result_t
            convert(alternative<A, B> const& alt) // Recursive case
            {
                return result_t(
                    to_longest_alternative<A>::convert(alt.left()),
                    to_longest_alternative<B>::convert(alt.right()));
            }
        };

        template <typename T>
        struct to_shortest_alternative
        {
            typedef T result_t;
            static result_t const&
            convert(T const& a) //  Special (end) case
            { return a; }
        };

        template <typename A, typename B>
        struct to_shortest_alternative<alternative<A, B> >
        {
            typedef typename to_shortest_alternative<A>::result_t   a_t;
            typedef typename to_shortest_alternative<B>::result_t   b_t;
            typedef shortest_alternative<a_t, b_t>                  result_t;

            static result_t
            convert(alternative<A, B> const& alt) //  Recursive case
            {
                return result_t(
                    to_shortest_alternative<A>::convert(alt.left()),
                    to_shortest_alternative<B>::convert(alt.right()));
            }
        };
#endif
    }

BOOST_SPIRIT_CLASSIC_NAMESPACE_END

}} // namespace boost::spirit

#endif

