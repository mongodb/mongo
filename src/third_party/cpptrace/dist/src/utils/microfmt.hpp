#ifndef MICROFMT_HPP
#define MICROFMT_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#if ((defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L)
 #include <string_view>
#endif
#ifdef _MSC_VER
 #include <intrin.h>
#endif

#include "utils/string_view.hpp"

// https://github.com/jeremy-rifkin/microfmt
// Format: {[align][width][:[fill][base]]}  # width: number or {}

CPPTRACE_BEGIN_NAMESPACE
namespace microfmt {
    namespace detail {
        inline std::uint64_t clz(std::uint64_t value) {
            #ifdef _MSC_VER
             unsigned long out = 0;
             #ifdef _WIN64
              _BitScanReverse64(&out, value);
             #else
              if(_BitScanReverse(&out, std::uint32_t(value >> 32))) {
                  return 63 - int(out + 32);
              }
              _BitScanReverse(&out, std::uint32_t(value));
             #endif
             return 63 - out;
            #else
             return __builtin_clzll(value);
            #endif
        }

        template<typename U, typename V> U to(V v) {
            return static_cast<U>(v); // A way to cast to U without "warning: useless cast to type"
        }

        enum class alignment { left, right };

        struct format_options {
            alignment align = alignment::left;
            char fill = ' ';
            size_t width = 0;
            char base = 'd';
        };

        template<typename OutputIt, typename InputIt>
        void do_write(OutputIt out, InputIt begin, InputIt end, const format_options& options) {
            auto size = end - begin;
            if(static_cast<std::size_t>(size) >= options.width) {
                std::copy(begin, end, out);
            } else {
                if(options.align == alignment::left) {
                    std::copy(begin, end, out);
                    std::fill_n(out, options.width - size, options.fill);
                } else {
                    std::fill_n(out, options.width - size, options.fill);
                    std::copy(begin, end, out);
                }
            }
        }

        template<int shift, int mask>
        std::string to_string(std::uint64_t value, const char* digits = "0123456789abcdef") {
            if(value == 0) {
                return "0";
            } else {
                // digits = floor(1 + log_base(x))
                // log_base(x) = log_2(x) / log_2(base)
                // log_2(x) == 63 - clz(x)
                // 1 + (63 - clz(value)) / (63 - clz(1 << shift))
                // 63 - clz(1 << shift) is the same as shift
                auto n_digits = to<std::size_t>(1 + (63 - clz(value)) / shift);
                std::string number;
                number.resize(n_digits);
                std::size_t i = n_digits - 1;
                while(value > 0) {
                    number[i--] = digits[value & mask];
                    value >>= shift;
                }
                return number;
            }
        }

        inline std::string to_string(std::uint64_t value, const format_options& options) {
            switch(options.base) {
                case 'H': return to_string<4, 0xf>(value, "0123456789ABCDEF");
                case 'h': return to_string<4, 0xf>(value);
                case 'o': return to_string<3, 0x7>(value);
                case 'b': return to_string<1, 0x1>(value);
                default: return std::to_string(value); // failure: decimal
            }
        }

        struct string_view {
            const char* data;
            std::size_t size;
        };

        class format_value {
            enum class value_type {
                char_value,
                int64_value,
                uint64_value,
                string_value,
                string_view_value,
                c_string_value,
            };
            union {
                char char_value;
                std::int64_t int64_value;
                std::uint64_t uint64_value;
                const std::string* string_value;
                string_view string_view_value;
                const char* c_string_value;
            };
            value_type value;

        public:
            format_value(char c) : char_value(c), value(value_type::char_value) {}
            format_value(short int_val) : int64_value(int_val), value(value_type::int64_value) {}
            format_value(int int_val) : int64_value(int_val), value(value_type::int64_value) {}
            format_value(long int_val) : int64_value(int_val), value(value_type::int64_value) {}
            format_value(long long int_val) : int64_value(int_val), value(value_type::int64_value) {}
            format_value(unsigned char int_val) : uint64_value(int_val), value(value_type::uint64_value) {}
            format_value(unsigned short int_val) : uint64_value(int_val), value(value_type::uint64_value) {}
            format_value(unsigned int int_val) : uint64_value(int_val), value(value_type::uint64_value) {}
            format_value(unsigned long int_val) : uint64_value(int_val), value(value_type::uint64_value) {}
            format_value(unsigned long long int_val) : uint64_value(int_val), value(value_type::uint64_value) {}
            format_value(const std::string& string) : string_value(&string), value(value_type::string_value) {}
            #if ((defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L)
            format_value(std::string_view sv)
                : string_view_value{sv.data(), sv.size()}, value(value_type::string_view_value) {}
            #endif
            format_value(cpptrace::detail::string_view sv)
                : string_view_value{sv.data(), sv.size()}, value(value_type::string_view_value) {}
            format_value(const char* c_string) : c_string_value(c_string), value(value_type::c_string_value) {}

            int unwrap_int() const {
                switch(value) {
                    case value_type::int64_value:  return static_cast<int>(int64_value);
                    case value_type::uint64_value: return static_cast<int>(uint64_value);
                    default: return 0; // failure: just 0
                }
            }

        public:
            template<typename OutputIt>
            void write(OutputIt out, const format_options& options) const {
                switch(value) {
                    case value_type::char_value:
                        do_write(out, &char_value, &char_value + 1, options);
                        break;
                    case value_type::int64_value:
                        {
                            std::string str;
                            std::int64_t val = int64_value;
                            if(val < 0) {
                                str += '-';
                                val *= -1;
                            }
                            str += to_string(static_cast<std::uint64_t>(val), options);
                            do_write(out, str.begin(), str.end(), options);
                        }
                        break;
                    case value_type::uint64_value:
                        {
                            std::string str = to_string(uint64_value, options);
                            do_write(out, str.begin(), str.end(), options);
                        }
                        break;
                    case value_type::string_value:
                        do_write(out, string_value->begin(), string_value->end(), options);
                        break;
                    case value_type::string_view_value:
                        do_write(out, string_view_value.data, string_view_value.data + string_view_value.size, options);
                        break;
                    case value_type::c_string_value:
                        do_write(out, c_string_value, c_string_value + std::strlen(c_string_value), options);
                        break;
                } // failure: nop
            }
        };

        // note: previously used std::array and there was a bug with std::array<T, 0> affecting old msvc
        // https://godbolt.org/z/88T8hrzzq mre: https://godbolt.org/z/drd8echbP
        template<typename OutputIt, typename InputIt>
        void format(OutputIt out, InputIt fmt_begin, InputIt fmt_end, const std::initializer_list<format_value>& args) {
            std::size_t arg_i = 0;
            auto it = fmt_begin;
            auto peek = [&] (std::size_t dist) -> char { // 0 on failure
                return fmt_end - it > signed(dist) ? *(it + dist) : 0;
            };
            auto read_number = [&] () -> int { // -1 on failure
                auto scan = it;
                int num = 0;
                while(scan != fmt_end && isdigit(*scan)) {
                    num *= 10;
                    num += *scan - '0';
                    scan++;
                }
                if(scan != it) {
                    it = scan;
                    return num;
                } else {
                    return -1;
                }
            };
            for(; it != fmt_end; it++) {
                if((*it == '{' || *it == '}') && peek(1) == *it) { // parse {{ and }} escapes
                    it++;
                } else if(*it == '{' && it + 1 != fmt_end) {
                    auto saved_it = it;
                    auto handle_formatter = [&] () {
                        it++;
                        format_options options;
                        // try to parse alignment
                        if(*it == '<' || *it == '>') {
                            options.align = *it++ == '<' ? alignment::left : alignment::right;
                        }
                        // try to parse width
                        auto width = read_number(); // handles fmt_end check
                        if(width != -1) {
                            options.width = width;
                        } else if(it != fmt_end && *it == '{') { // try to parse variable width
                            if(peek(1) != '}') {
                                return false;
                            }
                            it += 2;
                            options.width = arg_i < args.size() ? args.begin()[arg_i++].unwrap_int() : 0;
                        }
                        // try to parse fill/base
                        if(it != fmt_end && *it == ':') {
                            it++;
                            if(fmt_end - it > 1 && *it != '}' && peek(1) != '}') { // two chars before the }, fill+base
                                options.fill = *it++;
                                options.base = *it++;
                            } else if(it != fmt_end && *it != '}') { // one char before the }, just base
                                if(*it == 'd' || *it == 'h' || *it == 'H' || *it == 'o' || *it == 'b') {
                                    options.base = *it++;
                                } else {
                                    options.fill = *it++;
                                }
                            }
                        }
                        if(it == fmt_end || *it != '}') {
                            return false;
                        }
                        if(arg_i < args.size()) {
                            args.begin()[arg_i++].write(out, options);
                        }
                        return true;
                    };
                    if(handle_formatter()) {
                        continue; // If reached here, successfully parsed and wrote a formatter. Don't write *it.
                    }
                    it = saved_it; // go back
                }
                *out++ = *it;
            }
        }

        #if ((defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L)
        template<typename OutputIt>
        void format(OutputIt out, std::string_view fmt, const std::initializer_list<format_value>& args) {
            return format(out, fmt.begin(), fmt.end(), args);
        }
        #endif

        template<typename OutputIt>
        void format(OutputIt out, const char* fmt, const std::initializer_list<format_value>& args) {
            return format(out, fmt, fmt + std::strlen(fmt), args);
        }

        std::ostream& get_cout();
    }

    template<typename S, typename... Args>
    std::string format(const S& fmt, Args&&... args) {
        std::string str;
        detail::format(std::back_inserter(str), fmt, {detail::format_value(args)...});
        return str;
    }

    template<typename S, typename... Args>
    void print(const S& fmt, Args&&... args) {
        detail::format(std::ostream_iterator<char>(detail::get_cout()), fmt, {args...});
    }

    template<typename S, typename... Args>
    void print(std::ostream& ostream, const S& fmt, Args&&... args) {
        detail::format(std::ostream_iterator<char>(ostream), fmt, {args...});
    }

    template<typename S, typename... Args>
    void print(std::FILE* stream, const S& fmt, Args&&... args) {
        auto str = format(fmt, args...);
        fwrite(str.data(), 1, str.size(), stream);
    }
}
CPPTRACE_END_NAMESPACE

#endif
