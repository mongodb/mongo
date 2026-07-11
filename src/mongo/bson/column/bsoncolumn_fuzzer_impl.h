// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo::bsoncolumn {
// Fuzzes the provided binary as BSONColumn data. Binary must have passed BSONColumn structural
// validation and be memory safe to parse. Will invariant if our different BSONColumn
// implementations are not behaving identical.
void fuzzer(const char* binary, size_t size);
}  // namespace mongo::bsoncolumn
