// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/util/modules.h"


[[MONGO_MOD_PUBLIC]];
namespace mongo::CollationSpec {

constexpr const char* kSimpleBinaryComparison = "simple";

// Collation spec which the user can supply to represent the "simple" locale.
const static BSONObj kSimpleSpec =
    BSON(Collation::kLocaleFieldName << CollationSpec::kSimpleBinaryComparison);

}  // namespace mongo::CollationSpec
