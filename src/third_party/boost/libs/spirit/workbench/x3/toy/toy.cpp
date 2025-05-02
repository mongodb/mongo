#include <iostream>
#include <utility>
#include <cstring>
#include <boost/mpl/identity.hpp>

namespace boost { namespace spirit { namespace x3
{
    template <typename Derived>
    struct parser
    {
        Derived const& derived() const
        {
            return *static_cast<Derived const*>(this);
        }
    };

    template <typename Char>
    struct char_parser : parser<char_parser<Char>>
    {
        char_parser(Char ch) : ch(ch) {}

        template <typename Iterator, typename Context>
        bool parse(Iterator& first, Iterator last, Context const& ctx) const
        {
            if (first != last && *first == ch)
            {
                ++first;
                return true;
            }
            return false;
        }

        Char ch;
    };

    template <typename Char>
    inline char_parser<Char> char_(Char ch)
    {
        return char_parser<Char>(ch);
    };

    template <typename Left, typename Right>
    struct sequence_parser : parser<sequence_parser<Left, Right>>
    {
        sequence_parser(Left left, Right right)
            : left(left), right(right) {}

        template <typename Iterator, typename Context>
        bool parse(Iterator& first, Iterator last, Context const& ctx) const
        {
            return left.parse(first, last, ctx)
                && right.parse(first, last, ctx);
        }

        Left left;
        Right right;
    };

    template <typename Left, typename Right>
    inline sequence_parser<Left, Right> operator>>(
        parser<Left> const& left, parser<Right> const& right)
    {
        return sequence_parser<Left, Right>(
            left.derived(), right.derived());
    }

    template <typename Left, typename Right>
    struct alternative_parser : parser<alternative_parser<Left, Right>>
    {
        alternative_parser(Left left, Right right)
            : left(left), right(right) {}

        template <typename Iterator, typename Context>
        bool parse(Iterator& first, Iterator last, Context const& ctx) const
        {
            if (left.parse(first, last, ctx))
                return true;
            return right.parse(first, last, ctx);
        }

        Left left;
        Right right;
    };

    template <typename Left, typename Right>
    inline alternative_parser<Left, Right> operator|(
        parser<Left> const& left, parser<Right> const& right)
    {
        return alternative_parser<Left, Right>(
            left.derived(), right.derived());
    }

    template <typename ID, typename T, typename NextContext>
    struct context
    {
        context(T const& val, NextContext const& next_ctx)
            : val(val), next_ctx(next_ctx) {}

        T const& get(mpl::identity<ID>) const
        {
            return val;
        }

        template <typename Identity>
        decltype(std::declval<NextContext>().get(Identity()))
        get(Identity id) const
        {
            return next_ctx.get(id);
        }

        T const& val;
        NextContext const& next_ctx;
    };

    struct empty_context
    {
        struct undefined {};
        template <typename ID>
        undefined get(ID) const
        {
            return undefined();
        }
    };

    template <typename ID, typename RHS>
    struct rule_definition : parser<rule_definition<ID, RHS>>
    {
        rule_definition(RHS rhs)
            : rhs(rhs) {}

        template <typename Iterator, typename Context>
        bool parse(Iterator& first, Iterator last, Context const& ctx) const
        {
            context<ID, RHS, Context> this_ctx(rhs,  ctx);
            return rhs.parse(first, last, this_ctx);
        }

        RHS rhs;
    };

    template <typename ID>
    struct rule : parser<rule<ID>>
    {
        template <typename Derived>
        rule_definition<ID, Derived>
        operator=(parser<Derived> const& definition) const
        {
            return rule_definition<ID, Derived>(definition.derived());
        }

        template <typename Iterator, typename Context>
        bool parse(Iterator& first, Iterator last, Context const& ctx) const
        {
            return ctx.get(mpl::identity<ID>()).parse(first, last, ctx);
        }
    };

    template <typename Iterator, typename Derived>
    inline bool parse(parser<Derived> const& p, Iterator& first, Iterator last)
    {
        empty_context ctx;
        return p.derived().parse(first, last, ctx);
    }

}}}

///////////////////////////////////////////////////////////////////////////////
// test code

template <typename Parser>
bool test_parse(Parser const& p, char const* in)
{
    return parse(p, in, in + std::strlen(in));
}

namespace parser
{
    using namespace boost::spirit::x3;

    namespace g_definition
    {
        auto const x = rule<class x>();
        auto const ax = char_('a') >> x;

        auto const g =
            x = char_('x') | ax;
    }
    using g_definition::g;
}

int main()
{

   { // a non-recursive parser
      using namespace boost::spirit::x3;

      auto abc = char_('a') >> char_('b') >> char_('c');
      std::cout << test_parse(abc, "abc") << std::endl;
      std::cout << test_parse(abc, "abx") << std::endl;
      std::cout << "==========================================" << std::endl;
   }

   { // a recursive rule
      using namespace boost::spirit::x3;

      auto const x = rule<class x>();
      auto const ax = char_('a') >> x;
      auto const start = (x = char_('x') | ax);

      std::cout << test_parse(start, "x") << std::endl;
      std::cout << test_parse(start, "ax") << std::endl;
      std::cout << test_parse(start, "aaaaax") << std::endl;
      std::cout << test_parse(start, "aaz") << std::endl;
      std::cout << "==========================================" << std::endl;
   }

   { // a grammar ( gcc and clang only: see http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2013/n3582.html )

      using namespace boost::spirit::x3;
      auto g = []()
      {
         rule<class x> x;
         auto ax = char_('a') >> x;
         return x = char_('x') | ax;

      }();

      std::cout << test_parse(g, "x") << std::endl;
      std::cout << test_parse(g, "ax") << std::endl;
      std::cout << test_parse(g, "aaaaax") << std::endl;
      std::cout << test_parse(g, "aaz") << std::endl;
      std::cout << "==========================================" << std::endl;
   }

   { // another grammar using namespaces (standard c++, see grammar g definition above in namespace parser.)
      using parser::g;

      std::cout << test_parse(g, "x") << std::endl;
      std::cout << test_parse(g, "ax") << std::endl;
      std::cout << test_parse(g, "aaaaax") << std::endl;
      std::cout << test_parse(g, "aaz") << std::endl;
      std::cout << "==========================================" << std::endl;
   }

   return 0;
}

