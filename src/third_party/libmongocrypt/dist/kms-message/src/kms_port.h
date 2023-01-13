/*
 * Copyright 2018-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"){}
 *
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

#ifndef KMS_PORT_H
#define KMS_PORT_H

#include <stddef.h>

#if defined(_WIN32)
#define kms_strcasecmp _stricmp
char *
kms_strndup (const char *src, size_t len);
#else
#include <strings.h>

#define kms_strndup strndup
#define kms_strcasecmp strcasecmp
#endif

#endif /* KMS_PORT_H */
