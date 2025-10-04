//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2021-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_SRC_LOCALE_MO_LAMBDA_HPP_INCLUDED
#define BOOST_SRC_LOCALE_MO_LAMBDA_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <memory>

namespace boost { namespace locale { namespace gnu_gettext { namespace lambda {

    struct BOOST_SYMBOL_VISIBLE expr {
        using value_type = long long;
        virtual value_type operator()(value_type n) const = 0;
        virtual ~expr() = default;
    };
    using expr_ptr = std::unique_ptr<expr>;

    class plural_expr {
        expr_ptr p_;

    public:
        plural_expr() = default;
        explicit plural_expr(expr_ptr p) : p_(std::move(p)) {}
        expr::value_type operator()(expr::value_type n) const { return (*p_)(n); }
        explicit operator bool() const { return static_cast<bool>(p_); }
    };

    BOOST_LOCALE_DECL plural_expr compile(const char* c_expression);

}}}} // namespace boost::locale::gnu_gettext::lambda

#endif
