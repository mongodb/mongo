#ifndef SPAN_HPP
#define SPAN_HPP

#include "utils/utils.hpp"

#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    // basic span implementation
    // I haven't implemented most members because I don't need them, more will be added as needed

    template<typename T>
    class span {
        T* ptr;
        std::size_t count;

    public:
        using element_type = T;
        using value_type = typename std::remove_cv<T>::type;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using const_pointer = const T*;
        using reference = T&;
        using const_reference = const T&;
        using iterator = T*;
        using const_iterator = const T*;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<reverse_iterator>;
        using i_am_span = void;

        span() : ptr(nullptr), count(0) {}
        span(T* ptr, std::size_t count) : ptr(ptr), count(count) {}
        template<typename It>
        span(It begin, It end) : ptr(std::addressof(*begin)), count(end - begin) {}

        T* data() const noexcept {
            return ptr;
        }
        std::size_t size() const noexcept {
            return count;
        }
        bool empty() const noexcept {
            return count == 0;
        }

        iterator begin() noexcept {
            return ptr;
        }
        iterator end() noexcept {
            return ptr + count;
        }
        const_iterator begin() const noexcept {
            return ptr;
        }
        const_iterator end() const noexcept {
            return ptr + count;
        }
    };

    using bspan = span<char>;
    using cbspan = span<const char>;

    template<typename T, typename = int>
    struct is_span : std::false_type {};

    template<typename T>
    struct is_span<T, void_t<typename T::i_am_span>> : std::true_type {};

    template<typename It>
    auto make_span(It begin, It end) -> span<typename std::remove_reference<decltype(*begin)>::type> {
        return {begin, end};
    }

    template<typename It>
    auto make_span(It begin, std::size_t count) -> span<typename std::remove_reference<decltype(*begin)>::type> {
        return {begin, count};
    }

    template<
        typename T,
        typename std::enable_if<
            std::is_standard_layout<T>::value && is_trivially_copyable<T>::value && !is_span<T>::value,
            int
        >::type = 0
    >
    span<char> make_bspan(T& object) {
        return span<char>(reinterpret_cast<char*>(std::addressof(object)), sizeof(object));
    }
}
CPPTRACE_END_NAMESPACE

#endif
