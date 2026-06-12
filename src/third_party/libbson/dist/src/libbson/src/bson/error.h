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

#ifndef BSON_ERROR_T_INCLUDED
#define BSON_ERROR_T_INCLUDED

#include <bson/macros.h>

#include <stdint.h>

BSON_BEGIN_DECLS

#define BSON_ERROR_BUFFER_SIZE 503

BSON_ALIGNED_BEGIN(BSON_ALIGN_OF_PTR) // Aligned for backwards-compatibility.
typedef struct _bson_error_t {
   uint32_t domain;
   uint32_t code;
   char message[BSON_ERROR_BUFFER_SIZE];
   uint8_t reserved; // For internal use only!
} bson_error_t BSON_ALIGNED_END(BSON_ALIGN_OF_PTR);


BSON_STATIC_ASSERT2(error_t, sizeof(bson_error_t) == 512);

#define BSON_ERROR_JSON 1
#define BSON_ERROR_READER 2
#define BSON_ERROR_INVALID 3
#define BSON_ERROR_VECTOR 4

BSON_EXPORT(void)
bson_set_error(bson_error_t *error, uint32_t domain, uint32_t code, const char *format, ...) BSON_GNUC_PRINTF(4, 5);

BSON_EXPORT(char *)
bson_strerror_r(int err_code, char *buf, size_t buflen);

/**
 * @brief Reset the content of a bson_error_t to indicate no error.
 *
 * @param error Pointer to an error to be overwritten. If null, this function
 * has no effect.
 *
 * This is static-inline because it is trivially optimizable as a (conditional)
 * `memset`.
 */
static inline void
bson_error_clear(bson_error_t *error)
{
   if (!error) {
      return;
   }
   // Statically initialized to a zero struct:
   static bson_error_t zero_error;
   // Replace the caller's value:
   *error = zero_error;
}

/**
 * @brief Given a `bson_error_t` pointer l-value, ensure that it is non-null, and clear any
 * error value that it might hold.
 *
 * @param ErrorPointer An l-value expression of type `bson_error_t*`.
 *
 * If the passed pointer is null, then it will be updated to point to an anonymous
 * `bson_error_t` object that lives in the caller's scope.
 *
 * @note This macro is not valid in C++ because it relies on C99 compound literal semantics
 */
#define bson_error_reset(ErrorPointer) bson_error_reset(&(ErrorPointer), &(bson_error_t){0})
static inline void(bson_error_reset)(bson_error_t **error, bson_error_t *localptr)
{
   if (*error == NULL) {
      *error = localptr;
   }
   bson_error_clear(*error);
}

BSON_END_DECLS


#endif // BSON_ERROR_T_INCLUDED
