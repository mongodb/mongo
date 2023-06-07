/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_SOURCES_C_API_INCLUDE_CHECKERS_HPP_
#define QPL_SOURCES_C_API_INCLUDE_CHECKERS_HPP_

#include "common/defs.hpp"

#define OWN_QPL_CHECK_STATUS(status_expression) { auto value = (status_expression);\
if(value) return static_cast<qpl_status>(value); }

namespace qpl::ml::bad_argument {
static inline auto buffers_overlap(const uint8_t *const ptr1,
                                   const size_t ptr1_size,
                                   const uint8_t *const ptr2,
                                   const size_t ptr2_size) {
        if ((ptr2 >= ptr1) && (ptr2 < (ptr1 + ptr1_size))) {
            return true;
        }

        if ((ptr1 >= ptr2) && (ptr1 < (ptr2 + ptr2_size))) {
            return true;
        }

        return false;
    }

template <typename first_arg_t, typename ...other_args_t>
static inline auto check_for_nullptr(first_arg_t first_arg, other_args_t... other_args) noexcept -> ml::qpl_ml_status {
    if constexpr (!sizeof...(other_args_t)) {
        if (first_arg == nullptr) {
            return ml::status_list::nullptr_error;
        } else {
            return ml::status_list::ok;
        }
    } else {
        if (first_arg == nullptr) {
            return ml::status_list::nullptr_error;
        } else {
            return check_for_nullptr(other_args...);
        }
    }
}
}

#endif //QPL_SOURCES_C_API_INCLUDE_CHECKERS_HPP_
