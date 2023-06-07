/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_VERSION_H_
#define QPL_VERSION_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Returns a string with a version of the library
 */
const char *qpl_get_library_version();

#ifdef __cplusplus
}
#endif

#endif //QPL_VERSION_H_
