// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
