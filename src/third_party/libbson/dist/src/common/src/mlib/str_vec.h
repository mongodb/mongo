/**
 * @file str_vec.h
 * @brief This file defines mstr_vec, a common "array of strings" type
 * @date 2025-09-30
 *
 * @copyright Copyright 2009-present MongoDB, Inc.
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
#ifndef MLIB_STR_VEC_H_INCLUDED
#define MLIB_STR_VEC_H_INCLUDED

#include <mlib/config.h>
#include <mlib/str.h>

#define T mstr
#define VecDestroyElement(Ptr) (mstr_destroy(Ptr))
#define VecCopyElement(Dst, Src) (*Dst = mstr_copy(*Src), Dst->data != NULL)
#include <mlib/vec.th>

#endif // MLIB_STR_VEC_H_INCLUDED
