/*
 * Copyright 2018-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef KMS_MESSAGE_DEFINES_H
#define KMS_MESSAGE_DEFINES_H


#ifdef _MSC_VER
#ifdef KMS_MSG_STATIC
#define KMS_MSG_API
#elif defined(KMS_MSG_COMPILATION)
#define KMS_MSG_API __declspec(dllexport)
#else
#define KMS_MSG_API __declspec(dllimport)
#endif
#define KMS_MSG_CALL __cdecl
#elif defined(__GNUC__)
#ifdef KMS_MSG_STATIC
#define KMS_MSG_API
#elif defined(KMS_MSG_COMPILATION)
#define KMS_MSG_API __attribute__ ((visibility ("default")))
#else
#define KMS_MSG_API
#endif
#define KMS_MSG_CALL
#endif

#define KMS_MSG_EXPORT(type) KMS_MSG_API type KMS_MSG_CALL

#ifdef __cplusplus
extern "C" {
#endif

KMS_MSG_EXPORT (int)
kms_message_init (void);
KMS_MSG_EXPORT (void)
kms_message_cleanup (void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#ifdef _MSC_VER
#include <basetsd.h>
#pragma warning(disable : 4142)
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#pragma warning(default : 4142)
#endif

#if defined(_MSC_VER)
#define KMS_MSG_INLINE __inline
#else
#define KMS_MSG_INLINE __inline__
#endif

#endif /* KMS_MESSAGE_DEFINES_H */
