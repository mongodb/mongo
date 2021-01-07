/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <kms_message/kms_message.h>
#include <memory>

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
