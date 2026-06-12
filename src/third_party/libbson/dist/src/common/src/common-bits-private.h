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

#include <common-prelude.h>

#ifndef MONGO_C_DRIVER_COMMON_BITS_PRIVATE_H
#define MONGO_C_DRIVER_COMMON_BITS_PRIVATE_H

#include <bson/bson.h>


// Round up to the next power of two uint32_t value. Saturates on overflow.
static BSON_INLINE uint32_t
mcommon_next_power_of_two_u32(uint32_t v)
{
   if (v == 0) {
      return 1;
   }

   // https://graphics.stanford.edu/%7Eseander/bithacks.html#RoundUpPowerOf2
   v--;
   v |= v >> 1;
   v |= v >> 2;
   v |= v >> 4;
   v |= v >> 8;
   v |= v >> 16;
   v++;

   if (v == 0) {
      return UINT32_MAX;
   } else {
      return v;
   }
}


#endif /* MONGO_C_DRIVER_COMMON_BITS_PRIVATE_H */
