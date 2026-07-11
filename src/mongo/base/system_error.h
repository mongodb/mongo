// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/util/modules.h"

#include <system_error>
#include <type_traits>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

const std::error_category& mongoErrorCategory();

// The next two functions are explicitly named (contrary to our naming style) so that they can be
// picked up by ADL.
std::error_code make_error_code(ErrorCodes::Error code);

std::error_condition make_error_condition(ErrorCodes::Error code);

}  // namespace mongo

namespace std {

/**
 * Allows a std::error_condition to be implicitly constructed from a mongo::ErrorCodes::Error.
 * We specialize this instead of is_error_code_enum as our ErrorCodes are platform independent.
 */
template <>
struct is_error_condition_enum<mongo::ErrorCodes::Error> : public true_type {};

}  // namespace std
