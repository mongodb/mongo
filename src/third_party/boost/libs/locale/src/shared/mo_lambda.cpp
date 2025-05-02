//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2021-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "mo_lambda.hpp"
#include <boost/assert.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>

#ifdef BOOST_MSVC
#    pragma warning(disable : 4512) // assignment operator could not be generated
#endif

namespace boost { namespace locale { namespace gnu_gettext { namespace lambda {

    namespace { // anon
        template<class TExp, typename... Ts>
        expr_ptr make_expr(Ts&&... ts)
        {
            return expr_ptr(new TExp(std::forward<Ts>(ts)...));
        }

        struct identity final : expr {
            value_type operator()(value_type n) const override { return n; }
        };

        using sub_expr_type = plural_expr;

        template<class Functor>
        struct unary final : expr, Functor {
            unary(expr_ptr p) : op1(std::move(p)) {}
            value_type operator()(value_type n) const override { return Functor::operator()(op1(n)); }

        protected:
            sub_expr_type op1;
        };

        template<class Functor, bool returnZeroOnZero2ndArg = false>
        struct binary final : expr, Functor {
            binary(expr_ptr p1, expr_ptr p2) : op1(std::move(p1)), op2(std::move(p2)) {}
            value_type operator()(value_type n) const override
            {
                const auto v1 = op1(n);
                const auto v2 = op2(n);
                BOOST_LOCALE_START_CONST_CONDITION
                if(returnZeroOnZero2ndArg && v2 == 0)
                    return 0;
                BOOST_LOCALE_END_CONST_CONDITION
                return Functor::operator()(v1, v2);
            }

        protected:
            sub_expr_type op1, op2;
        };

        struct number final : expr {
            number(value_type v) : val(v) {}
            value_type operator()(value_type /*n*/) const override { return val; }

        private:
            value_type val;
        };

        struct conditional final : public expr {
            conditional(expr_ptr p1, expr_ptr p2, expr_ptr p3) :
                op1(std::move(p1)), op2(std::move(p2)), op3(std::move(p3))
            {}
            value_type operator()(value_type n) const override { return op1(n) ? op2(n) : op3(n); }

        private:
            sub_expr_type op1, op2, op3;
        };

        using token_t = int;
        enum : token_t { END = 0, GTE = 256, LTE, EQ, NEQ, AND, OR, NUM, VARIABLE };

        expr_ptr bin_factory(const token_t value, expr_ptr left, expr_ptr right)
        {
#define BINOP_CASE(match, cls) \
    case match: return make_expr<cls>(std::move(left), std::move(right))
            // Special cases: Avoid division by zero
            using divides = binary<std::divides<expr::value_type>, true>;
            using modulus = binary<std::modulus<expr::value_type>, true>;
            switch(value) {
                BINOP_CASE('/', divides);
                BINOP_CASE('*', binary<std::multiplies<expr::value_type>>);
                BINOP_CASE('%', modulus);
                BINOP_CASE('+', binary<std::plus<expr::value_type>>);
                BINOP_CASE('-', binary<std::minus<expr::value_type>>);
                BINOP_CASE('>', binary<std::greater<expr::value_type>>);
                BINOP_CASE('<', binary<std::less<expr::value_type>>);
                BINOP_CASE(GTE, binary<std::greater_equal<expr::value_type>>);
                BINOP_CASE(LTE, binary<std::less_equal<expr::value_type>>);
                BINOP_CASE(EQ, binary<std::equal_to<expr::value_type>>);
                BINOP_CASE(NEQ, binary<std::not_equal_to<expr::value_type>>);
                BINOP_CASE(AND, binary<std::logical_and<expr::value_type>>);
                BINOP_CASE(OR, binary<std::logical_or<expr::value_type>>);
                default: throw std::logic_error("Unexpected binary operator"); // LCOV_EXCL_LINE
            }
#undef BINOP_CASE
        }

        template<size_t size>
        bool is_in(const token_t token, const token_t (&tokens)[size])
        {
            for(const auto el : tokens) {
                if(token == el)
                    return true;
            }
            return false;
        }

        class tokenizer {
        public:
            tokenizer(const char* s) : text_(s), next_tocken_(0), numeric_value_(0) { step(); }
            token_t get(long long* val = nullptr)
            {
                const token_t res = next(val);
                step();
                return res;
            }
            token_t next(long long* val = nullptr) const
            {
                if(val && next_tocken_ == NUM)
                    *val = numeric_value_;
                return next_tocken_;
            }

        private:
            const char* text_;
            token_t next_tocken_;
            long long numeric_value_;

            static constexpr bool is_blank(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
            static constexpr bool is_digit(char c) { return '0' <= c && c <= '9'; }
            template<size_t size>
            static bool is(const char* s, const char (&search)[size])
            {
                return strncmp(s, search, size - 1) == 0;
            }
            void step()
            {
                while(is_blank(*text_))
                    text_++;
                const char* text = text_;
                if(is(text, "&&")) {
                    text_ += 2;
                    next_tocken_ = AND;
                } else if(is(text, "||")) {
                    text_ += 2;
                    next_tocken_ = OR;
                } else if(is(text, "<=")) {
                    text_ += 2;
                    next_tocken_ = LTE;
                } else if(is(text, ">=")) {
                    text_ += 2;
                    next_tocken_ = GTE;
                } else if(is(text, "==")) {
                    text_ += 2;
                    next_tocken_ = EQ;
                } else if(is(text, "!=")) {
                    text_ += 2;
                    next_tocken_ = NEQ;
                } else if(*text == 'n') {
                    text_++;
                    next_tocken_ = VARIABLE;
                } else if(is_digit(*text)) {
                    char* tmp_ptr;
                    // strtoll not always available -> parse as unsigned long
                    const auto value = std::strtoul(text, &tmp_ptr, 10);
                    // Saturate in case long=long long
                    numeric_value_ = std::min<unsigned long long>(std::numeric_limits<long long>::max(), value);
                    text_ = tmp_ptr;
                    next_tocken_ = NUM;
                } else if(*text == '\0')
                    next_tocken_ = END;
                else {
                    next_tocken_ = *text;
                    text_++;
                }
            }
        };

        constexpr token_t level6[] = {'*', '/', '%'};
        constexpr token_t level5[] = {'+', '-'};
        constexpr token_t level4[] = {'<', '>', GTE, LTE};
        constexpr token_t level3[] = {EQ, NEQ};
        constexpr token_t level2[] = {AND};
        constexpr token_t level1[] = {OR};

        class parser {
        public:
            parser(const char* str) : t(str) {}

            expr_ptr compile()
            {
                expr_ptr res = cond_expr();
                if(res && t.next() != END)
                    return expr_ptr();
                return res;
            }

        private:
            expr_ptr value_expr()
            {
                expr_ptr op;
                if(t.next() == '(') {
                    t.get();
                    if(!(op = cond_expr()))
                        return expr_ptr();
                    if(t.get() != ')')
                        return expr_ptr();
                    return op;
                } else if(t.next() == NUM) {
                    expr::value_type value;
                    t.get(&value);
                    return make_expr<number>(value);
                } else if(t.next() == VARIABLE) {
                    t.get();
                    return make_expr<identity>();
                }
                return expr_ptr();
            }

            expr_ptr unary_expr()
            {
                constexpr token_t level_unary[] = {'!', '-'};
                if(is_in(t.next(), level_unary)) {
                    const token_t op = t.get();
                    expr_ptr op1 = unary_expr();
                    if(!op1)
                        return expr_ptr();
                    if(BOOST_LIKELY(op == '!'))
                        return make_expr<unary<std::logical_not<expr::value_type>>>(std::move(op1));
                    else {
                        BOOST_ASSERT(op == '-');
                        return make_expr<unary<std::negate<expr::value_type>>>(std::move(op1));
                    }
                } else
                    return value_expr();
            }

#define BINARY_EXPR(lvl, nextLvl, list)                           \
    expr_ptr lvl()                                                \
    {                                                             \
        expr_ptr op1 = nextLvl();                                 \
        if(!op1)                                                  \
            return expr_ptr();                                    \
        while(is_in(t.next(), list)) {                            \
            const token_t o = t.get();                            \
            expr_ptr op2 = nextLvl();                             \
            if(!op2)                                              \
                return expr_ptr();                                \
            op1 = bin_factory(o, std::move(op1), std::move(op2)); \
        }                                                         \
        return op1;                                               \
    }

            BINARY_EXPR(l6, unary_expr, level6);
            BINARY_EXPR(l5, l6, level5);
            BINARY_EXPR(l4, l5, level4);
            BINARY_EXPR(l3, l4, level3);
            BINARY_EXPR(l2, l3, level2);
            BINARY_EXPR(l1, l2, level1);
#undef BINARY_EXPR

            expr_ptr cond_expr()
            {
                expr_ptr cond;
                if(!(cond = l1()))
                    return expr_ptr();
                if(t.next() != '?')
                    return cond;
                t.get();
                expr_ptr case1, case2;
                if(!(case1 = cond_expr()))
                    return expr_ptr();
                if(t.get() != ':')
                    return expr_ptr();
                if(!(case2 = cond_expr()))
                    return expr_ptr();
                return make_expr<conditional>(std::move(cond), std::move(case1), std::move(case2));
            }

            tokenizer t;
        };

    } // namespace

    plural_expr compile(const char* str)
    {
        parser p(str);
        return plural_expr(p.compile());
    }

}}}} // namespace boost::locale::gnu_gettext::lambda
