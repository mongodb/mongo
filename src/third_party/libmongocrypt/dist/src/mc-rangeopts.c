/*
 * Copyright 2022-present MongoDB, Inc.
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

#include "mc-rangeopts-private.h"

#include "mc-check-conversions-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-util-private.h" // mc_bson_type_to_string

/* Enable -Wconversion as error for only this file.
 * Other libmongocrypt files warn for -Wconversion. */
MC_BEGIN_CONVERSION_ERRORS

// Common logic for testing field name, tracking duplication, and presence.
#define IF_FIELD(Name, ErrorPrefix)                                            \
   if (0 == strcmp (field, #Name)) {                                           \
      if (has_##Name) {                                                        \
         CLIENT_ERR ("%sUnexpected duplicate field '" #Name "'", ErrorPrefix); \
         return false;                                                         \
      }                                                                        \
      has_##Name = true;

#define END_IF_FIELD \
   continue;         \
   }

#define CHECK_HAS(Name, ErrorPrefix)                           \
   if (!has_##Name) {                                          \
      CLIENT_ERR ("%sMissing field '" #Name "'", ErrorPrefix); \
      return false;                                            \
   }

bool
mc_RangeOpts_parse (mc_RangeOpts_t *ro,
                    const bson_t *in,
                    mongocrypt_status_t *status)
{
   bson_iter_t iter;
   bool has_min = false, has_max = false, has_sparsity = false;
   const char *const error_prefix = "Error parsing RangeOpts: ";

   BSON_ASSERT_PARAM (ro);
   BSON_ASSERT_PARAM (in);
   BSON_ASSERT (status || true);

   *ro = (mc_RangeOpts_t){0};
   ro->bson = bson_copy (in);

   if (!bson_iter_init (&iter, ro->bson)) {
      CLIENT_ERR ("%sInvalid BSON", error_prefix);
      return false;
   }

   while (bson_iter_next (&iter)) {
      const char *field = bson_iter_key (&iter);
      BSON_ASSERT (field);

      IF_FIELD (min, error_prefix)
      ro->min = iter;
      END_IF_FIELD

      IF_FIELD (max, error_prefix)
      ro->max = iter;
      END_IF_FIELD

      IF_FIELD (sparsity, error_prefix)
      if (!BSON_ITER_HOLDS_INT64 (&iter)) {
         CLIENT_ERR ("%sExpected int64 for sparsity, got: %s",
                     error_prefix,
                     mc_bson_type_to_string (bson_iter_type (&iter)));
         return false;
      };
      ro->sparsity = bson_iter_int64 (&iter);
      END_IF_FIELD

      CLIENT_ERR ("%sUnrecognized field: '%s'", error_prefix, field);
      return false;
   }

   CHECK_HAS (min, error_prefix);
   CHECK_HAS (max, error_prefix);
   CHECK_HAS (sparsity, error_prefix);

   return true;
}


bool
mc_RangeOpts_to_FLE2RangeInsertSpec (const mc_RangeOpts_t *ro,
                                     const bson_t *v,
                                     bson_t *out,
                                     mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (ro);
   BSON_ASSERT_PARAM (v);
   BSON_ASSERT_PARAM (out);
   BSON_ASSERT (status || true);

   const char *const error_prefix = "Error making FLE2RangeInsertSpec: ";
   bson_iter_t v_iter;
   if (!bson_iter_init_find (&v_iter, v, "v")) {
      CLIENT_ERR ("Unable to find 'v' in input");
      return false;
   }

   bson_t child;
   if (!BSON_APPEND_DOCUMENT_BEGIN (out, "v", &child)) {
      CLIENT_ERR ("%sError appending to BSON", error_prefix);
      return false;
   }
   if (!bson_append_iter (&child, "v", 1, &v_iter)) {
      CLIENT_ERR ("%sError appending to BSON", error_prefix);
      return false;
   }
   if (!bson_append_iter (&child, "min", 3, &ro->min)) {
      CLIENT_ERR ("%sError appending to BSON", error_prefix);
      return false;
   }
   if (!bson_append_iter (&child, "max", 3, &ro->max)) {
      CLIENT_ERR ("%sError appending to BSON", error_prefix);
      return false;
   }
   if (!bson_append_document_end (out, &child)) {
      CLIENT_ERR ("%sError appending to BSON", error_prefix);
      return false;
   }
   return true;
}

void
mc_RangeOpts_cleanup (mc_RangeOpts_t *ro)
{
   if (!ro) {
      return;
   }

   bson_destroy (ro->bson);
}

MC_END_CONVERSION_ERRORS
