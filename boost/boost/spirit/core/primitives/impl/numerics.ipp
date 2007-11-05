/*=============================================================================
    Copyright (c) 1998-2003 Joel de Guzman
    Copyright (c) 2001-2003 Hartmut Kaiser
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#ifndef BOOST_SPIRIT_NUMERICS_IPP
#define BOOST_SPIRIT_NUMERICS_IPP

#include <cmath>

#if defined(BOOST_NO_STDC_NAMESPACE)
#  define BOOST_SPIRIT_IMPL_STD_NS
#else
#  define BOOST_SPIRIT_IMPL_STD_NS std
#endif

namespace boost { namespace spirit {

    struct sign_parser; // forward declaration only

    namespace impl
    {
        ///////////////////////////////////////////////////////////////////////
        //
        //  Extract the prefix sign (- or +)
        //
        ///////////////////////////////////////////////////////////////////////
        template <typename ScannerT>
        bool
        extract_sign(ScannerT const& scan, std::size_t& count)
        {
            //  Extract the sign
            count = 0;
            bool neg = *scan == '-';
            if (neg || (*scan == '+'))
            {
                ++scan;
                ++count;
                return neg;
            }

            return false;
        }

        ///////////////////////////////////////////////////////////////////////
        //
        //  Traits class for radix specific number conversion
        //
        //      Test the validity of a single character:
        //
        //          template<typename CharT> static bool is_valid(CharT ch);
        //
        //      Convert a digit from character representation to binary
        //      representation:
        //
        //          template<typename CharT> static int digit(CharT ch);
        //
        ///////////////////////////////////////////////////////////////////////
        template<const int Radix>
        struct radix_traits;

        ////////////////////////////////// Binary
        template<>
        struct radix_traits<2>
        {
            template<typename CharT>
            static bool is_valid(CharT ch)
            {
                return ('0' == ch || '1' == ch);
            }

            template<typename CharT>
            static int digit(CharT ch)
            {
                return ch - '0';
            }
        };

        ////////////////////////////////// Octal
        template<>
        struct radix_traits<8>
        {
            template<typename CharT>
            static bool is_valid(CharT ch)
            {
                return ('0' <= ch && ch <= '7');
            }

            template<typename CharT>
            static int digit(CharT ch)
            {
                return ch - '0';
            }
        };

        ////////////////////////////////// Decimal
        template<>
        struct radix_traits<10>
        {
            template<typename CharT>
            static bool is_valid(CharT ch)
            {
                return impl::isdigit_(ch);
            }

            template<typename CharT>
            static int digit(CharT ch)
            {
                return ch - '0';
            }
        };

        ////////////////////////////////// Hexadecimal
        template<>
        struct radix_traits<16>
        {
            template<typename CharT>
            static bool is_valid(CharT ch)
            {
                return impl::isxdigit_(ch);
            }

            template<typename CharT>
            static int digit(CharT ch)
            {
                if (impl::isdigit_(ch))
                    return ch - '0';
                return impl::tolower_(ch) - 'a' + 10;
            }
        };

        ///////////////////////////////////////////////////////////////////////
        //
        //      Helper templates for encapsulation of radix specific
        //      conversion of an input string to an integral value.
        //
        //      main entry point:
        //
        //          extract_int<Radix, MinDigits, MaxDigits, Accumulate>
        //              ::f(first, last, n, count);
        //
        //          The template parameter Radix represents the radix of the
        //          number contained in the parsed string. The template
        //          parameter MinDigits specifies the minimum digits to
        //          accept. The template parameter MaxDigits specifies the
        //          maximum digits to parse. A -1 value for MaxDigits will
        //          make it parse an arbitrarilly large number as long as the
        //          numeric type can hold it. Accumulate is either
        //          positive_accumulate<Radix> (default) for parsing positive
        //          numbers or negative_accumulate<Radix> otherwise.
        //
        //          scan.first and scan.last are iterators as usual (i.e.
        //          first is mutable and is moved forward when a match is
        //          found), n is a variable that holds the number (passed by
        //          reference). The number of parsed characters is added to
        //          count (also passed by reference)
        //
        //      NOTE:
        //              Returns a non-match, if the number to parse
        //              overflows (or underflows) the used integral type.
        //              Overflow (or underflow) is detected when an
        //              operation wraps a value from its maximum to its
        //              minimum (or vice-versa). For example, overflow
        //              occurs when the result of the expression n * x is
        //              less than n (assuming n is positive and x is
        //              greater than 1).
        //
        //      BEWARE:
        //              the parameters 'n' and 'count' should be properly
        //              initialized before calling this function.
        //
        ///////////////////////////////////////////////////////////////////////
        template <int Radix>
        struct positive_accumulate
        {
            //  Use this accumulator if number is positive

            template <typename T>
            static bool check(T const& n, T const& prev)
            {
                return n < prev;
            }

            template <typename T, typename CharT>
            static void add(T& n, CharT ch)
            {
                n += radix_traits<Radix>::digit(ch);
            }
        };

        template <int Radix>
        struct negative_accumulate
        {
            //  Use this accumulator if number is negative

            template <typename T>
            static bool check(T const& n, T const& prev)
            {
                return n > prev;
            }

            template <typename T, typename CharT>
            static void add(T& n, CharT ch)
            {
                n -= radix_traits<Radix>::digit(ch);
            }
        };

        template <int Radix, typename Accumulate>
        struct extract_int_base
        {
            //  Common code for extract_int specializations
            template <typename ScannerT, typename T>
            static bool
            f(ScannerT& scan, T& n)
            {
                T prev = n;
                n *= Radix;
                if (Accumulate::check(n, prev))
                    return false;   //  over/underflow!
                prev = n;
                Accumulate::add(n, *scan);
                if (Accumulate::check(n, prev))
                    return false;   //  over/underflow!
                return true;
            }
        };

        template <bool Bounded>
        struct extract_int_
        {
            template <
                int Radix,
                unsigned MinDigits,
                int MaxDigits,
                typename Accumulate
            >
            struct apply
            {
                typedef extract_int_base<Radix, Accumulate> base;
                typedef radix_traits<Radix> check;

                template <typename ScannerT, typename T>
                static bool
                f(ScannerT& scan, T& n, std::size_t& count)
                {
                    std::size_t i = 0;
                    for (; (i < MaxDigits) && !scan.at_end()
                        && check::is_valid(*scan);
                        ++i, ++scan, ++count)
                    {
                        if (!base::f(scan, n))
                            return false;   //  over/underflow!
                    }
                    return i >= MinDigits;
                }
            };
        };

        template <>
        struct extract_int_<false>
        {
            template <
                int Radix,
                unsigned MinDigits,
                int MaxDigits,
                typename Accumulate
            >
            struct apply
            {
                typedef extract_int_base<Radix, Accumulate> base;
                typedef radix_traits<Radix> check;

                template <typename ScannerT, typename T>
                static bool
                f(ScannerT& scan, T& n, std::size_t& count)
                {
                    std::size_t i = 0;
                    for (; !scan.at_end() && check::is_valid(*scan);
                        ++i, ++scan, ++count)
                    {
                        if (!base::f(scan, n))
                            return false;   //  over/underflow!
                    }
                    return i >= MinDigits;
                }
            };
        };

        //////////////////////////////////
        template <
            int Radix, unsigned MinDigits, int MaxDigits,
            typename Accumulate = positive_accumulate<Radix>
        >
        struct extract_int
        {
            template <typename ScannerT, typename T>
            static bool
            f(ScannerT& scan, T& n, std::size_t& count)
            {
                typedef typename extract_int_<(MaxDigits >= 0)>::template
                    apply<Radix, MinDigits, MaxDigits, Accumulate> extractor;
                return extractor::f(scan, n, count);
            }
        };

        ///////////////////////////////////////////////////////////////////////
        //
        //  uint_parser_impl class
        //
        ///////////////////////////////////////////////////////////////////////
        template <
            typename T = unsigned,
            int Radix = 10,
            unsigned MinDigits = 1,
            int MaxDigits = -1
        >
        struct uint_parser_impl
            : parser<uint_parser_impl<T, Radix, MinDigits, MaxDigits> >
        {
            typedef uint_parser_impl<T, Radix, MinDigits, MaxDigits> self_t;

            template <typename ScannerT>
            struct result
            {
                typedef typename match_result<ScannerT, T>::type type;
            };

            template <typename ScannerT>
            typename parser_result<self_t, ScannerT>::type
            parse(ScannerT const& scan) const
            {
                if (!scan.at_end())
                {
                    T n = 0;
                    std::size_t count = 0;
                    typename ScannerT::iterator_t save = scan.first;
                    if (extract_int<Radix, MinDigits, MaxDigits>::
                        f(scan, n, count))
                    {
                        return scan.create_match(count, n, save, scan.first);
                    }
                    // return no-match if number overflows
                }
                return scan.no_match();
            }
        };

        ///////////////////////////////////////////////////////////////////////
        //
        //  int_parser_impl class
        //
        ///////////////////////////////////////////////////////////////////////
        template <
            typename T = unsigned,
            int Radix = 10,
            unsigned MinDigits = 1,
            int MaxDigits = -1
        >
        struct int_parser_impl
            : parser<int_parser_impl<T, Radix, MinDigits, MaxDigits> >
        {
            typedef int_parser_impl<T, Radix, MinDigits, MaxDigits> self_t;

            template <typename ScannerT>
            struct result
            {
                typedef typename match_result<ScannerT, T>::type type;
            };

            template <typename ScannerT>
            typename parser_result<self_t, ScannerT>::type
            parse(ScannerT const& scan) const
            {
                typedef extract_int<Radix, MinDigits, MaxDigits,
                    negative_accumulate<Radix> > extract_int_neg_t;
                typedef extract_int<Radix, MinDigits, MaxDigits>
                    extract_int_pos_t;

                if (!scan.at_end())
                {
                    T n = 0;
                    std::size_t count = 0;
                    typename ScannerT::iterator_t save = scan.first;

                    bool hit = impl::extract_sign(scan, count);

                    if (hit)
                        hit = extract_int_neg_t::f(scan, n, count);
                    else
                        hit = extract_int_pos_t::f(scan, n, count);

                    if (hit)
                        return scan.create_match(count, n, save, scan.first);
                    else
                        scan.first = save;
                    // return no-match if number overflows or underflows
                }
                return scan.no_match();
            }
        };

        ///////////////////////////////////////////////////////////////////////
        //
        //  real_parser_impl class
        //
        ///////////////////////////////////////////////////////////////////////
#if (defined(BOOST_MSVC) && (BOOST_MSVC <= 1310))
#pragma warning(push)
#pragma warning(disable:4127)
#endif

        template <typename RT, typename T, typename RealPoliciesT>
        struct real_parser_impl
        {
            typedef real_parser_impl<RT, T, RealPoliciesT> self_t;

            template <typename ScannerT>
            RT parse_main(ScannerT const& scan) const
            {
                if (scan.at_end())
                    return scan.no_match();
                typename ScannerT::iterator_t save = scan.first;

                typedef typename parser_result<sign_parser, ScannerT>::type
                    sign_match_t;
                typedef typename parser_result<chlit<>, ScannerT>::type
                    exp_match_t;

                sign_match_t    sign_match = RealPoliciesT::parse_sign(scan);
                std::size_t     count = sign_match ? sign_match.length() : 0;
                bool            neg = sign_match.has_valid_attribute() ?
                                    sign_match.value() : false;

                RT              n_match = RealPoliciesT::parse_n(scan);
                T               n = n_match.has_valid_attribute() ?
                                    n_match.value() : T(0);
                bool            got_a_number = n_match;
                exp_match_t     e_hit;

                if (!got_a_number && !RealPoliciesT::allow_leading_dot)
                     return scan.no_match();
                else
                    count += n_match.length();

                if (neg)
                    n = -n;

                if (RealPoliciesT::parse_dot(scan))
                {
                    //  We got the decimal point. Now we will try to parse
                    //  the fraction if it is there. If not, it defaults
                    //  to zero (0) only if we already got a number.

                    if (RT hit = RealPoliciesT::parse_frac_n(scan))
                    {
                        hit.value(hit.value()
                            * BOOST_SPIRIT_IMPL_STD_NS::
                                pow(T(10), T(-hit.length())));
                        if (neg)
                            n -= hit.value();
                        else
                            n += hit.value();
                        count += hit.length() + 1;

                    }

                    else if (!got_a_number ||
                        !RealPoliciesT::allow_trailing_dot)
                        return scan.no_match();

                    e_hit = RealPoliciesT::parse_exp(scan);
                }
                else
                {
                    //  We have reached a point where we
                    //  still haven't seen a number at all.
                    //  We return early with a no-match.
                    if (!got_a_number)
                        return scan.no_match();

                    //  If we must expect a dot and we didn't see
                    //  an exponent, return early with a no-match.
                    e_hit = RealPoliciesT::parse_exp(scan);
                    if (RealPoliciesT::expect_dot && !e_hit)
                        return scan.no_match();
                }

                if (e_hit)
                {
                    //  We got the exponent prefix. Now we will try to parse the
                    //  actual exponent. It is an error if it is not there.
                    if (RT e_n_hit = RealPoliciesT::parse_exp_n(scan))
                    {
                        n *= BOOST_SPIRIT_IMPL_STD_NS::
                            pow(T(10), T(e_n_hit.value()));
                        count += e_n_hit.length() + e_hit.length();
                    }
                    else
                    {
                        //  Oops, no exponent, return a no-match
                        return scan.no_match();
                    }
                }

                return scan.create_match(count, n, save, scan.first);
            }

            template <typename ScannerT>
            static RT parse(ScannerT const& scan)
            {
                static self_t this_;
                return impl::implicit_lexeme_parse<RT>(this_, scan, scan);
            }
        };

#if (defined(BOOST_MSVC) && (BOOST_MSVC <= 1310))
#pragma warning(pop)
#endif

    }   //  namespace impl

///////////////////////////////////////////////////////////////////////////////
}} // namespace boost::spirit

#endif
#undef BOOST_SPIRIT_IMPL_STD_NS
