/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>

#include "mongo/base/init.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/cqf_get_executor.h"
#include "mongo/db/query/multiple_collection_accessor.h"


namespace mongo::optimizer::fast_path {

#define REGISTER_FAST_PATH_EXEC_TREE_GENERATOR(key, pattern, generator) \
    MONGO_INITIALIZER(addFastPath_##key)                                \
    (InitializerContext*) {                                             \
        registerExecTreeGenerator(pattern, generator);                  \
    }

/**
 * Returns the arguments to create a PlanExecutor for the given CanonicalQuery.
 */
boost::optional<ExecParams> tryGetSBEExecutorViaFastPath(
    const MultipleCollectionAccessor& collections, const CanonicalQuery* query);

/**
 * Returns the arguments to create a PlanExecutor for the given Pipeline.
 *
 * The CanonicalQuery parameter allows for code reuse between functions in this file and should not
 * be set by callers.
 */
boost::optional<ExecParams> tryGetSBEExecutorViaFastPath(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const MultipleCollectionAccessor& collections,
    bool hasExplain,
    bool hasIndexHint,
    const Pipeline* pipeline,
    const CanonicalQuery* canonicalQuery = nullptr);
}  // namespace mongo::optimizer::fast_path
