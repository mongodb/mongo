/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/traces/traceparent.h"

#include "mongo/base/error_codes.h"

#include <cstddef>

#include <fmt/format.h>

namespace mongo::otel::traces {
namespace {

// This is similar to IsValidHex from
// src/third_party/opentelemetry-cpp/api/include/opentelemetry/trace/propagation/detail/hex.h but
// that is an internal detail that is not guaranteed to be stable, so we write our own.

// Field layout for a version "00" W3C traceparent:
//   "<version>-<trace-id>-<parent-id>-<trace-flags>"
//      2     1    32     1    16     1     2          = 55 characters
constexpr std::size_t kVersionLen = 2;
constexpr std::size_t kTraceIdLen = 32;
constexpr std::size_t kParentIdLen = 16;
constexpr std::size_t kFlagsLen = 2;
constexpr std::size_t kTraceParentLen =
    kVersionLen + 1 + kTraceIdLen + 1 + kParentIdLen + 1 + kFlagsLen;

// Positions of the three '-' delimiters in a well-formed traceparent.
constexpr std::size_t kDelim1 = kVersionLen;
constexpr std::size_t kDelim2 = kVersionLen + 1 + kTraceIdLen;
constexpr std::size_t kDelim3 = kVersionLen + 1 + kTraceIdLen + 1 + kParentIdLen;

}  // namespace

Status validateW3CTraceparent(std::string_view value) {
    if (value.size() != kTraceParentLen) {
        return {ErrorCodes::BadValue,
                fmt::format("traceparent must be exactly {} characters, but got {}",
                            kTraceParentLen,
                            value.size())};
    }

    // Single pass: tokenize and validate each character in place. Delimiter positions must hold
    // '-'; every other position must be a lowercase hex digit. We also accumulate whether the
    // trace-id and parent-id fields are entirely zero, which the W3C spec forbids.
    auto traceIdAllZero = true;
    auto parentIdAllZero = true;
    for (std::size_t i = 0; i < kTraceParentLen; ++i) {
        const auto c = value[i];

        if (i == kDelim1 || i == kDelim2 || i == kDelim3) {
            if (c != '-') {
                return {ErrorCodes::BadValue,
                        "traceparent fields must be delimited by '-' in the form "
                        "\"<version>-<trace-id>-<parent-id>-<trace-flags>\""};
            }
            continue;
        }

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            if (i < kDelim1) {
                return {ErrorCodes::BadValue,
                        "traceparent version must be two lowercase hexadecimal digits"};
            }
            if (i < kDelim2) {
                return {ErrorCodes::BadValue,
                        "traceparent trace-id must be 32 lowercase hexadecimal digits"};
            }
            if (i < kDelim3) {
                return {ErrorCodes::BadValue,
                        "traceparent parent-id must be 16 lowercase hexadecimal digits"};
            }
            return {ErrorCodes::BadValue,
                    "traceparent trace-flags must be two lowercase hexadecimal digits"};
        }

        if (c != '0') {
            if (i > kDelim1 && i < kDelim2) {
                traceIdAllZero = false;
            } else if (i > kDelim2 && i < kDelim3) {
                parentIdAllZero = false;
            }
        }
    }

    if (value[0] == 'f' && value[1] == 'f') {
        return {ErrorCodes::BadValue, "traceparent version 'ff' is forbidden by the W3C spec"};
    }
    if (traceIdAllZero) {
        return {ErrorCodes::BadValue, "traceparent trace-id must not be all zeroes"};
    }
    if (parentIdAllZero) {
        return {ErrorCodes::BadValue, "traceparent parent-id must not be all zeroes"};
    }

    return Status::OK();
}

}  // namespace mongo::otel::traces
