// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_type.h"
#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <cstring>
#include <iosfwd>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

template <char C, typename T>
struct Terminated {
    Terminated() : value(DataType::defaultConstruct<T>()) {}
    Terminated(T value) : value(std::move(value)) {}
    T value;

    operator T() const {
        return value;
    }
};

struct TerminatedHelper {
    static Status makeLoadNoTerminalStatus(char c, size_t length, std::ptrdiff_t debug_offset);
    static Status makeLoadShortReadStatus(char c,
                                          size_t read,
                                          size_t length,
                                          std::ptrdiff_t debug_offset);
    static Status makeStoreStatus(char c, size_t length, std::ptrdiff_t debug_offset);
};

template <char C, typename T>
struct DataType::Handler<Terminated<C, T>> {
    using TerminatedType = Terminated<C, T>;

    static Status load(TerminatedType* tt,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        size_t local_advanced = 0;

        const char* end = static_cast<const char*>(std::memchr(ptr, C, length));

        if (!end) {
            return TerminatedHelper::makeLoadNoTerminalStatus(C, length, debug_offset);
        }

        auto status = DataType::load(
            tt ? &tt->value : nullptr, ptr, end - ptr, &local_advanced, debug_offset);

        if (!status.isOK()) {
            return status;
        }

        if (local_advanced != static_cast<size_t>(end - ptr)) {
            return TerminatedHelper::makeLoadShortReadStatus(
                C, local_advanced, end - ptr, debug_offset);
        }

        if (advanced) {
            *advanced = local_advanced + 1;
        }

        return Status::OK();
    }

    static Status store(const TerminatedType& tt,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset) {
        size_t local_advanced = 0;

        auto status = DataType::store(tt.value, ptr, length, &local_advanced, debug_offset);

        if (!status.isOK()) {
            return status;
        }

        if (length - local_advanced < 1) {
            return TerminatedHelper::makeStoreStatus(C, length, debug_offset + local_advanced);
        }

        if (ptr) {
            ptr[local_advanced] = C;
        }

        if (advanced) {
            *advanced = local_advanced + 1;
        }

        return Status::OK();
    }

    static TerminatedType defaultConstruct() {
        return TerminatedType();
    }
};

}  // namespace mongo
