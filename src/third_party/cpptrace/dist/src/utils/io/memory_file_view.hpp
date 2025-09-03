#ifndef MEMORY_FILE_VIEW_HPP
#define MEMORY_FILE_VIEW_HPP

#include "utils/error.hpp"
#include "utils/span.hpp"
#include "utils/io/base_file.hpp"
#include "utils/utils.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    class memory_file_view : public base_file {
        cbspan data;
        std::string object_path = "<memory file>";

    public:
        memory_file_view(cbspan data) : data(data) {}
        ~memory_file_view() override = default;

        string_view path() const override;

        virtual Result<monostate, internal_error> read_bytes(bspan buffer, off_t offset) const override;
    };
}
CPPTRACE_END_NAMESPACE

#endif
