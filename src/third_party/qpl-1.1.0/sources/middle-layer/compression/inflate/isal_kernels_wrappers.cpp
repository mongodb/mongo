/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "isal_kernels_wrappers.hpp"
#include "crc.h"
#include "igzip_lib.h"
#include "igzip_checksums.h"

namespace qpl::ml::compression::isal_kernels {
    using isal_status = int;

    static auto inline isal_to_qpl_status(isal_status status) noexcept -> qpl_ml_status;

    auto read_deflate_header(isal_inflate_state &inflate_state) noexcept -> qpl_ml_status {
        auto status = read_header(&inflate_state);

        return isal_to_qpl_status(status);
    }

    auto decode_huffman_code_block(isal_inflate_state &inflate_state, uint8_t *start_out_ptr) noexcept -> qpl_ml_status {
        auto status = decode_huffman_code_block_stateless(&inflate_state, start_out_ptr);

        return isal_to_qpl_status(status);
    }

    auto check_gzip_checksum(isal_inflate_state &inflate_state) noexcept -> qpl_ml_status {
        auto status = check_gzip_checksum(&inflate_state);

        // There are two statuses are possible:
        if (status == ISAL_INCORRECT_CHECKSUM) {
            return status_list::verify_error;
        } else {
            return status_list::ok;
        }
    }

    static auto inline isal_to_qpl_status(isal_status status)  noexcept -> qpl_ml_status {
        if (status <= QPL_HW_BASE_CODE) {
            return static_cast<qpl_ml_status>(-status);
        } else {
            if (ISAL_END_INPUT == status) {
                return status_list::input_too_small;
            } else {
                return static_cast<qpl_ml_status>(status);
            }
        }
    }
}
