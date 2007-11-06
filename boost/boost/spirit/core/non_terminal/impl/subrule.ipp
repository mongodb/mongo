/*=============================================================================
    Copyright (c) 2002-2003 Joel de Guzman
    Copyright (c) 2002-2003 Hartmut Kaiser
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_SUBRULE_IPP)
#define BOOST_SPIRIT_SUBRULE_IPP

namespace boost { namespace spirit {

    template <typename FirstT, typename RestT>
    struct subrule_list;

    template <int ID, typename DefT, typename ContextT>
    struct subrule_parser;

    namespace impl {

    #if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)

        template <int N, typename ListT>
        struct get_subrule;

        template <int N, typename ListT>
        struct get_subrule_chooser
        {
                static ListT t();
                static char test(nil_t);
                static int  test(...);

                //  Set value to
                //      0: ListT is empty
                //      1: ListT's first item has same ID
                //      2: ListT's first item has a different ID

                enum
                {
                id = ListT::first_t::id,
                is_same_id = N == id,
                    is_nil_t = sizeof(char) == sizeof(test(t())),
                    value = is_nil_t ? 0 : (is_same_id ? 1 : 2)
                };
          };

        template <int N>
        struct subrule_chooser;

        template <>
        struct subrule_chooser<0>
        {
            //  First case. ListT is empty

            template <int N, typename ListT>
            struct result
            { typedef nil_t type; };
        };

        template <>
        struct subrule_chooser<1>
        {
            //  Second case. ListT is non-empty and the list's
            //  first item has the ID we are looking for.

            template <int N, typename ListT>
            struct result
            { typedef typename ListT::first_t::def_t type; };
        };

        template <>
        struct subrule_chooser<2>
        {
            //  Third case. ListT is non-empty but the list's
            //  first item does not have the ID we are looking for.

            template <int N, typename ListT>
            struct result
            { typedef typename get_subrule<N, ListT::rest_t>::type type; };
        };

        template <int N, typename ListT>
        struct get_subrule
        {
            enum { n = get_subrule_chooser<N, ListT>::value };
            typedef typename subrule_chooser<n>::template
                result<N, ListT>::type type;
        };

    #else

        template <int N, typename ListT>
        struct get_subrule
        {
            //  First case. ListT is non-empty but the list's
            //  first item does not have the ID we are looking for.

            typedef typename get_subrule<N, typename ListT::rest_t>::type type;
        };

        template <int ID, typename DefT, typename ContextT, typename RestT>
        struct get_subrule<
            ID,
            subrule_list<
                subrule_parser<ID, DefT, ContextT>,
                RestT> >
        {
            //  Second case. ListT is non-empty and the list's
            //  first item has the ID we are looking for.

            typedef DefT type;
        };

        template <int ID>
        struct get_subrule<ID, nil_t>
        {
            //  Third case. ListT is empty
            typedef nil_t type;
        };

    #endif

        template <typename T1, typename T2>
        struct get_result_t {

        //  If the result type dictated by the context is nil_t (no closures
        //  present), then the whole subrule_parser return type is equal to
        //  the return type of the right hand side of this subrule_parser,
        //  otherwise it is equal to the dictated return value.

            typedef typename mpl::if_<
                boost::is_same<T1, nil_t>, T2, T1
            >::type type;
        };

        template <int ID, typename ScannerT, typename ContextResultT>
        struct get_subrule_result
        {
            typedef typename
                impl::get_subrule<ID, typename ScannerT::list_t>::type
            parser_t;

            typedef typename parser_result<parser_t, ScannerT>::type
            def_result_t;

            typedef typename match_result<ScannerT, ContextResultT>::type
            context_result_t;

            typedef typename get_result_t<context_result_t, def_result_t>::type
            type;
        };

        template <typename DefT, typename ScannerT, typename ContextResultT>
        struct get_subrule_parser_result
        {
            typedef typename parser_result<DefT, ScannerT>::type
            def_result_t;

            typedef typename match_result<ScannerT, ContextResultT>::type
            context_result_t;

            typedef typename get_result_t<context_result_t, def_result_t>::type
            type;
        };

        template <typename SubruleT, int ID>
        struct same_subrule_id
        {
            BOOST_STATIC_CONSTANT(bool, value = (SubruleT::id == ID));
        };

        template <typename RT, typename ScannerT, int ID>
        struct parse_subrule
        {
            template <typename ListT>
            static void
            do_parse(RT& r, ScannerT const& scan, ListT const& list, mpl::true_)
            {
                r = list.first.rhs.parse(scan);
            }

            template <typename ListT>
            static void
            do_parse(RT& r, ScannerT const& scan, ListT const& list, mpl::false_)
            {
                typedef typename ListT::rest_t::first_t subrule_t;
                mpl::bool_<same_subrule_id<subrule_t, ID>::value> same_id;
                do_parse(r, scan, list.rest, same_id);
            }

            static void
            do_(RT& r, ScannerT const& scan)
            {
                typedef typename ScannerT::list_t::first_t subrule_t;
                mpl::bool_<same_subrule_id<subrule_t, ID>::value> same_id;
                do_parse(r, scan, scan.list, same_id);
            }
        };

}}} // namespace boost::spirit::impl

#endif

