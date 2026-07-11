// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_type_endian.h"
#include "mongo/util/modules.h"

#include <cstdint>  // uint8_t

namespace mongo {
namespace sbe {
namespace vm {
/**
 * Reads directly from memory for the ByteCode VM.
 */
template <typename T>
T readFromMemory(const uint8_t* ptr) noexcept {
    static_assert(!IsEndian<T>::value);

    T val;
    memcpy(&val, ptr, sizeof(T));
    return val;
}

/**
 * Writes directly to memory for the ByteCode VM.
 */
template <typename T>
size_t writeToMemory(void* ptr, const T& val) noexcept {
    static_assert(!IsEndian<T>::value);

    memcpy(ptr, &val, sizeof(T));
    return sizeof(T);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
