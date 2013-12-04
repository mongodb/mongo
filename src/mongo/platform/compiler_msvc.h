/*
 * Copyright 2012 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Compiler-specific implementations for MSVC.
 *
 * Refer to mongo/platform/compiler.h for usage documentation.
 */

#pragma once

#define MONGO_COMPILER_NORETURN __declspec(noreturn)

#define MONGO_COMPILER_VARIABLE_UNUSED

#define MONGO_COMPILER_ALIGN_TYPE(ALIGNMENT) __declspec( align( ALIGNMENT ) )

#define MONGO_COMPILER_ALIGN_VARIABLE(ALIGNMENT) __declspec( align( ALIGNMENT ) )
