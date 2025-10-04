#ifndef OPTIONAL_HPP
#define OPTIONAL_HPP

#include <functional>
#include <new>
#include <type_traits>
#include <utility>

#include "utils/common.hpp"
#include "utils/error.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    struct nullopt_t {};

    static constexpr nullopt_t nullopt;

    template<
        typename T,
        typename std::enable_if<
            !std::is_same<typename std::decay<T>::type, void>::value && !std::is_rvalue_reference<T>::value,
            int
        >::type = 0
    >
    using well_behaved = typename std::conditional<
        std::is_reference<T>::value, std::reference_wrapper<typename std::remove_reference<T>::type>, T
    >::type;

    template<
        typename T,
        typename std::enable_if<!std::is_same<typename std::decay<T>::type, void>::value, int>::type = 0
    >
    class optional {
        using value_type = well_behaved<T>;

        union {
            char x{};
            value_type uvalue;
        };

        bool holds_value = false;

    public:
        optional() noexcept {}

        optional(nullopt_t) noexcept {}

        ~optional() {
            reset();
        }

        optional(const optional& other) : holds_value(other.holds_value) {
            if(holds_value) {
                new (static_cast<void*>(std::addressof(uvalue))) value_type(other.uvalue);
            }
        }

        optional(optional&& other)
            noexcept(std::is_nothrow_move_constructible<value_type>::value)
            : holds_value(other.holds_value)
        {
            if(holds_value) {
                new (static_cast<void*>(std::addressof(uvalue))) value_type(std::move(other.uvalue));
            }
        }

        optional& operator=(const optional& other) {
            optional copy(other);
            swap(copy);
            return *this;
        }

        optional& operator=(optional&& other)
            noexcept(std::is_nothrow_move_assignable<value_type>::value && std::is_nothrow_move_constructible<value_type>::value)
        {
            if (this != &other) {
                reset();
                if (other.holds_value) {
                    new (static_cast<void*>(std::addressof(uvalue))) value_type(std::move(other.uvalue));
                    holds_value = true;
                }
            }
            return *this;
        }

        template<
            typename U = T,
            typename std::enable_if<!std::is_same<typename std::decay<U>::type, optional<T>>::value, int>::type = 0
        >
        optional(U&& value) : holds_value(true) {
            new (static_cast<void*>(std::addressof(uvalue))) value_type(std::forward<U>(value));
        }

        template<
            typename U = T,
            typename std::enable_if<!std::is_same<typename std::decay<U>::type, optional<T>>::value, int>::type = 0
        >
        optional& operator=(U&& value) {
            optional o(std::forward<U>(value));
            swap(o);
            return *this;
        }

        optional& operator=(nullopt_t) noexcept {
            reset();
            return *this;
        }

        void swap(optional& other) noexcept {
            if(holds_value && other.holds_value) {
                std::swap(uvalue, other.uvalue);
            } else if(holds_value && !other.holds_value) {
                new (&other.uvalue) value_type(std::move(uvalue));
                uvalue.~value_type();
            } else if(!holds_value && other.holds_value) {
                new (static_cast<void*>(std::addressof(uvalue))) value_type(std::move(other.uvalue));
                other.uvalue.~value_type();
            }
            std::swap(holds_value, other.holds_value);
        }

        bool has_value() const {
            return holds_value;
        }

        explicit operator bool() const {
            return holds_value;
        }

        void reset() {
            if(holds_value) {
                uvalue.~value_type();
            }
            holds_value = false;
        }

        NODISCARD T& unwrap() & {
            ASSERT(holds_value, "Optional does not contain a value");
            return uvalue;
        }

        NODISCARD const T& unwrap() const & {
            ASSERT(holds_value, "Optional does not contain a value");
            return uvalue;
        }

        NODISCARD T&& unwrap() && {
            ASSERT(holds_value, "Optional does not contain a value");
            return std::move(uvalue);
        }

        NODISCARD const T&& unwrap() const && {
            ASSERT(holds_value, "Optional does not contain a value");
            return std::move(uvalue);
        }

        template<typename U>
        NODISCARD T value_or(U&& default_value) const & {
            return holds_value ? static_cast<T>(uvalue) : static_cast<T>(std::forward<U>(default_value));
        }

        template<typename U>
        NODISCARD T value_or(U&& default_value) && {
            return holds_value ? static_cast<T>(std::move(uvalue)) : static_cast<T>(std::forward<U>(default_value));
        }
    };
}
CPPTRACE_END_NAMESPACE

#endif
