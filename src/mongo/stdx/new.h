/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/config.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <new>

namespace MONGO_MOD_PUB mongo {
namespace stdx {

// libc++ 8.0 and later define __cpp_lib_hardware_interference_size but don't actually implement it
#if __cplusplus < 201703L || \
    !(defined(__cpp_lib_hardware_interference_size) && !defined(_LIBCPP_VERSION))

#if defined(MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT)
static_assert(MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT >= sizeof(std::uint64_t),
              "Bad extended alignment");
constexpr std::size_t hardware_destructive_interference_size = MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT;
#else
constexpr std::size_t hardware_destructive_interference_size = alignof(std::max_align_t);
#endif

constexpr auto hardware_constructive_interference_size = hardware_destructive_interference_size;

#else

using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;

#endif  // hardware_interference_size

#if __cpp_lib_launder >= 201606
using std::launder;
#else
template <typename T>
[[nodiscard]] constexpr T* launder(T* p) noexcept {
    return p;
}
#endif  // launder

}  // namespace stdx
}  // namespace MONGO_MOD_PUB mongo
