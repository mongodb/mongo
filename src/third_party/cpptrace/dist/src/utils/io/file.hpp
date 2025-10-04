#ifndef FILE_HPP
#define FILE_HPP

#include "utils/string_view.hpp"
#include "utils/span.hpp"
#include "utils/io/base_file.hpp"
#include "utils/utils.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    class file : public base_file {
        file_wrapper file_obj;
        std::string object_path;

        file(file_wrapper file_obj, string_view path) : file_obj(std::move(file_obj)), object_path(path) {}

    public:
        file(file&&) = default;
        ~file() override = default;

        string_view path() const override;

        static Result<file, internal_error> open(cstring_view object_path);

        virtual Result<monostate, internal_error> read_bytes(bspan buffer, off_t offset) const override;
    };
}
CPPTRACE_END_NAMESPACE

#endif
