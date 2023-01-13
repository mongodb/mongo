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

#ifndef MC_OPTIONAL_PRIVATE_H
#define MC_OPTIONAL_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
   bool set;
   int32_t value;
} mc_optional_int32_t;

#define OPT_I32(val)            \
   (mc_optional_int32_t)        \
   {                            \
      .set = true, .value = val \
   }

#define OPT_I32_C(val)          \
   {                            \
      .set = true, .value = val \
   }

typedef struct {
   bool set;
   uint32_t value;
} mc_optional_uint32_t;

#define OPT_U32(val)            \
   (mc_optional_uint32_t)       \
   {                            \
      .set = true, .value = val \
   }

#define OPT_U32_C(val)          \
   {                            \
      .set = true, .value = val \
   }

typedef struct {
   bool set;
   int64_t value;
} mc_optional_int64_t;

#define OPT_I64(val)            \
   (mc_optional_int64_t)        \
   {                            \
      .set = true, .value = val \
   }

#define OPT_I64_C(val)          \
   {                            \
      .set = true, .value = val \
   }

typedef struct {
   bool set;
   uint64_t value;
} mc_optional_uint64_t;

#define OPT_U64(val)            \
   (mc_optional_uint64_t)       \
   {                            \
      .set = true, .value = val \
   }

#define OPT_U64_C(val)          \
   {                            \
      .set = true, .value = val \
   }

typedef struct {
   bool set;
   double value;
} mc_optional_double_t;

#define OPT_DOUBLE(val)         \
   (mc_optional_double_t)       \
   {                            \
      .set = true, .value = val \
   }

#define OPT_DOUBLE_C(val)       \
   {                            \
      .set = true, .value = val \
   }

#endif /* MC_OPTIONAL_PRIVATE_H */
