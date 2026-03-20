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

#pragma once

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/engine_selection.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Returns 'false' for query plans that can not be executed in SBE.
 */
bool isPlanSbeEligible(const QuerySolution* solution);

/**
 * Returns the engine of choice for executing the specified query plan.
 */
EngineChoice engineSelectionForPlan(const QuerySolution* solution);

/**
 * Returns true iff 'keyPattern' has fields A and B where all of the following hold
 *
 *   - A is a path prefix of B
 *   - A is a hashed field in the index
 *   - B is a non-hashed field in the index
 *
 * TODO SERVER-99889 this is a workaround for an SBE stage builder bug.
 */
bool indexHasHashedPathPrefixOfNonHashedPath(const BSONObj& keyPattern);

}  // namespace mongo
