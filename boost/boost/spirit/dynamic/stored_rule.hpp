/*=============================================================================
    Copyright (c) 1998-2003 Joel de Guzman
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_STORED_RULE_HPP)
#define BOOST_SPIRIT_STORED_RULE_HPP

///////////////////////////////////////////////////////////////////////////////
#include <boost/spirit/core/non_terminal/impl/rule.ipp>
#include <boost/spirit/dynamic/rule_alias.hpp>
#include <boost/shared_ptr.hpp>

#include <boost/spirit/dynamic/stored_rule_fwd.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace boost { namespace spirit {

    ///////////////////////////////////////////////////////////////////////////
    //
    //  stored_rule class
    //
    ///////////////////////////////////////////////////////////////////////////
    template <
        typename T0 
      , typename T1
      , typename T2
      , bool EmbedByValue
    >
    class stored_rule
        : public impl::rule_base<
            stored_rule<T0, T1, T2, EmbedByValue>
          , typename mpl::if_c<
                EmbedByValue
              , stored_rule<T0, T1, T2, true>
              , stored_rule<T0, T1, T2> const&>::type
          , T0, T1, T2>
    {
    public:

        typedef stored_rule<T0, T1, T2, EmbedByValue> self_t;
        typedef impl::rule_base<
            self_t
          , typename mpl::if_c<
                EmbedByValue
              , stored_rule<T0, T1, T2, true>
              , self_t const&>::type
          , T0, T1, T2>
        base_t;

        typedef typename base_t::scanner_t scanner_t;
        typedef typename base_t::attr_t attr_t;
        typedef impl::abstract_parser<scanner_t, attr_t> abstract_parser_t;
        typedef rule_alias<self_t> alias_t;

        stored_rule() : ptr() {}
        ~stored_rule() {}

        stored_rule(stored_rule const& r)
        : ptr(r.ptr) {}

        template <typename ParserT>
        stored_rule(ParserT const& p)
        : ptr(new impl::concrete_parser<ParserT, scanner_t, attr_t>(p)) {}

        template <typename ParserT>
        stored_rule& operator=(ParserT const& p)
        {
            ptr.reset(new impl::concrete_parser<ParserT, scanner_t, attr_t>(p));
            return *this;
        }

        stored_rule& operator=(stored_rule const& r)
        {
            //  If this is placed above the templatized assignment
            //  operator, VC6 incorrectly complains ambiguity with
            //  r1 = r2, where r1 and r2 are both rules.
            ptr = r.ptr;
            return *this;
        }

        stored_rule<T0, T1, T2, true>
        copy() const
        {
            return stored_rule<T0, T1, T2, true>(ptr);
        }

        alias_t
        alias() const
        {
            return alias_t(*this);
        }

    private:

        friend class impl::rule_base_access;
        friend class stored_rule<T0, T1, T2, !EmbedByValue>;

#if defined(__GNUC__) && (__GNUC__ < 3)
    public:
#endif
        abstract_parser_t*
        get() const
        {
            return ptr.get();
        }
#if defined(__GNUC__) && (__GNUC__ < 3)
    private:
#endif

        stored_rule(shared_ptr<abstract_parser_t> const& ptr)
        : ptr(ptr) {}

        shared_ptr<abstract_parser_t> ptr;
    };

///////////////////////////////////////////////////////////////////////////////
}} // namespace boost::spirit

#endif
