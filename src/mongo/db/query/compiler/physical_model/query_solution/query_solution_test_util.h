// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Make a minimal IndexEntry from just a key pattern. A dummy name will be added if none provided.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp, std::string name = "test_foo");

}  // namespace mongo
