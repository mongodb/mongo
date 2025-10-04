/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/shard_filterer.h"

#include <variant>

namespace mongo {

/**
 * This file contains types which describe the desired behavior of a PlanExecutor with respect to
 * shard filtering. Callers who are building a PlanExecutor can use these types to customize the
 * constructed plan tree, though it is expected that almost everyone should use the default
 * automatic shard filtering.
 */

/**
 * This struct represents the policy of "do the normal thing." This will use state set up on the
 * OperationContext to determine whether to add a ShardFilterer to the execution plan. In almost all
 * cases, this is what you want to use.
 */
struct AutomaticShardFiltering {};

/**
 * This type indicates that the consumer of the PlanExecutor does not want any shard filtering to
 * happen within the executor, since they have their own ShardFilterer which will perform shard
 * filtering outside of this PlanExecutor. This was built for the $search use case, but the key
 * rationale here is that the caller wants to have a ShardFilterer with a longer lifetime than this
 * PlanExecutor, which will hold a data placement snapshot for a longer period of time.
 */
struct ProofOfUpstreamFiltering {
    // This should be the only way to construct this type - where the caller provides proof that
    // they have a non-null ShardFilterer which they plan to use. The compiler can then help check
    // correctness and intentionality.
    ProofOfUpstreamFiltering(const ShardFilterer& proofOfManualFilter) {}
};

using ExecShardFilterPolicy = std::variant<AutomaticShardFiltering, ProofOfUpstreamFiltering>;

}  // namespace mongo
