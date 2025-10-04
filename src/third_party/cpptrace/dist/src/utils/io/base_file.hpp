#ifndef BASE_FILE_HPP
#define BASE_FILE_HPP

#include "utils/span.hpp"
#include "utils/utils.hpp"

#include <type_traits>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    class base_file {
    public:
        virtual ~base_file() = default;
        virtual string_view path() const = 0;
        virtual Result<monostate, internal_error> read_bytes(bspan buffer, off_t offset) const = 0;

        template<
            typename T,
            typename std::enable_if<
                std::is_standard_layout<T>::value && is_trivially_copyable<T>::value && !is_span<T>::value,
                int
            >::type = 0
        >
        Result<T, internal_error> read(off_t offset) {
            T object{};
            auto res = read_bytes(make_bspan(object), offset);
            if(!res) {
                return res.unwrap_error();
            }
            return object;
        }

        template<
            typename T,
            typename std::enable_if<
                std::is_standard_layout<T>::value && is_trivially_copyable<T>::value,
                int
            >::type = 0
        >
        Result<monostate, internal_error> read_span(span<T> items, off_t offset) {
            return read_bytes(
                make_span(reinterpret_cast<char*>(items.data()), reinterpret_cast<char*>(items.data() + items.size())),
                offset
            );
        }
    };
}
CPPTRACE_END_NAMESPACE

#endif
