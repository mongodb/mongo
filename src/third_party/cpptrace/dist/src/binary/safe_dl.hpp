#ifndef SAFE_DL_HPP
#define SAFE_DL_HPP

#include "utils/common.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    void get_safe_object_frame(frame_ptr address, safe_object_frame* out);

    bool has_get_safe_object_frame();
}
CPPTRACE_END_NAMESPACE

#endif
