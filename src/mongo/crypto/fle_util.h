// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/mongocrypt_definitions.h"
#include "mongo/util/modules.h"

#include <memory>

// Shared utility used by various pieces of the FLE code.
namespace mongo {

namespace libmongocrypt_support_detail {

template <typename T>
using libmongocrypt_deleter_func = void(T*);

template <typename T, libmongocrypt_deleter_func<T> DelFunc>
struct LibMongoCryptDeleter {
    void operator()(T* ptr) {
        if (ptr) {
            DelFunc(ptr);
        }
    }
};

template <typename T, libmongocrypt_deleter_func<T> DelFunc>
using libmongocrypt_unique_ptr = std::unique_ptr<T, LibMongoCryptDeleter<T, DelFunc>>;

}  // namespace libmongocrypt_support_detail

_mongocrypt_t* getGlobalMongoCrypt();

}  // namespace mongo
