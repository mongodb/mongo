//
// Copyright (c) 2023-2025 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_DETAIL_ANY_STRING_HPP_INCLUDED
#define BOOST_LOCALE_DETAIL_ANY_STRING_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <boost/assert.hpp>
#include <boost/core/detail/string_view.hpp>
#include <memory>
#include <stdexcept>
#include <string>

/// \cond INTERNAL
namespace boost { namespace locale { namespace detail {
    /// Type-erased std::basic_string
    class any_string {
        struct BOOST_SYMBOL_VISIBLE base {
            virtual ~base() = default;
            virtual base* clone() const = 0;

        protected:
            base() = default;
            base(const base&) = default;
            base(base&&) = delete;
            base& operator=(const base&) = default;
            base& operator=(base&&) = delete;
        };
        template<typename Char>
        struct BOOST_SYMBOL_VISIBLE impl : base {
            explicit impl(const core::basic_string_view<Char> value) : s(value) {}
            impl* clone() const override { return new impl(*this); }
            std::basic_string<Char> s;
        };

        std::unique_ptr<const base> s_;

    public:
        any_string() = default;
        any_string(const any_string& other) : s_(other.s_ ? other.s_->clone() : nullptr) {}
        any_string(any_string&&) = default;
        any_string& operator=(any_string other) // Covers the copy and move assignment
        {
            s_.swap(other.s_);
            return *this;
        }

        template<typename Char>
        void set(const core::basic_string_view<Char> s)
        {
            BOOST_ASSERT(!s.empty());
            s_.reset(new impl<Char>(s));
        }

        template<typename Char>
        std::basic_string<Char> get() const
        {
            if(!s_)
                throw std::bad_cast();
            return dynamic_cast<const impl<Char>&>(*s_).s;
        }
    };

}}} // namespace boost::locale::detail

/// \endcond

#endif
