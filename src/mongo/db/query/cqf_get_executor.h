/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

/**
 * Returns a PlanExecutor for the given Pipeline.
 *
 * The CanonicalQuery parameter allows for code reuse between functions in this file and should not
 * be set by callers.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getSBEExecutorViaCascadesOptimizer(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const CollectionPtr& collection,
    const boost::optional<BSONObj>& indexHint,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    std::unique_ptr<CanonicalQuery> = nullptr);

/**
 * Returns a PlanExecutor for the given CanonicalQuery.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getSBEExecutorViaCascadesOptimizer(
    const CollectionPtr& collection, std::unique_ptr<CanonicalQuery> query);

}  // namespace mongo
