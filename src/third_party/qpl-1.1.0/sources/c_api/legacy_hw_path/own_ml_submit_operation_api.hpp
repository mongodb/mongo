/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef HW_PATH_ML_OWN_SUBMIT_OPERATION_API_H_
#define HW_PATH_ML_OWN_SUBMIT_OPERATION_API_H_

#include "qpl/c_api/job.h"

#ifdef __cplusplus
extern "C" {
#endif

enum class execution_mode_e {
    single_chunk,
    multi_chunk
};

/* ====== Compress ====== */
qpl_status hw_descriptor_compress_init_deflate_base(qpl_job *qpl_job_ptr,
                                                    hw_iaa_analytics_descriptor *const descriptor_ptr,
                                                    hw_completion_record *const completion_record_ptr,
                                                    qpl_hw_state *const state_ptr);

void hw_descriptor_compress_init_deflate_dynamic(hw_iaa_analytics_descriptor *desc_ptr,
                                                 qpl_hw_state *state_ptr,
                                                 qpl_job *qpl_job_ptr,
                                                 hw_iaa_aecs_compress *cfg_in_ptr,
                                                 hw_iaa_completion_record *comp_ptr);

void hw_descriptor_compress_init_deflate_canned(qpl_job *const job_ptr);


/* ====== Decompress ====== */
qpl_status hw_submit_decompress_job(qpl_job *qpl_job_ptr, uint32_t last_job, uint8_t *next_in_ptr, uint32_t available_in);

qpl_status hw_submit_simple_inflate(qpl_job *qpl_job_ptr);

qpl_status hw_submit_verify_job(qpl_job *qpl_job_ptr);

qpl_status hw_descriptor_decompress_init_inflate_body(hw_descriptor *const descriptor_ptr,
                                                      uint8_t **const data_ptr,
                                                      uint32_t *const data_size,
                                                      uint8_t *out_ptr,
                                                      uint32_t out_size,
                                                      const uint8_t ignore_start_bit,
                                                      const uint8_t ignore_end_bit,
                                                      const uint32_t crc_seed,
                                                      hw_iaa_aecs *const state_ptr);

#ifdef __cplusplus
}
#endif

#endif //HW_PATH_ML_OWN_SUBMIT_OPERATION_API_H_
