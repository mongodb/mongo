/*=============================================================================
    Copyright (c) 2017 wanghan02
    Copyright (c) 2024 Nana Sakisaka

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_X3_SUPPORT_EXPECTATION_HPP)
#define BOOST_SPIRIT_X3_SUPPORT_EXPECTATION_HPP

#if !defined(BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE)
# define BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE 1
#endif

#include <boost/config.hpp> // for BOOST_SYMBOL_VISIBLE, BOOST_ATTRIBUTE_NODISCARD
#include <boost/core/ignore_unused.hpp>
#include <boost/spirit/home/x3/support/unused.hpp>
#include <boost/spirit/home/x3/support/context.hpp>

// We utilize `x3::traits::build_optional<...>` for customization point
// instead of directly wrapping `expectation_failure` with `boost::optional`.
// This would make it possible for the user to eliminate the usages of
// `boost::optional<T>`, and use `std::optional<T>` everywhere.
//
// Note that we are intentionally including this header regardless of
// the value of BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE, since the
// helper types defined in non-throwing version might still be required
// when the users benchmark their application just by switching the
// macro while keeping their implementation unmodified.
//
// This will make it possible for the users to unconditionally
// inject `x3::expectation_failure_optional<Iterator>` into their parser,
// safely assuming that the value is no-op in throwing mode.
#include <boost/spirit/home/x3/support/traits/optional_traits.hpp>

// This is required for partial specialization of relevant helpers.
// TODO: Add a macro to discard all #includes of <boost/optional.hpp>.
//       (this is TODO because it requires changes in `optional_traits.hpp`.)
#include <boost/optional.hpp>
#include <optional>

#if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE // throwing mode
# define BOOST_SPIRIT_X3_EXPECTATION_FAILURE_API BOOST_SYMBOL_VISIBLE
# define BOOST_SPIRIT_X3_EXPECTATION_FAILURE_BASE : std::runtime_error
# define BOOST_SPIRIT_X3_EXPECTATION_FAILURE_NS throwing
# include <boost/throw_exception.hpp>
# include <stdexcept>

#else // non-throwing mode
# define BOOST_SPIRIT_X3_EXPECTATION_FAILURE_API
# define BOOST_SPIRIT_X3_EXPECTATION_FAILURE_BASE
# define BOOST_SPIRIT_X3_EXPECTATION_FAILURE_NS non_throwing
#endif

#include <string>
#include <type_traits>


namespace boost { namespace spirit { namespace x3
{
    struct expectation_failure_tag;

    inline namespace BOOST_SPIRIT_X3_EXPECTATION_FAILURE_NS
    {
        template <typename Iterator>
        struct BOOST_SPIRIT_X3_EXPECTATION_FAILURE_API
            expectation_failure BOOST_SPIRIT_X3_EXPECTATION_FAILURE_BASE
        {
        public:
            expectation_failure(Iterator where, std::string const& which)
            #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
                : std::runtime_error("boost::spirit::x3::expectation_failure"),
            #else
                :
            #endif
                where_(where), which_(which)
            {}

            BOOST_ATTRIBUTE_NODISCARD
            constexpr Iterator const& where() const noexcept { return where_; }

            BOOST_ATTRIBUTE_NODISCARD
            constexpr std::string const& which() const noexcept { return which_; }

        private:
            Iterator where_;
            std::string which_;
        };


        template <typename Context>
        using expectation_failure_t = std::remove_cv_t<std::remove_reference_t<
            decltype(x3::get<expectation_failure_tag>(std::declval<Context>()))>>;

        template <typename Iterator>
        using expectation_failure_optional =
            typename traits::build_optional<expectation_failure<Iterator>>::type;


        // x3::where(x), x3::which(x)
        // Convenient accessors for absorbing the variation of
        // optional/reference_wrapper interfaces.

        // beware ADL - we should avoid overgeneralization here.

        namespace expectation_failure_helpers
        {
            // bare type
            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) where(expectation_failure<Iterator> const& failure) noexcept { return failure.where(); }

            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) which(expectation_failure<Iterator> const& failure) noexcept { return failure.which(); }

            // std::optional
            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) where(std::optional<expectation_failure<Iterator>> const& failure) noexcept { return failure->where(); }

            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) which(std::optional<expectation_failure<Iterator>> const& failure) noexcept { return failure->which(); }

            // boost::optional
            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) where(boost::optional<expectation_failure<Iterator>> const& failure) noexcept { return failure->where(); }

            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) which(boost::optional<expectation_failure<Iterator>> const& failure) noexcept { return failure->which(); }

            // std::optional + std::reference_wrapper
            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) where(std::reference_wrapper<std::optional<expectation_failure<Iterator>>> const& failure) noexcept { return failure.get()->where(); }

            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) which(std::reference_wrapper<std::optional<expectation_failure<Iterator>>> const& failure) noexcept { return failure.get()->which(); }

            // boost::optional + std::reference_wrapper
            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) where(std::reference_wrapper<boost::optional<expectation_failure<Iterator>>> const& failure) noexcept { return failure.get()->where(); }

            template <typename Iterator>
            BOOST_ATTRIBUTE_NODISCARD
            constexpr decltype(auto) which(std::reference_wrapper<boost::optional<expectation_failure<Iterator>>> const& failure) noexcept { return failure.get()->which(); }
        } // expectation_failure_helpers

        using expectation_failure_helpers::where;
        using expectation_failure_helpers::which;

    } // inline namespace BOOST_SPIRIT_X3_EXPECTATION_FAILURE_NS

    #if !BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
    namespace detail {
        inline namespace BOOST_SPIRIT_X3_EXPECTATION_FAILURE_NS
        {
            inline constexpr bool has_expectation_failure_impl(unused_type const&) noexcept = delete;

            inline constexpr bool has_expectation_failure_impl(bool& failure) noexcept {
                return failure;
            }

            template <typename Iterator>
            constexpr bool has_expectation_failure_impl(std::optional<expectation_failure<Iterator>> const& failure) noexcept
            {
                return failure.has_value();
            }

            template <typename Iterator>
            constexpr bool has_expectation_failure_impl(boost::optional<expectation_failure<Iterator>> const& failure) noexcept
            {
                return failure.has_value();
            }

            template <typename T>
            constexpr bool has_expectation_failure_impl(std::reference_wrapper<T> const& ref) noexcept
            {
                return has_expectation_failure_impl(ref.get());
            }


            template <typename Iterator, typename T>
            constexpr void set_expectation_failure_impl(bool& failure, T&& value)
            {
                failure = std::forward<T>(value);
            }

            template <typename Iterator, typename T>
            constexpr void set_expectation_failure_impl(std::optional<expectation_failure<Iterator>>& failure, T&& value)
            {
                failure = std::forward<T>(value);
            }

            template <typename Iterator, typename T>
            constexpr void set_expectation_failure_impl(boost::optional<expectation_failure<Iterator>>& failure, T&& value)
            {
                failure = std::forward<T>(value);
            }

            template <typename AnyExpectationFailure, typename T>
            constexpr void set_expectation_failure_impl(std::reference_wrapper<AnyExpectationFailure>& failure, T&& value)
            {
                set_expectation_failure_impl(failure.get(), std::forward<T>(value));
            }


            template <typename Iterator>
            constexpr void clear_expectation_failure_impl(unused_type const&) noexcept = delete;

            template <typename Iterator>
            constexpr void clear_expectation_failure_impl(bool& failure) noexcept
            {
                failure = false;
            }

            template <typename Iterator>
            constexpr void clear_expectation_failure_impl(std::optional<expectation_failure<Iterator>>& failure) noexcept
            {
                failure.reset();
            }

            template <typename Iterator>
            constexpr void clear_expectation_failure_impl(boost::optional<expectation_failure<Iterator>>& failure) noexcept
            {
                failure.reset();
            }

            template <typename T>
            constexpr void clear_expectation_failure_impl(std::reference_wrapper<T>& ref) noexcept
            {
                return clear_expectation_failure_impl(ref.get());
            }
        } // inline namespace BOOST_SPIRIT_X3_EXPECTATION_FAILURE_NS
    } // detail
    #endif

    inline namespace BOOST_SPIRIT_X3_EXPECTATION_FAILURE_NS
    {
        template <typename Context>
        BOOST_ATTRIBUTE_NODISCARD
        constexpr bool has_expectation_failure(Context const& context) noexcept {
        #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
            boost::ignore_unused(context);
            return false;
        #else
            using T = expectation_failure_t<Context>;
            static_assert(
                !std::is_same_v<unused_type, T>,
                "Context type was not specified for x3::expectation_failure_tag. "
                "You probably forgot: `x3::with<x3::expectation_failure_tag>(failure)[p]`. "
                "Note that you must also bind the context to your skipper."
            );
            return detail::has_expectation_failure_impl(
                x3::get<expectation_failure_tag>(context));
        #endif
        }

        //
        // Creation of a brand new expectation_failure instance.
        // This is the primary overload.
        //
        template <typename Iterator, typename Subject, typename Context>
        constexpr void set_expectation_failure(
            Iterator const& where,
            Subject const& subject,
            Context const& context
        ) {
        #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
            boost::ignore_unused(where, subject, context);

        #else
            using T = expectation_failure_t<Context>;
            static_assert(
                !std::is_same_v<unused_type, T>,
                "Context type was not specified for x3::expectation_failure_tag. "
                "You probably forgot: `x3::with<x3::expectation_failure_tag>(failure)[p]`. "
                "Note that you must also bind the context to your skipper."
            );

            if constexpr (std::is_same_v<T, bool>)
            {
                boost::ignore_unused(where, subject);
                detail::set_expectation_failure_impl(
                    x3::get<expectation_failure_tag>(context),
                    true);
            }
            else
            {
                detail::set_expectation_failure_impl(
                    x3::get<expectation_failure_tag>(context),
                    expectation_failure<Iterator>(where, what(subject)));
            }
        #endif
        }

        //
        // Copy-assignment of existing expectation_failure instance.
        //
        // When you're in the situation where this functionality is
        // *really* needed, it essentially means that you have
        // multiple valid exceptions at the same time.
        //
        // There are only two decent situations that I can think of:
        //
        //   (a) When you are writing a custom parser procedure with very specific characteristics:
        //       1. You're forking a context.
        //       2. Your parser class has delegated some process to child parser(s).
        //       3. The child parser(s) have raised an exceptation_failure.
        //       4. You need to propagate the failure back to the parent context.
        //
        //   (b) When you truly need a nested exception.
        //       That is, you're trying to preserve a nested exception structure
        //       raised by nested directive: e.g. `x3::expect[x3::expect[p]]`.
        //       Note that all builtin primitives just save the first error,
        //       so this structure does not exist in core (as of now).
        //
        template <typename AnyExpectationFailure, typename Context>
        constexpr void set_expectation_failure(
            AnyExpectationFailure const& existing_failure,
            Context const& context
        ) {
        #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
            boost::ignore_unused(existing_failure, context);

        #else
            using T = expectation_failure_t<Context>;
            static_assert(
                !std::is_same_v<T, unused_type>,
                "Context type was not specified for x3::expectation_failure_tag. "
                "You probably forgot: `x3::with<x3::expectation_failure_tag>(failure)[p]`. "
                "Note that you must also bind the context to your skipper."
            );

            static_assert(
                std::is_assignable_v<T, AnyExpectationFailure const&>,
                "previous/current expectation failure types should be compatible"
            );

            detail::set_expectation_failure_impl(
                x3::get<expectation_failure_tag>(context), existing_failure);
        #endif
        }

    #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
        template <typename Context>
        constexpr decltype(auto) get_expectation_failure(Context const&) = delete;

    #else
        template <typename Context>
        BOOST_ATTRIBUTE_NODISCARD
        constexpr decltype(auto) get_expectation_failure(Context const& context)
        {
            using T = expectation_failure_t<Context>;
            static_assert(
                !std::is_same_v<T, unused_type>,
                "Context type was not specified for x3::expectation_failure_tag. "
                "You probably forgot: `x3::with<x3::expectation_failure_tag>(failure)[p]`. "
                "Note that you must also bind the context to your skipper."
            );

            return x3::get<expectation_failure_tag>(context);
        }
    #endif

        template <typename Context>
        constexpr void clear_expectation_failure(Context const& context) noexcept
        {
        #if BOOST_SPIRIT_X3_THROW_EXPECTATION_FAILURE
            boost::ignore_unused(context);
        #else
            using T = expectation_failure_t<Context>;
            static_assert(
                !std::is_same_v<T, unused_type>,
                "Context type was not specified for x3::expectation_failure_tag. "
                "You probably forgot: `x3::with<x3::expectation_failure_tag>(failure)[p]`. "
                "Note that you must also bind the context to your skipper."
            );
            detail::clear_expectation_failure_impl(
                x3::get<expectation_failure_tag>(context));
        #endif
        }

    } // inline namespace BOOST_SPIRIT_X3_EXPECTATION_FAILURE_NS
}}}

#endif
