// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <fmt/format.h>

/**
 * Helpers for the versioned document format shared by WindowFunctionState::serializeState() and
 * restoreState(). Every serialized state is a document whose kVersionField entry is an int format
 * version.
 */
namespace mongo::window_function_state {

constexpr std::string_view kVersionField = "v";

/**
 * Validates that 'state' is a document with an int kVersionField equal to 'expectedVersion' and
 * returns it.
 */
inline Document validateVersionedStateDoc(const Value& state,
                                          int expectedVersion,
                                          std::string_view owner) {
    tassert(13608304,
            fmt::format("Expected a document as the serialized {} state", owner),
            state.getType() == BSONType::object);
    auto doc = state.getDocument();
    auto version = doc[kVersionField];
    uassert(ErrorCodes::UnsupportedFormat,
            fmt::format("Unsupported {} serialized state version: {}", owner, version.toString()),
            version.getType() == BSONType::numberInt && version.getInt() == expectedVersion);
    return doc;
}

inline int getIntField(const Document& state, std::string_view fieldName) {
    auto value = state[fieldName];
    tassert(13608305,
            fmt::format("Expected field '{}' in a serialized window function state to be an int, "
                        "but found {}",
                        fieldName,
                        typeName(value.getType())),
            value.getType() == BSONType::numberInt);
    return value.getInt();
}

inline long long getLongField(const Document& state, std::string_view fieldName) {
    auto value = state[fieldName];
    tassert(13608306,
            fmt::format("Expected field '{}' in a serialized window function state to be a long, "
                        "but found {}",
                        fieldName,
                        typeName(value.getType())),
            value.getType() == BSONType::numberLong);
    return value.getLong();
}

}  // namespace mongo::window_function_state
