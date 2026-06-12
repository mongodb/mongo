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

#ifndef BSON_ERROR_PRIVATE_H
#define BSON_ERROR_PRIVATE_H

#include <bson/error.h> // IWYU pragma: export

//

#include <bson/macros.h>


#define BSON_ERROR_CATEGORY 1


static BSON_INLINE void
bson_set_error_category(bson_error_t *error, uint8_t category)
{
   BSON_ASSERT_PARAM(error);
   error->reserved = category;
}

#endif /* BSON_ERROR_PRIVATE_H */
