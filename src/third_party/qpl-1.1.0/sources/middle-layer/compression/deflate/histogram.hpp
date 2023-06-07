/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_HISTOGRAM_HPP_
#define QPL_HISTOGRAM_HPP_

#include <cstddef>
#include "common/defs.hpp"
#include "qpl/c_api/statistics.h"

namespace qpl::ml::compression {

using deflate_histogram = qpl_histogram;
using deflate_level = qpl_compression_levels;

template <execution_path_t path,
        class iterator_t,
        class = typename std::enable_if<path == execution_path_t::software ||
                                        path == execution_path_t::hardware>::type>
auto update_histogram(iterator_t begin,
                      iterator_t end,
                      deflate_histogram &histogram,
                      deflate_level level = qpl_default_level) noexcept -> qpl_ml_status;
}

#endif //QPL_HISTOGRAM_HPP_
