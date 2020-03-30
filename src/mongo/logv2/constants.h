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

#include "mongo/base/string_data.h"
#include "mongo/logv2/log_truncation.h"

namespace mongo::logv2::constants {

// Used in data structures to indicate number of attributes to store without having to allocate
// memory.
constexpr size_t kNumStaticAttrs = 16;

// Field names used in the JSON and BSON formatter
constexpr StringData kTimestampFieldName = "t"_sd;
constexpr StringData kSeverityFieldName = "s"_sd;
constexpr StringData kComponentFieldName = "c"_sd;
constexpr StringData kContextFieldName = "ctx"_sd;
constexpr StringData kIdFieldName = "id"_sd;
constexpr StringData kMessageFieldName = "msg"_sd;
constexpr StringData kAttributesFieldName = "attr"_sd;
constexpr StringData kTruncatedFieldName = "truncated"_sd;
constexpr StringData kTruncatedSizeFieldName = "size"_sd;
constexpr StringData kTagsFieldName = "tags"_sd;

// String to be used when logging empty boost::optional with the text formatter
constexpr StringData kNullOptionalString = "(nothing)"_sd;

constexpr LogTruncation kDefaultTruncation = LogTruncation::Enabled;
constexpr int32_t kDefaultMaxAttributeOutputSizeKB = 10;

constexpr int32_t kUserAssertWithLogID = -1;

}  // namespace mongo::logv2::constants
