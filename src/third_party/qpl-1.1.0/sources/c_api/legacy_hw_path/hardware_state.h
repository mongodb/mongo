/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 03/23/2020
 * @brief Types and Macro Definitions for Intel(R) Query Processing Library (Intel(R) QPL) Hardware Path.
 */

#ifndef HW_PATH_ML_HW_DEFINITIONS_H_
#define HW_PATH_ML_HW_DEFINITIONS_H_

#include "stdbool.h"
#include "qpl/c_api/defs.h"
#include "hw_definitions.h"
#include "hw_devices.h"
#include "hw_aecs_api.h"
#include "hw_completion_record_api.h"
#include "core_deflate_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QPL_INTERNAL_BUFFER_SIZE 1024u  /**< Max size of internal buffer */
#define FLIP_AECS_OFFSET(p) (p)->aecs_hw_read_offset = (p)->aecs_size - (p)->aecs_hw_read_offset   /**< @todo */

typedef enum {
    qpl_task_execution_step_statistic_collection,
    qpl_task_execution_step_header_inserting,
    qpl_task_execution_step_data_processing,
    qpl_task_execution_step_verification,
    qpl_task_execution_step_completed,
} qpl_task_execution_step;


typedef enum {
    qpl_cst_fixed,
    qpl_cst_static,
    qpl_cst_dynamic
} qpl_comp_style;

typedef struct {
    bool                    first_job_has_been_submitted;
    qpl_task_execution_step execution_step;
    qpl_comp_style          comp_style;
    uint32_t         compress_crc;
    uint8_t          *saved_next_out_ptr;
} qpl_execution_history;

/**
 * @brief Holds temporal data for some operations
 */
typedef struct {
    uint8_t  data[QPL_INTERNAL_BUFFER_SIZE]; /**< Buffer data              */
    uint32_t actual_bytes;                   /**< Written bytes count      */
    uint32_t available_bytes;                /**< Available bytes to write */
} qpl_buffer;

/**
 * @todo
 * @note Structure is aligned to 64-bytes, put things that need alignment first
 */
typedef struct {
    hw_iaa_analytics_descriptor  desc_ptr;                                     /**< @todo */
    hw_iaa_completion_record comp_ptr;                                     /**< @todo */
    hw_iaa_aecs_analytic     dcfg[2];                                      /**< @todo */
    hw_iaa_aecs_compress     ccfg[2];                                      /**< @todo */
    qpl_buffer               accumulation_buffer;
    qpl_execution_history    execution_history;
    uint32_t                 config_valid;                                 /**< @todo */
    uint32_t                 aecs_hw_read_offset;                          /**< @todo */
    uint32_t                 aecs_size;                                    /**< @todo */
    hw_huffman_code          eob_code;
    uint32_t                 saved_num_output_accum_bits;                  /**< @todo */
    hw_accelerator_context   accel_context;
    uint32_t                 descriptor_not_submitted;
    bool                     job_is_submitted;
} qpl_hw_state;

#ifdef __cplusplus
}
#endif

#endif // HW_PATH_ML_HW_DEFINITIONS_H_
