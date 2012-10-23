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

#pragma once

/**
 * Include "mongo/platform/cstdint.h" to get the C++11 cstdint types in namespace mongo.
 */

#if defined(_MSC_VER)
#include <cstdint>
#define _MONGO_STDINT_NAMESPACE std
#elif defined(__GNUC__)
#include <stdint.h>
#define _MONGO_STDINT_NAMESPACE
#else
#error "Unsupported compiler family"
#endif

namespace mongo {
    using _MONGO_STDINT_NAMESPACE::int8_t;
    using _MONGO_STDINT_NAMESPACE::int16_t;
    using _MONGO_STDINT_NAMESPACE::int32_t;
    using _MONGO_STDINT_NAMESPACE::int64_t;
    using _MONGO_STDINT_NAMESPACE::intptr_t;

    using _MONGO_STDINT_NAMESPACE::uint8_t;
    using _MONGO_STDINT_NAMESPACE::uint16_t;
    using _MONGO_STDINT_NAMESPACE::uint32_t;
    using _MONGO_STDINT_NAMESPACE::uint64_t;
    using _MONGO_STDINT_NAMESPACE::uintptr_t;
}  // namespace mongo

#undef _MONGO_STDINT_NAMESPACE
