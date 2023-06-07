/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_DICTIONARY_H_
#define QPL_DICTIONARY_H_

#include "qpl/c_api/status.h"
#include "qpl/c_api/defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup JOB_API_DEFINITIONS
 * @{
 */

/**
 * @brief Structure represents the dictionary used for compression / decompression
 */
typedef struct qpl_dictionary qpl_dictionary;

/** @} */

/**
 * @addtogroup JOB_API_FUNCTIONS
 * @{
 */

/**
 * @brief Calculates dictionary size with provided parameters
 *
 * @param[in] sw_level       The compression level for a software path
 * @param[in] hw_level       The compression level for a hardware path
 * @param[in] raw_dict_size  The size (in bytes) of a raw dictionary
 *
 * @return Returns dictionary size (in bytes)
 */
QPL_API(size_t, qpl_get_dictionary_size, (sw_compression_level sw_level,
                                          hw_compression_level hw_level,
                                          size_t raw_dict_size)) ;

/**
 * @brief Returns the size of the dictionary built
 *
 * @param[in]  dict_ptr     Pointer to the dictionary
 * @param[out] destination  Pointer to size_t, where the dictionary size (in bytes) is stored
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_NULL_PTR_ERR.
 */
QPL_API(qpl_status, qpl_get_existing_dict_size, (qpl_dictionary * dict_ptr,
                                                 size_t * destination)) ;

/**
 * @brief This function creates @ref qpl_dictionary from the raw dictionary given (raw data)
 *
 * @param[out] dict_ptr       Pointer to result @ref qpl_dictionary
 * @param[in]  sw_level       The compression level for a software path
 * @param[in]  hw_level       The compression level for a hardware path
 * @param[in]  raw_dict_ptr   Pointer to the raw dictionary (or raw data)
 * @param[in]  raw_dict_size  The size (in bytes) of the raw dictionary
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_NULL_PTR_ERR.
 */
QPL_API(qpl_status, qpl_build_dictionary, (qpl_dictionary * dict_ptr,
                                           sw_compression_level sw_level,
                                           hw_compression_level hw_level,
                                           const uint8_t *raw_dict_ptr,
                                           size_t        raw_dict_size)) ;

/**
 * @brief Sets id to the dictionary specified
 *
 * @param[out]     dictionary_ptr  Pointer to the dictionary to set id to
 * @param[in]      dictionary_id   Id of the dictionary to be set
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_NULL_PTR_ERR.
 */
QPL_API(qpl_status, qpl_set_dictionary_id, (qpl_dictionary * dictionary_ptr,
                                            uint32_t dictionary_id)) ;

/**
 * @brief Returns dictionary id
 *
 * @param[in]  dictionary_ptr  Pointer to the dictionary initialized
 * @param[out] destination     Pointer to uint32_t, where the dictionary id is stored
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_NULL_PTR_ERR.
 */
QPL_API(qpl_status, qpl_get_dictionary_id, (qpl_dictionary * dictionary_ptr,
                                            uint32_t * destination)) ;
/** @} */

#ifdef __cplusplus
}
#endif

#endif //QPL_DICTIONARY_H_
