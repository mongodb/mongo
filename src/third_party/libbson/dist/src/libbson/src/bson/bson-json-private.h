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

#ifndef BSON_JSON_PRIVATE_H
#define BSON_JSON_PRIVATE_H


struct _bson_json_opts_t {
   bson_json_mode_t mode;
   int32_t max_len;
   bool is_outermost_array;
};


#endif /* BSON_JSON_PRIVATE_H */
