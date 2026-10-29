// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <memory>

#include <kms_message/kms_message.h>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace kms_message_support_detail {

template <typename T>
using kms_message_deleter_func = void(T*);

template <typename T, kms_message_deleter_func<T> DelFunc>
struct KMSMessageDeleter {
    void operator()(T* ptr) {
        if (ptr) {
            DelFunc(ptr);
        }
    }
};

template <typename T, kms_message_deleter_func<T> DelFunc>
using kms_message_unique_ptr = std::unique_ptr<T, KMSMessageDeleter<T, DelFunc>>;

}  // namespace kms_message_support_detail

/**
 * Unique pointers to various types from the kms_message library.
 */
using UniqueKmsRequest =
    kms_message_support_detail::kms_message_unique_ptr<kms_request_t, kms_request_destroy>;
using UniqueKmsResponseParser =
    kms_message_support_detail::kms_message_unique_ptr<kms_response_parser_t,
                                                       kms_response_parser_destroy>;
using UniqueKmsRequestOpts =
    kms_message_support_detail::kms_message_unique_ptr<kms_request_opt_t, kms_request_opt_destroy>;
using UniqueKmsResponse =
    kms_message_support_detail::kms_message_unique_ptr<kms_response_t, kms_response_destroy>;
using UniqueKmsCharBuffer =
    kms_message_support_detail::kms_message_unique_ptr<char, kms_request_free_string>;

}  // namespace mongo
