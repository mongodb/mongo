/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef avro_Config_hh
#define avro_Config_hh

// Windows DLL support

#ifdef _WIN32
#pragma warning(disable : 4275 4251)

#if defined(AVRO_DYN_LINK)
#ifdef AVRO_SOURCE
#define AVRO_DECL __declspec(dllexport)
#else
#define AVRO_DECL __declspec(dllimport)
#endif // AVRO_SOURCE
#endif // AVRO_DYN_LINK

#include <intsafe.h>
using ssize_t = SSIZE_T;
#endif // _WIN32

#ifndef AVRO_DECL
#define AVRO_DECL
#endif

#endif
