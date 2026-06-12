/*
 * Copyright 2009-present MongoDB, Inc.
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

#include <bson/bson-prelude.h>


#ifndef BSON_STRING_H
#define BSON_STRING_H


#include <bson/bson-types.h>
#include <bson/macros.h>

#include <stdarg.h>


BSON_BEGIN_DECLS

BSON_EXPORT(char *)
bson_strdup(const char *str);

BSON_EXPORT(char *)
bson_strdup_printf(const char *format, ...) BSON_GNUC_PRINTF(1, 2);

BSON_EXPORT(char *)
bson_strdupv_printf(const char *format, va_list args) BSON_GNUC_PRINTF(1, 0);

BSON_EXPORT(char *)
bson_strndup(const char *str, size_t n_bytes);

BSON_EXPORT(void)
bson_strncpy(char *dst, const char *src, size_t size);

BSON_EXPORT(int)
bson_vsnprintf(char *str, size_t size, const char *format, va_list ap) BSON_GNUC_PRINTF(3, 0);

BSON_EXPORT(int)
bson_snprintf(char *str, size_t size, const char *format, ...) BSON_GNUC_PRINTF(3, 4);

BSON_EXPORT(void)
bson_strfreev(char **strv);

BSON_EXPORT(size_t)
bson_strnlen(const char *s, size_t maxlen);

BSON_EXPORT(int64_t)
bson_ascii_strtoll(const char *str, char **endptr, int base);

BSON_EXPORT(int)
bson_strcasecmp(const char *s1, const char *s2);

BSON_EXPORT(bool)
bson_isspace(int c);


BSON_END_DECLS


#endif /* BSON_STRING_H */
