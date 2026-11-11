// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/db/query/stage_builder/sbe/type_signature.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {
/**
 * Returns an expression equal to getField(<makeObj>, field) that doesn't build the object, or none
 * if there isn't a cheap one.
 *
 * - 'makeObj' must be a makeObj() call with a constant spec.
 * - The result is a slot, a constant or a field of a slot, so reading it costs no more than the
 *   getField() it replaces.
 * - A set or added field is only read when 'inputType', the types the call's input may have, is
 *   provably an object.
 * - LambdaArg and MakeObj actions are not handled.
 * - makeBsonObj() calls are not handled, because they convert ArraySets to arrays.
 *   TODO SERVER-139018: Handle makeBsonObj() calls once sets never reach them.
 *
 * Results are not guaranteed to be the same when duplicate fields are present.
 */
boost::optional<abt::ABT> readFieldThroughMakeObj(const abt::ABT& makeObj,
                                                  std::string_view field,
                                                  TypeSignature inputType);
}  // namespace mongo::stage_builder
