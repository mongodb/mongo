#ifndef RESULT_HPP
#define RESULT_HPP

#include <new>
#include <type_traits>
#include <utility>

#include "utils/common.hpp"
#include "utils/error.hpp"
#include "utils/optional.hpp"
#include "options.hpp"
#include "logging.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    template<typename T, typename E, typename std::enable_if<!std::is_same<T, E>::value, int>::type = 0>
    class Result {
        using value_type = well_behaved<T>;
        union {
            value_type value_;
            E error_;
        };
        enum class member { value, error };
        member active;
        void destroy() {
            if(active == member::value) {
                value_.~value_type();
            } else {
                error_.~E();
            }
        }
    public:
        Result(value_type&& value) : value_(std::move(value)), active(member::value) {}
        Result(E&& error) : error_(std::move(error)), active(member::error) {
            log::debug("Error result constructed: {}", unwrap_error().what());
        }
        Result(const value_type& value) : value_(value_type(value)), active(member::value) {}
        Result(const E& error) : error_(E(error)), active(member::error) {
            log::debug("Error result constructed: {}", unwrap_error().what());
        }
        template<
            typename U = T,
            typename std::enable_if<
                !std::is_same<typename std::decay<U>::type, Result<T, E>>::value &&
                std::is_constructible<value_type, U>::value &&
                !std::is_constructible<E, U>::value,
                int
            >::type = 0
        >
        Result(U&& u) : Result(value_type(std::forward<U>(u))) {}
        Result(Result&& other) : active(other.active) {
            if(other.active == member::value) {
                new (&value_) value_type(std::move(other.value_));
            } else {
                new (&error_) E(std::move(other.error_));
            }
        }
        ~Result() {
            destroy();
        }
        Result& operator=(const Result& other) {
            if (this != &other) {
                destroy();
                if(other.active == member::value) {
                    new (&value_) value_type(other.value_);
                } else {
                    new (&error_) E(other.error_);
                }
                active = other.active;
            }
            return *this;
        }
        Result& operator=(Result&& other)
            noexcept(
                std::is_nothrow_move_constructible<value_type>::value && std::is_nothrow_move_constructible<E>::value
            )
        {
            if (this != &other) {
                destroy();
                if(other.active == member::value) {
                    new (&value_) value_type(std::move(other.value_));
                } else {
                    new (&error_) E(std::move(other.error_));
                }
                active = other.active;
            }
            return *this;
        }

        bool has_value() const {
            return active == member::value;
        }

        bool is_error() const {
            return active == member::error;
        }

        explicit operator bool() const {
            return has_value();
        }

        NODISCARD optional<T> value() const & {
            return has_value() ? optional<T>(value_) : nullopt;
        }

        NODISCARD optional<E> error() const & {
            return is_error() ? optional<E>(error_) : nullopt;
        }

        NODISCARD optional<T> value() && {
            return has_value() ? optional<T>(std::move(value_)) : nullopt;
        }

        NODISCARD optional<E> error() && {
            return is_error() ? optional<E>(std::move(error_)) : nullopt;
        }

        NODISCARD T& unwrap_value() & {
            ASSERT(has_value(), "Result does not contain a value");
            return value_;
        }

        NODISCARD const T& unwrap_value() const & {
            ASSERT(has_value(), "Result does not contain a value");
            return value_;
        }

        NODISCARD T unwrap_value() && {
            ASSERT(has_value(), "Result does not contain a value");
            return std::move(value_);
        }

        NODISCARD E& unwrap_error() & {
            ASSERT(is_error(), "Result does not contain an error");
            return error_;
        }

        NODISCARD const E& unwrap_error() const & {
            ASSERT(is_error(), "Result does not contain an error");
            return error_;
        }

        NODISCARD E unwrap_error() && {
            ASSERT(is_error(), "Result does not contain an error");
            return std::move(error_);
        }

        template<typename U>
        NODISCARD T value_or(U&& default_value) const & {
            return has_value() ? static_cast<T>(value_) : static_cast<T>(std::forward<U>(default_value));
        }

        template<typename U>
        NODISCARD T value_or(U&& default_value) && {
            return has_value() ? static_cast<T>(std::move(value_)) : static_cast<T>(std::forward<U>(default_value));
        }

        template<typename F>
        NODISCARD auto transform(F&& f) & -> Result<decltype(f(std::declval<T&>())), E> {
            if(has_value()) {
                return f(unwrap_value());
            } else {
                return unwrap_error();
            }
        }

        template<typename F>
        NODISCARD auto transform(F&& f) && -> Result<decltype(f(std::declval<T&&>())), E> {
            if(has_value()) {
                return f(std::move(unwrap_value()));
            } else {
                return unwrap_error();
            }
        }

        void drop_error() const {
            if(is_error()) {
                log::error(unwrap_error().what());
            }
        }
    };
}
CPPTRACE_END_NAMESPACE

#endif
