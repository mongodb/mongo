/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains API to work with Intel® In-Memory Analytics Accelerator (Intel® IAA) Devices.
 *
 * @details @todo
 *
 * @defgroup HW_DEVICES_API Devices API
 * @ingroup HW_PUBLIC_API
 * @{
 */

#ifndef HW_PATH_HW_DEVICES_H_
#define HW_PATH_HW_DEVICES_H_

#include "stdbool.h"
#include <inttypes.h>

#ifdef LOG_HW_INIT

#include <stdio.h>
#define DIAGA(...) printf(__VA_ARGS__); fflush(stdout)                  /**< Diagnostic printer for appending to line */
#define DIAG(...) printf("qpl-diag: " __VA_ARGS__); fflush(stdout)      /**< Diagnostic printer */
#else
#define DIAGA(...)                                                      /**< Diagnostic printer for appending to line */
#define DIAG(...)                                                       /**< Diagnostic printer */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ====== Definitions ====== */

/**
 * @todo
 */
#define IAA_DEVICE ((uint32_t)(((uint32_t)0xFF << 24u) | \
                   ((uint32_t)('x') << 16u) | ((uint32_t)('a') << 8u) | (uint32_t)('i')))
#define MAX_NUM_DEV 100u        /**< @todo */
#define MAX_NUM_WQ  100u        /**< @todo */
#define CHAR_MSK    0xFF202020  /**< @todo */

#define OWN_PAGE_MASK             0x0FFFllu     /**< Defines page mask for portal incrementing */

/* ====== Macros ====== */
/**
 * @name Gencap Configuration Macros
 * @anchor HW_GENCAP_MACROS
 * @todo
 * @{
 */
#define GC_BLOCK_ON_FAULT(GENCAP)           (((GENCAP))    &0x01)       /**< GENCAP bit 0      - block on fault support                    */
#define GC_OVERLAPPING(GENCAP)              (((GENCAP)>>1) &0x01)       /**< GENCAP bit 1      - overlapping copy support                  */
#define GC_CACHE_WRITE(GENCAP)              (((GENCAP)>>2) &0x01)       /**< GENCAP bit 2      - cache control support (memory)            */
#define GC_CACHE_FLUSH(GENCAP)              (((GENCAP)>>3) &0x01)       /**< GENCAP bit 3      - cache control support (cache flush)       */
#define GC_COM_CAP(GENCAP)                  (((GENCAP)>>4) &0x01)       /**< GENCAP bit 4      - command capabilities support              */
#define GC_DST_READBACK(GENCAP)             (((GENCAP)>>8) &0x01)       /**< GENCAP bit 8      - destination readback support              */
#define GC_DRAIN_READBACK(GENCAP)           (((GENCAP)>>9) &0x01)       /**< GENCAP bit 9      - drain descriptor readback address support */
#define GC_MAX_TRANSFER_SIZE(GENCAP)  (1 << (((GENCAP)>>16)&0x1F))      /**< GENCAP 20-16 bits - maximum supported transfer size           */
#define GC_INTERRUPT_STORAGE(GENCAP)       ((((GENCAP)>>25)&0x3F)*256u) /**< GENCAP 30-25 bits - interrupt message storage size            */
#define GC_CONF_SUPPORT(GENCAP)             (((GENCAP)>>31)&0x01)       /**< GENCAP bit 31     - configuration support                     */
#define GC_DECOMP_SUPPORT(GENCAP)           (((GENCAP)>>40)&0x01)       /**< GENCAP bit 40     - decompression support                     */
#define GC_IDX_SUPPORT(GENCAP)              (((GENCAP)>>41)&0x01)       /**< GENCAP bit 41     - indexing support                          */
#define GC_MAX_DECOMP_SET_SIZE(GENCAP)     ((((GENCAP)>>42)&0x1F) + 1u) /**< GENCAP 46-42 bits - maximum decompression set size            */
#define GC_MAX_SET_SIZE(GENCAP)            ((((GENCAP)>>47)&0x1F) + 1u) /**< GENCAP 51-47 bits - maximum set size                          */

/** @} */

/* ====== Structures ====== */

/**
 * @todo
 */
typedef struct {
    uint32_t max_set_size;                      /**< @todo */
    uint32_t max_decompressed_set_size;         /**< @todo */
    uint32_t max_transfer_size;                 /**< @todo */
    bool     cache_flush_available;             /**< @todo */
    bool     cache_write_available;             /**< @todo */
    bool     overlapping_available;             /**< @todo */
    bool     indexing_support_enabled;          /**< @todo */
    bool     decompression_support_enabled;     /**< @todo */
    bool     block_on_fault_enabled;            /**< @todo */
} hw_device_properties;


/* ====== Functions ====== */

#if defined( linux )
typedef struct accfg_ctx    accfg_ctx;   /**< @todo */
typedef struct accfg_device accfg_dev;   /**< @todo */
typedef struct accfg_group  accfgGrp;    /**< @todo */
typedef struct accfg_wq     accfg_wq;    /**< @todo */
typedef struct accfg_engine accfgEngn;   /**< @todo */
#endif

#if defined( linux )
typedef accfg_ctx  hw_context; /**< Linux defined context type */
#else
typedef void       hw_context; /**< Windows defined context type */
#endif

/**
 * @todo
 */
typedef struct {
    hw_device_properties  device_properties;   /**< Accelerator properties */
    hw_context           *ctx_ptr;             /**< Hardware context instance */
} hw_accelerator_context;

/**
 * @brief Structure for HW descriptor submit options
 */
typedef struct {
    uint64_t submit_flags;  /**< @todo */
    int32_t numa_id;        /**< ID of the NUMA. Set it to -1 for auto detecting */
} hw_accelerator_submit_options;

#ifdef __cplusplus
}
#endif

#endif //HW_PATH_HW_DEVICES_H_

/** @} */
