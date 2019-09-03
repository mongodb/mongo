/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/lite_parsed_pipeline.h"

#include "mongo/db/operation_context.h"

namespace mongo {

void LiteParsedPipeline::assertSupportsReadConcern(
    OperationContext* opCtx,
    boost::optional<ExplainOptions::Verbosity> explain,
    bool enableMajorityReadConcern) const {
    auto readConcern = repl::ReadConcernArgs::get(opCtx);

    // Reject non change stream aggregation queries that try to use "majority" read concern when
    // enableMajorityReadConcern=false.
    if (!hasChangeStream() && !enableMajorityReadConcern &&
        (repl::ReadConcernArgs::get(opCtx).getLevel() ==
         repl::ReadConcernLevel::kMajorityReadConcern)) {
        uasserted(ErrorCodes::ReadConcernMajorityNotEnabled,
                  "Only change stream aggregation queries support 'majority' read concern when "
                  "enableMajorityReadConcern=false");
    }

    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Explain for the aggregate command cannot run with a readConcern "
                          << "other than 'local'. Current readConcern: " << readConcern.toString(),
            !explain || readConcern.getLevel() == repl::ReadConcernLevel::kLocalReadConcern);

    for (auto&& spec : _stageSpecs) {
        spec->assertSupportsReadConcern(readConcern);
    }
}

void LiteParsedPipeline::assertSupportsMultiDocumentTransaction(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "Operation not permitted in transaction :: caused by :: Explain for the aggregate "
            "command cannot run within a multi-document transaction",
            !explain);

    for (auto&& spec : _stageSpecs) {
        spec->assertSupportsMultiDocumentTransaction();
    }
}

bool LiteParsedPipeline::verifyIsSupported(
    OperationContext* opCtx,
    const std::function<bool(OperationContext*, const NamespaceString&)> isSharded,
    const boost::optional<ExplainOptions::Verbosity> explain,
    bool enableMajorityReadConcern) const {
    // Verify litePipe can be run in a transaction.
    if (opCtx->inMultiDocumentTransaction()) {
        assertSupportsMultiDocumentTransaction(explain);
    }
    // Verify litePipe can be run at the given read concern.
    assertSupportsReadConcern(opCtx, explain, enableMajorityReadConcern);
    // Verify that no involved namespace is sharded unless allowed by the pipeline.
    auto sharded = false;
    for (const auto& nss : getInvolvedNamespaces()) {
        sharded = isSharded(opCtx, nss);
        uassert(28769,
                str::stream() << nss.ns() << " cannot be sharded",
                allowShardedForeignCollection(nss) || !sharded);
    }
    return sharded;
}

}  // namespace mongo
