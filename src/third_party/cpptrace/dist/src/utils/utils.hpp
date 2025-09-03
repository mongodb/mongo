#ifndef UTILS_HPP
#define UTILS_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "utils/common.hpp"
#include "utils/error.hpp"
#include "utils/optional.hpp"
#include "utils/result.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    bool isatty(int fd);
    int fileno(std::FILE* stream);

    void enable_virtual_terminal_processing_if_needed() noexcept;

    inline std::vector<std::string> split(const std::string& str, const std::string& delims) {
        std::vector<std::string> vec;
        std::size_t old_pos = 0;
        std::size_t pos = 0;
        while((pos = str.find_first_of(delims, old_pos)) != std::string::npos) {
            vec.emplace_back(str.substr(old_pos, pos - old_pos));
            old_pos = pos + 1;
        }
        vec.emplace_back(str.substr(old_pos));
        return vec;
    }

    template<typename C>
    std::string join(const C& container, const std::string& delim) {
        auto iter = std::begin(container);
        auto end = std::end(container);
        std::string str;
        if(std::distance(iter, end) > 0) {
            str += *iter;
            while(++iter != end) {
                str += delim;
                str += *iter;
            }
        }
        return str;
    }

    // closest value in a sorted range such that *it <= value
    template<typename ForwardIt, typename T>
    ForwardIt first_less_than_or_equal(ForwardIt begin, ForwardIt end, const T& value) {
        auto it = std::upper_bound(begin, end, value);
        // it is first > value, we want first <= value
        if(it != begin) {
            return --it;
        }
        return end;
    }

    // closest value in a sorted range such that *it <= value
    template<typename ForwardIt, typename T, typename Compare>
    ForwardIt first_less_than_or_equal(ForwardIt begin, ForwardIt end, const T& value, Compare compare) {
        auto it = std::upper_bound(begin, end, value, compare);
        // it is first > value, we want first <= value
        if(it != begin) {
            return --it;
        }
        return end;
    }

    constexpr const char* const whitespace = " \t\n\r\f\v";

    inline std::string trim(const std::string& str) {
        if(str.empty()) {
            return "";
        }
        const std::size_t left = str.find_first_not_of(whitespace);
        const std::size_t right = str.find_last_not_of(whitespace) + 1;
        return str.substr(left, right - left);
    }

    inline bool starts_with(const std::string& str, const std::string& prefix) {
        return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
    }

    inline bool is_little_endian() {
        std::uint16_t num = 0x1;
        const auto* ptr = (std::uint8_t*)&num;
        return ptr[0] == 1;
    }

    // Modified from
    // https://stackoverflow.com/questions/105252/how-do-i-convert-between-big-endian-and-little-endian-values-in-c
    template<typename T, std::size_t N>
    struct byte_swapper;

    template<typename T>
    struct byte_swapper<T, 1> {
        T operator()(T val) {
            return val;
        }
    };

    template<typename T>
    struct byte_swapper<T, 2> {
        T operator()(T val) {
            return (((val >> 8) & 0xff) | ((val & 0xff) << 8));
        }
    };

    template<typename T>
    struct byte_swapper<T, 4> {
        T operator()(T val) {
            return (((val & 0xff000000) >> 24) |
                    ((val & 0x00ff0000) >>  8) |
                    ((val & 0x0000ff00) <<  8) |
                    ((val & 0x000000ff) << 24));
        }
    };

    template<typename T>
    struct byte_swapper<T, 8> {
        T operator()(T val) {
            return (((val & 0xff00000000000000ULL) >> 56) |
                    ((val & 0x00ff000000000000ULL) >> 40) |
                    ((val & 0x0000ff0000000000ULL) >> 24) |
                    ((val & 0x000000ff00000000ULL) >> 8 ) |
                    ((val & 0x00000000ff000000ULL) << 8 ) |
                    ((val & 0x0000000000ff0000ULL) << 24) |
                    ((val & 0x000000000000ff00ULL) << 40) |
                    ((val & 0x00000000000000ffULL) << 56));
        }
    };

    template<typename T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    T byteswap(T value) {
        return byte_swapper<T, sizeof(T)>{}(value);
    }

    template<
        typename T,
        typename std::enable_if<std::is_arithmetic<T>::value && !std::is_signed<T>::value, int>::type = 0
    >
    constexpr bool is_positive_power_of_two(T value) {
        return (value != 0) && (value & (value - 1)) == 0;
    }

    template<
        typename T,
        typename std::enable_if<std::is_arithmetic<T>::value && std::is_signed<T>::value, int>::type = 0
    >
    bool is_positive_power_of_two(T value) {
        if(value < 0) {
            return false;
        }
        return is_positive_power_of_two(static_cast<typename std::make_unsigned<T>::type>(value));
    }

    constexpr unsigned n_digits(unsigned value) noexcept {
        return value < 10 ? 1 : 1 + n_digits(value / 10);
    }

    #if defined(__GNUC__) && (__GNUC__ < 5) && !defined(__clang__)
    template<typename T>
    using is_trivially_copyable = std::is_trivial<T>;
    #else
    template<typename T>
    using is_trivially_copyable = std::is_trivially_copyable<T>;
    #endif

    // TODO: Re-evaluate use of off_t
    template<
        typename T,
        typename std::enable_if<
            std::is_standard_layout<T>::value && is_trivially_copyable<T>::value,
            int
        >::type = 0
    >
    Result<T, internal_error> load_bytes(std::FILE* object_file, off_t offset) {
        T object;
        if(std::fseek(object_file, offset, SEEK_SET) != 0) {
            return internal_error("fseek error");
        }
        if(std::fread(&object, sizeof(T), 1, object_file) != 1) {
            return internal_error("fread error");
        }
        return object;
    }

    // shamelessly stolen from stackoverflow
    bool directory_exists(cstring_view path);

    inline std::string basename(cstring_view path, bool maybe_windows = false) {
        // Assumes no trailing /'s
        auto pos = path.find_last_of(maybe_windows ? "/\\" : "/");
        if(pos == cstring_view::npos) {
            return std::string(path);
        } else {
            return std::string(path.substr(pos + 1));
        }
    }

    // A way to cast to unsigned long long without "warning: useless cast to type"
    template<typename T>
    unsigned long long to_ull(T t) {
        return static_cast<unsigned long long>(t);
    }
    template<typename T>
    frame_ptr to_frame_ptr(T t) {
        return static_cast<frame_ptr>(t);
    }

    // A way to cast to U without "warning: useless cast to type"
    template<typename U, typename V>
    U to(V v) {
        return static_cast<U>(v);
    }

    template<typename T, typename U = T>
    T exchange(T& obj, U&& value) {
        T old = std::move(obj);
        obj = std::forward<U>(value);
        return old;
    }

    template<typename...>
    using void_t = void;

    struct monostate {};

    // TODO: Rework some stuff here. Not sure deleters should be optional or moved.
    // Also allow file_wrapper file = std::fopen(object_path.c_str(), "rb");
    template<
        typename T,
        typename D,
        // Note: Previously checked if D was invocable and returned void but this kept causing problems for MSVC
        //  == 19.38-specific msvc bug https://developercommunity.visualstudio.com/t/MSVC-1938331290-preview-fails-to-comp/10505565
        //  <= 19.23 msvc also appears to fail (but for a different reason https://godbolt.org/z/6Y5EvdWPK)
        //  <= 19.39 msvc also has trouble with it for different reasons https://godbolt.org/z/aPPPT7z3z
        typename std::enable_if<
            std::is_standard_layout<T>::value && is_trivially_copyable<T>::value,
            int
        >::type = 0,
        typename std::enable_if<
            std::is_nothrow_move_constructible<T>::value,
            int
        >::type = 0
    >
    class raii_wrapper {
        T obj;
        optional<D> deleter;
    public:
        raii_wrapper(T obj, D deleter) : obj(obj), deleter(deleter) {}
        raii_wrapper(raii_wrapper&& other) noexcept : obj(std::move(other.obj)), deleter(std::move(other.deleter)) {
            other.deleter = nullopt;
        }
        raii_wrapper(const raii_wrapper&) = delete;
        raii_wrapper& operator=(raii_wrapper&&) = delete;
        raii_wrapper& operator=(const raii_wrapper&) = delete;
        ~raii_wrapper() {
            if(deleter.has_value()) {
                deleter.unwrap()(obj);
            }
        }
        operator T&() {
            return obj;
        }
        operator const T&() const {
            return obj;
        }
        T& get() {
            return obj;
        }
        const T& get() const {
            return obj;
        }
    };

    template<typename T, typename D>
    raii_wrapper<typename std::remove_reference<T>::type, D> raii_wrap(T obj, D deleter) {
        return raii_wrapper<typename std::remove_reference<T>::type, D>(obj, deleter);
    }

    inline void file_deleter(std::FILE* ptr) {
        if(ptr) {
            fclose(ptr);
        }
    }

    using file_wrapper = raii_wrapper<std::FILE*, void(*)(std::FILE*)>;

    template<typename T, typename... Args>
    auto make_unique(Args&&... args) -> typename std::enable_if<!std::is_array<T>::value, std::unique_ptr<T>>::type {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }

    template<typename T>
    auto make_unique(T&& arg) -> typename std::enable_if<!std::is_array<T>::value, std::unique_ptr<T>>::type {
        return std::unique_ptr<T>(new T(std::forward<T>(arg)));
    }

    template<typename T>
    class maybe_owned {
        std::unique_ptr<T> owned;
        T* ptr;
    public:
        maybe_owned(T* ptr) : ptr(ptr) {}
        maybe_owned(std::unique_ptr<T>&& owned) : owned(std::move(owned)), ptr(this->owned.get()) {}
        T* operator->() {
            return ptr;
        }
        T& operator*() {
            return *ptr;
        }
        T* get() {
            return ptr;
        }
    };

    template<typename F>
    class scope_guard {
        F f;
        bool active;
    public:
        template<
            typename G,
            typename std::enable_if<!std::is_same<typename std::decay<G>::type, scope_guard<F>>::value, int>::type = 0
        >
        scope_guard(G&& f) : f(std::forward<F>(f)), active(true) {}
        ~scope_guard() {
            if(active) {
                f();
            }
        }
        scope_guard(const scope_guard&) = delete;
        scope_guard(scope_guard&& other) : f(std::move(other.f)), active(exchange(other.active, false)) {}
        scope_guard& operator=(const scope_guard&) = delete;
        scope_guard& operator=(scope_guard&& other) {
            f = std::move(other.f);
            active = exchange(other.active, false);
            return *this;
        }
    };

    template<typename F>
    NODISCARD auto scope_exit(F&& f) -> scope_guard<F> {
        return scope_guard<F>(std::forward<F>(f));
    }
}
CPPTRACE_END_NAMESPACE

#endif
