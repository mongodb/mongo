/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

 /**
  * @brief Contains implementation of all functions for scan analytics operation
  * @date 07/06/2020
  *
  * @details Function list:
  *          - @ref qplc_scan_lt_8u_i
  *          - @ref qplc_scan_lt_16u_i
  *          - @ref qplc_scan_lt_32u_i
  *          - @ref qplc_scan_le_8u_i
  *          - @ref qplc_scan_le_16u_i
  *          - @ref qplc_scan_le_32u_i
  *          - @ref qplc_scan_gt_8u_i
  *          - @ref qplc_scan_gt_16u_i
  *          - @ref qplc_scan_gt_32u_i
  *          - @ref qplc_scan_ge_8u_i
  *          - @ref qplc_scan_ge_16u_i
  *          - @ref qplc_scan_ge_32u_i
  *          - @ref qplc_scan_eq_8u_i
  *          - @ref qplc_scan_eq_16u_i
  *          - @ref qplc_scan_eq_32u_i
  *          - @ref qplc_scan_ne_8u_i
  *          - @ref qplc_scan_ne_16u_i
  *          - @ref qplc_scan_ne_32u_i
  *          - @ref qplc_scan_range_8u_i
  *          - @ref qplc_scan_range_16u_i
  *          - @ref qplc_scan_range_32u_i
  *          - @ref qplc_scan_not_range_8u_i
  *          - @ref qplc_scan_not_range_16u_i
  *          - @ref qplc_scan_not_range_32u_i
  *          - @ref qplc_scan_lt_8u
  *          - @ref qplc_scan_lt_16u
  *          - @ref qplc_scan_lt_32u
  *          - @ref qplc_scan_le_8u
  *          - @ref qplc_scan_le_16u
  *          - @ref qplc_scan_le_32u
  *          - @ref qplc_scan_gt_8u
  *          - @ref qplc_scan_gt_16u
  *          - @ref qplc_scan_gt_32u
  *          - @ref qplc_scan_ge_8u
  *          - @ref qplc_scan_ge_16u
  *          - @ref qplc_scan_ge_32u
  *          - @ref qplc_scan_eq_8u
  *          - @ref qplc_scan_eq_16u
  *          - @ref qplc_scan_eq_32u
  *          - @ref qplc_scan_ne_8u
  *          - @ref qplc_scan_ne_16u
  *          - @ref qplc_scan_ne_32u
  *          - @ref qplc_scan_range_8u
  *          - @ref qplc_scan_range_16u
  *          - @ref qplc_scan_range_32u
  *          - @ref qplc_scan_not_range_8u
  *          - @ref qplc_scan_not_range_16u
  *          - @ref qplc_scan_not_range_32u
  *
  */

#include "own_qplc_defs.h"

#if PLATFORM >= K0
#include "opt/qplc_scan_k0.h"
#endif


OWN_QPLC_FUN(void, qplc_scan_lt_8u_i, (uint8_t *src_dst_ptr, 
                                       uint32_t length, 
                                       uint32_t low_value, 
                                       uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_lt_8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        src_dst_ptr[idx] = (src_dst_ptr[idx] < low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_lt_16u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_lt_16u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint16_t *src_ptr = (uint16_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] < low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_lt_32u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_lt_32u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint32_t *src_ptr = (uint32_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] < low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_eq_8u_i, (uint8_t *src_dst_ptr, 
                                       uint32_t length, 
                                       uint32_t low_value, 
                                       uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_eq_8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        src_dst_ptr[idx] = (src_dst_ptr[idx] == low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_eq_16u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_eq_16u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint16_t *src_ptr = (uint16_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] == low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_eq_32u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_eq_32u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint32_t *src_ptr = (uint32_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] == low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ne_8u_i, (uint8_t *src_dst_ptr, 
                                       uint32_t length, 
                                       uint32_t low_value, 
                                       uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ne_8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        src_dst_ptr[idx] = (src_dst_ptr[idx] == low_value) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ne_16u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ne_16u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint16_t *src_ptr = (uint16_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] == low_value) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ne_32u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ne_32u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint32_t *src_ptr = (uint32_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] == low_value) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_le_8u_i, (uint8_t *src_dst_ptr, 
                                       uint32_t length, 
                                       uint32_t low_value, 
                                       uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_le_8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        src_dst_ptr[idx] = (src_dst_ptr[idx] <= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_le_16u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_le_16u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint16_t *src_ptr = (uint16_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] <= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_le_32u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_le_32u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint32_t *src_ptr = (uint32_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] <= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_gt_8u_i, (uint8_t *src_dst_ptr, 
                                       uint32_t length, 
                                       uint32_t low_value, 
                                       uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_gt_8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        src_dst_ptr[idx] = (src_dst_ptr[idx] > low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_gt_16u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_gt_16u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint16_t *src_ptr = (uint16_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] > low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_gt_32u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_gt_32u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint32_t *src_ptr = (uint32_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] > low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ge_8u_i, (uint8_t *src_dst_ptr, 
                                       uint32_t length, 
                                       uint32_t low_value, 
                                       uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ge_8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        src_dst_ptr[idx] = (src_dst_ptr[idx] >= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ge_16u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ge_16u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint16_t *src_ptr = (uint16_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] >= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ge_32u8u_i, (uint8_t *src_dst_ptr, 
                                          uint32_t length, 
                                          uint32_t low_value, 
                                          uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ge_32u8u)(src_dst_ptr, src_dst_ptr, length, low_value);
#else
    uint32_t *src_ptr = (uint32_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] >= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_range_8u_i, (uint8_t *src_dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_range_8u)(src_dst_ptr, src_dst_ptr, length, low_value, high_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        src_dst_ptr[idx] = ((src_dst_ptr[idx] >= low_value) && (src_dst_ptr[idx] <= high_value)) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_range_16u8u_i, (uint8_t *src_dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_range_16u8u)(src_dst_ptr, src_dst_ptr, length, low_value, high_value);
#else
    uint16_t *src_ptr = (uint16_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((src_ptr[idx] >= low_value) && (src_ptr[idx] <= high_value)) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_range_32u8u_i, (uint8_t *src_dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_range_32u8u)(src_dst_ptr, src_dst_ptr, length, low_value, high_value);
#else
    uint32_t *src_ptr = (uint32_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((src_ptr[idx] >= low_value) && (src_ptr[idx] <= high_value)) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_not_range_8u_i, (uint8_t *src_dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_not_range_8u)(src_dst_ptr, src_dst_ptr, length, low_value, high_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        src_dst_ptr[idx] = ((src_dst_ptr[idx] >= low_value) && (src_dst_ptr[idx] <= high_value)) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_not_range_16u8u_i, (uint8_t *src_dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_not_range_16u8u)(src_dst_ptr, src_dst_ptr, length, low_value, high_value);
#else
    uint16_t *src_ptr = (uint16_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((src_ptr[idx] >= low_value) && (src_ptr[idx] <= high_value)) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_not_range_32u8u_i, (uint8_t *src_dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_not_range_32u8u)(src_dst_ptr, src_dst_ptr, length, low_value, high_value);
#else
    uint32_t *src_ptr = (uint32_t *)src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((src_ptr[idx] >= low_value) && (src_ptr[idx] <= high_value)) ? 0u : 1u;
    }
#endif
}

/******** out-of-place scan functions ********/

OWN_QPLC_FUN(void, qplc_scan_lt_8u, (const uint8_t *src_ptr, 
                                     uint8_t *dst_ptr, 
                                     uint32_t length, 
                                     uint32_t low_value, 
                                     uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_lt_8u)(src_ptr, dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] < low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_lt_16u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_lt_16u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint16_t *p_src_16u = (uint16_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_16u[idx] < low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_lt_32u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_lt_32u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint32_t *p_src_32u = (uint32_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_32u[idx] < low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_eq_8u, (const uint8_t *src_ptr, 
                                     uint8_t *dst_ptr, 
                                     uint32_t length, 
                                     uint32_t low_value, 
                                     uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_eq_8u)(src_ptr, dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] == low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_eq_16u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_eq_16u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint16_t *p_src_16u = (uint16_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_16u[idx] == low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_eq_32u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_eq_32u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint32_t *p_src_32u = (uint32_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_32u[idx] == low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ne_8u, (const uint8_t *src_ptr, 
                                     uint8_t *dst_ptr, 
                                     uint32_t length, 
                                     uint32_t low_value, 
                                     uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ne_8u)(src_ptr, dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] == low_value) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ne_16u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ne_16u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint16_t *p_src_16u = (uint16_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_16u[idx] == low_value) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ne_32u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ne_32u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint32_t *p_src_32u = (uint32_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_32u[idx] == low_value) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_le_8u, (const uint8_t *src_ptr, 
                                     uint8_t *dst_ptr, 
                                     uint32_t length, 
                                     uint32_t low_value, 
                                     uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_le_8u)(src_ptr, dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] <= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_le_16u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_le_16u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint16_t *p_src_16u = (uint16_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_16u[idx] <= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_le_32u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_le_32u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint32_t *p_src_32u = (uint32_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_32u[idx] <= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_gt_8u, (const uint8_t *src_ptr, 
                                     uint8_t *dst_ptr, 
                                     uint32_t length, 
                                     uint32_t low_value, 
                                     uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_gt_8u)(src_ptr, dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] > low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_gt_16u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_gt_16u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint16_t *p_src_16u = (uint16_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_16u[idx] > low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_gt_32u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_gt_32u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint32_t *p_src_32u = (uint32_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_32u[idx] > low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ge_8u, (const uint8_t *src_ptr, 
                                     uint8_t *dst_ptr, 
                                     uint32_t length, 
                                     uint32_t low_value, 
                                     uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ge_8u)(src_ptr, dst_ptr, length, low_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (src_ptr[idx] >= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ge_16u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ge_16u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint16_t *p_src_16u = (uint16_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_16u[idx] >= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_ge_32u8u, (const uint8_t *src_ptr, 
                                        uint8_t *dst_ptr, 
                                        uint32_t length, 
                                        uint32_t low_value, 
                                        uint32_t UNREFERENCED_PARAMETER(high_value)))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_ge_32u8u)(src_ptr, dst_ptr, length, low_value);
#else
    const uint32_t *p_src_32u = (uint32_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = (p_src_32u[idx] >= low_value) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_range_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_range_8u)(src_ptr, dst_ptr, length, low_value, high_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((src_ptr[idx] >= low_value) && (src_ptr[idx] <= high_value)) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_range_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_range_16u8u)(src_ptr, dst_ptr, length, low_value, high_value);
#else
    const uint16_t *p_src_16u = (uint16_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((p_src_16u[idx] >= low_value) && (p_src_16u[idx] <= high_value)) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_range_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_range_32u8u)(src_ptr, dst_ptr, length, low_value, high_value);
#else
    const uint32_t *p_src_32u = (uint32_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((p_src_32u[idx] >= low_value) && (p_src_32u[idx] <= high_value)) ? 1u : 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_not_range_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_not_range_8u)(src_ptr, dst_ptr, length, low_value, high_value);
#else
    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((src_ptr[idx] >= low_value) && (src_ptr[idx] <= high_value)) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_not_range_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_not_range_16u8u)(src_ptr, dst_ptr, length, low_value, high_value);
#else
    const uint16_t *p_src_16u = (uint16_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((p_src_16u[idx] >= low_value) && (p_src_16u[idx] <= high_value)) ? 0u : 1u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_scan_not_range_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length, uint32_t low_value, uint32_t high_value))
{
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_scan_not_range_32u8u)(src_ptr, dst_ptr, length, low_value, high_value);
#else
    const uint32_t *p_src_32u = (uint32_t *)src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++)
    {
        dst_ptr[idx] = ((p_src_32u[idx] >= low_value) && (p_src_32u[idx] <= high_value)) ? 0u : 1u;
    }
#endif
}

