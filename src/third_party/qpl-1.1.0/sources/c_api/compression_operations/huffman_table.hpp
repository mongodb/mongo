/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (private C API)
 */

#ifndef QPL_STATISTICS__HPP_
#define QPL_STATISTICS__HPP_

#include "qpl/c_api/huffman_table.h"
#include "compression/huffman_table/huffman_table.hpp"

template<qpl::ml::compression::compression_algorithm_e algorithm>
auto check_huffman_table_is_correct(qpl_huffman_table_t table) {
    auto meta = reinterpret_cast<qpl::ml::compression::huffman_table_meta_t*>(table);

    if (meta->algorithm != algorithm) {
        return QPL_STS_HUFFMAN_TABLE_TYPE_ERROR;
    } else {
        return QPL_STS_OK;
    }
}

template<qpl::ml::compression::compression_algorithm_e algorithm>
auto use_as_huffman_table(qpl_huffman_table_t table) {
    return reinterpret_cast<qpl::ml::compression::huffman_table_t<algorithm>*>(table);
}

#endif //QPL_STATISTICS__HPP_
