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

#include "mongo/logv2/log_truncation.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>

namespace mongo::logv2::constants {
using namespace std::literals::string_view_literals;

// Used in data structures to indicate number of attributes to store without having to allocate
// memory.
constexpr size_t kNumStaticAttrs = 16;

// Field names used in the JSON and BSON formatter
constexpr std::string_view kTimestampFieldName = "t"sv;
[[MONGO_MOD_NEEDS_REPLACEMENT]] constexpr std::string_view kSeverityFieldName = "s"sv;
constexpr std::string_view kComponentFieldName = "c"sv;
constexpr std::string_view kServiceFieldName = "svc"sv;
constexpr std::string_view kContextFieldName = "ctx"sv;
[[MONGO_MOD_NEEDS_REPLACEMENT]] constexpr std::string_view kIdFieldName = "id"sv;
constexpr std::string_view kMessageFieldName = "msg"sv;
constexpr std::string_view kAttributesFieldName = "attr"sv;
constexpr std::string_view kTruncatedFieldName = "truncated"sv;
constexpr std::string_view kOmittedFieldName = "omitted"sv;
constexpr std::string_view kTruncatedSizeFieldName = "size"sv;
constexpr std::string_view kTagsFieldName = "tags"sv;
constexpr std::string_view kTenantFieldName = "tenant"sv;

// String to be used when logging empty boost::optional with the text formatter
constexpr std::string_view kNullOptionalString = "(nothing)"sv;

constexpr LogTruncation kDefaultTruncation = LogTruncation::Enabled;
[[MONGO_MOD_NEEDS_REPLACEMENT]] constexpr int32_t kDefaultMaxAttributeOutputSizeKB = 10;

constexpr int32_t kUserAssertWithLogID = -1;

}  // namespace mongo::logv2::constants
