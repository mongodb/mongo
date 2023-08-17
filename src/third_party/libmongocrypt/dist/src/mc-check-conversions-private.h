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

#ifndef MC_CHECK_CONVERSIONS_PRIVATE
#define MC_CHECK_CONVERSIONS_PRIVATE

// gcc 4.6 added support for "diagnostic push".
#if (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#define MC_BEGIN_CONVERSION_IGNORE _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wconversion\"")
#define MC_END_CONVERSION_IGNORE _Pragma("GCC diagnostic pop")
#elif defined(__clang__)
#define MC_BEGIN_CONVERSION_IGNORE _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wconversion\"")
#define MC_END_CONVERSION_IGNORE _Pragma("clang diagnostic pop")
#else
#define MC_BEGIN_CONVERSION_IGNORE
#define MC_END_CONVERSION_IGNORE
#endif

#endif /* MC_CHECK_CONVERSIONS_PRIVATE */
