// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"

#include <cstring>
#include <memory>
#include <numeric>
#include <vector>

#include <grpcpp/impl/service_type.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/status.h>

namespace grpc {

template <>
class SerializationTraits<mongo::ConstSharedBuffer, void> {
public:
    static Status Serialize(const mongo::ConstSharedBuffer& source,
                            ByteBuffer* buffer,
                            bool* own_buffer) {
        // Make a shallow copy of the input buffer to increment its reference count and ensure the
        // data stays alive as long as the slice referencing it does.
        auto copy = std::make_unique<mongo::ConstSharedBuffer>(source);
        Slice slice{(void*)source.get(), source.capacity(), Destroy, copy.release()};
        ByteBuffer tmp{&slice, /* n_slices */ 1};
        buffer->Swap(&tmp);
        *own_buffer = true;
        return Status::OK;
    }

private:
    static void Destroy(void* data) {
        std::unique_ptr<mongo::ConstSharedBuffer> buf{static_cast<mongo::ConstSharedBuffer*>(data)};
    }
};

/**
 * (De)serialization implementations required to use SharedBuffer with streams provided by gRPC.
 * See: https://grpc.github.io/grpc/cpp/classgrpc_1_1_serialization_traits.html
 */
template <>
class SerializationTraits<mongo::SharedBuffer> {
public:
    static Status Deserialize(ByteBuffer* byte_buffer, mongo::SharedBuffer* dest) {
        const mongo::ScopeGuard freeByteBuffer([&]() { byte_buffer->Clear(); });
        Slice singleSlice;
        if (byte_buffer->TrySingleSlice(&singleSlice).ok()) {
            dest->realloc(singleSlice.size());
            std::memcpy(dest->get(), singleSlice.begin(), singleSlice.size());
            return Status::OK;
        }

        std::vector<Slice> slices;
        auto status = byte_buffer->Dump(&slices);

        if (!status.ok()) {
            return status;
        }

        size_t total =
            std::accumulate(slices.begin(), slices.end(), 0, [](size_t runningTotal, Slice& slice) {
                return runningTotal + slice.size();
            });
        dest->realloc(total);
        size_t index = 0;
        for (auto& s : slices) {
            std::memcpy(dest->get() + index, s.begin(), s.size());
            index += s.size();
        }
        return Status::OK;
    }
};
}  // namespace grpc
