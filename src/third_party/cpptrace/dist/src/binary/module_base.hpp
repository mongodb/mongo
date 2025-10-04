#ifndef IMAGE_MODULE_BASE_HPP
#define IMAGE_MODULE_BASE_HPP

#include "utils/utils.hpp"

#include <cstdint>
#include <string>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    Result<std::uintptr_t, internal_error> get_module_image_base(const std::string& object_path);
}
CPPTRACE_END_NAMESPACE

#endif
