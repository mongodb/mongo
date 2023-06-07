/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "own_defs.h"
#include "compression/deflate/histogram.hpp"
#include "util/checkers.hpp"

extern "C" {

QPL_FUN(qpl_status, qpl_gather_deflate_statistics, (uint8_t * source_ptr,
        const uint32_t               source_length,
        qpl_histogram                *histogram_ptr,
        const qpl_compression_levels level,
        const qpl_path_t             path)) {
    using namespace qpl::ml;

    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(source_ptr, histogram_ptr));

    if (level != qpl_high_level && level != qpl_default_level) {
        return QPL_STS_UNSUPPORTED_COMPRESSION_LEVEL;
    }

    qpl_ml_status status = status_list::ok;

    const uint8_t *const begin = source_ptr;
    const uint8_t *const end = source_ptr + source_length;

    switch (path) {
        case qpl_path_auto:
            return QPL_STS_NOT_SUPPORTED_MODE_ERR;

        case qpl_path_hardware:
            status = compression::update_histogram<execution_path_t::hardware>(begin,
                                                                               end,
                                                                               *histogram_ptr);
            return static_cast<qpl_status>(status);
        case qpl_path_software:
            status = compression::update_histogram<execution_path_t::software>(begin,
                                                                               end,
                                                                               *histogram_ptr,
                                                                               level);
            return static_cast<qpl_status>(status);
        default:
            return QPL_STS_PATH_ERR;
    }
}

}
