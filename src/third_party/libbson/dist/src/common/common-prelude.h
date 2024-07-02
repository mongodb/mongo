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

#if !defined(MONGOC_INSIDE) && !defined(MONGOC_COMPILATION) && !defined(BSON_COMPILATION) && !defined(BSON_INSIDE)
#error "Only <mongoc/mongoc.h> or <bson/bson.h> can be included directly."
#endif

#define COMMON_NAME_1(a, b) COMMON_NAME_2 (a, b)
#define COMMON_NAME_2(a, b) a##_##b

#if defined(MCOMMON_NAME_PREFIX) && !defined(__INTELLISENSE__)
#define COMMON_NAME(Name) COMMON_NAME_1 (MCOMMON_NAME_PREFIX, Name)
#else
#define COMMON_NAME(Name) COMMON_NAME_1 (mcommon, Name)
#endif
