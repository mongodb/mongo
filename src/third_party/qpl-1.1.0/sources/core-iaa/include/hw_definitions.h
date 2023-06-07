/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef HW_PATH_HW_DEFINITIONS_H_
#define HW_PATH_HW_DEFINITIONS_H_

/**
 * @brief Types and Macro Definitions for Intel(R) Query Processing Library (Intel(R) QPL) Hardware Path.
 */

/**
 * @defgroup HW_INTERCONNECT_API Private API:Hardware Interconnect API
 */

/**
 * @defgroup HW_PUBLIC_API Public
 * @ingroup HW_INTERCONNECT_API
 */

/**
 * @addtogroup HW_PUBLIC_API
 * @{
 */

#include <stdint.h>
#include "hw_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ############### HARDWARE PATH DEFINITIONS ############## */
#if defined( _WIN32 ) || defined ( _WIN64 )
/**
 * The standard calling convention for the Microsoft Win32 API (not actual for Linux)
 */
#define HW_STDCALL __stdcall

/**
 * Standard calling convention for x86 architectures (not actual for Linux)
 */
#define HW_CDECL   __cdecl

#else
/**
 * The standard calling convention for the Microsoft Win32 API (not actual for Linux)
 */
#define HW_STDCALL

/**
 * Standard calling convention for x86 architectures (not actual for Linux)
 */
#define HW_CDECL

#endif


#if !defined( HW_PATH_GENERAL_API )
#define HW_PATH_GENERAL_API(type, name, arg) type HW_STDCALL hw_##name arg
#endif

#define HW_PATH_STRUCTURES_REQUIRED_ALIGN 64u  /**< @todo */

/* ################# HARDWARE PATH MACROS ################# */

#if defined(__GNUC__)
    /** @todo */
    #define HW_PATH_ALIGN_STRUCTURE __attribute__((aligned(HW_PATH_STRUCTURES_REQUIRED_ALIGN)))

    /** @todo */
    #define HW_PATH_VOLATILE __volatile__

    /**
     * @brief Packs a structure byte by byte
     */
    #define HW_PATH_BYTE_PACKED_STRUCTURE_BEGIN \
        typedef struct __attribute__ ((__packed__))

    /**
     * @brief Pops a previous structure pack property
     */
    #define HW_PATH_BYTE_PACKED_STRUCTURE_END
#elif(_MSC_VER)
    /** @todo */
    #define HW_PATH_ALIGN_STRUCTURE

    /** @todo */
    #define HW_PATH_VOLATILE volatile

    /**
     * @brief Packs a structure byte by byte
     */
    #define HW_PATH_BYTE_PACKED_STRUCTURE_BEGIN \
        __pragma(pack(push, 1)) \
        typedef struct

    /**
    * @brief Pops a previous structure pack property
    */
    #define HW_PATH_BYTE_PACKED_STRUCTURE_END \
        __pragma(pack(pop))
#else
    #error Compiler not supported
#endif


/* ################# DESCRIPTOR  ################# */

#define HW_PATH_DESCRIPTOR_SIZE  (64u)  /**< Hardware descriptor byte size */

/**
 * @brief Defines a common type of the hardware descriptor
 */
HW_PATH_BYTE_PACKED_STRUCTURE_BEGIN {
    uint8_t data[HW_PATH_DESCRIPTOR_SIZE];    /**< Allocated memory for an abstract descriptor */
} hw_descriptor;
HW_PATH_BYTE_PACKED_STRUCTURE_END


/* ################# COMPLETION RECORD  ################# */

#define HW_PATH_COMPLETION_RECORD_SIZE  (64u) /**< Hardware completion record byte size */

/**
 * @brief Defines an abstract type of the Hardware completion record
 */
HW_PATH_BYTE_PACKED_STRUCTURE_BEGIN {
    uint8_t status;                                         /**< Completion status field */
    uint8_t error;                                          /**< Completion error field */
    uint8_t bytes[HW_PATH_COMPLETION_RECORD_SIZE - 2u];     /**< Allocated memory for others fields */
} hw_completion_record;
HW_PATH_BYTE_PACKED_STRUCTURE_END


/**
 * @todo hide details
 */
HW_PATH_BYTE_PACKED_STRUCTURE_BEGIN {
    uint32_t trusted_fields;            /**< 19:0 PASID - process address space ID; 30:20 - reserved; 31 - User/Supervisor */
    uint32_t op_code_op_flags;          /**< Opcode 31:24, opflags 23:0 */
    uint8_t  *completion_record_ptr;    /**< Completion record address */
    uint8_t  *src1_ptr;                 /**< Source 1 address */
    uint8_t  *dst_ptr;                  /**< Destination address */
    uint32_t src1_size;                 /**< Source 1 transfer size */
    uint16_t comp_int_handle;           /**< Not used (completion interrupt handle) */
    uint16_t decomp_flags;              /**< (De)compression flags */
    uint8_t  *src2_ptr;                 /**< Source 2 address | AECS address (32-bit aligned) */
    uint32_t max_dst_size;              /**< Maximum destination size */
    uint32_t src2_size;                 /**< Source 2 transfer size | AECS size (multiple of 32-bytes, LE 288 bytes) */
    uint32_t filter_flags;              /**< Crc64 poly | filter flags */
    uint32_t num_input_elements;        /**< Crc64 poly | number of input elements */
} hw_iaa_analytics_descriptor;
HW_PATH_BYTE_PACKED_STRUCTURE_END

#ifdef __cplusplus
}
#endif

#endif //HW_PATH_HW_DEFINITIONS_H_

/** @} */
